/*
 * gt_gate.c - Guardian-TRON gatekeeper task (μT-Kernel 3.0 / STM32N6570-DK)
 *
 *   Jetson --USART2 115200--> [15-byte CMD] -> gk_core_process() -> [12-byte VERDICT] --> Jetson
 *                                                    |
 *                                       applied cmd -> VESC (USART3 38400): servo + erpm
 *
 * Only this task ever commands VESC while driving. Independent of the Jetson it also:
 *   - stops the car when no valid CMD arrived for GT_WATCHDOG_MS (frozen Jetson / cut cable)
 *   - stops the car when the local safety monitor (camera/NPU/ToF) reports a hazard
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "stm32n6xx_hal.h"
#include "gt_uart.h"
#include "gt_vesc.h"
#include "gt_gate.h"
#include "protocol.h"
#include "gatekeeper_core.h"

/* ---- car2 actuator calibration (from the Jetson F1TENTH vesc.yaml, verified on the stand) ---- */
#define SERVO_CENTER		0.423f		/* straight */
#define SERVO_GAIN		(-0.89f)	/* servo units per rad; lower value = LEFT */
#define SERVO_MIN		0.15f
#define SERVO_MAX		0.85f
#define ERPM_PER_MPS		(-4403.0f)	/* forward = negative erpm */
#define DEG2RAD			0.017453292f

#define GT_WATCHDOG_MS		200
#define GT_BRAKE_AMPS		5.0f
#define GT_STOP_SPEED		0.03f		/* |v| below this -> hold with rpm 0 */

LOCAL gk_core_state_t	gk;
LOCAL gt_gate_status_t	st;

/* ---- microsecond clock from the Cortex-M55 cycle counter (kernel owns SysTick) ---- */
LOCAL void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
LOCAL UW now_us(void) { return (UW)((UD)DWT->CYCCNT * 1000000ULL / SystemCoreClock); }
LOCAL UW now_ms(void) { SYSTIM t; tk_get_tim(&t); return t.lo; }

/* ---- actuator ---- */
LOCAL void apply_cmd(const gk_cmd_t *c)
{
	float servo = SERVO_CENTER + SERVO_GAIN * (c->steer_deg * DEG2RAD);
	if (servo < SERVO_MIN) servo = SERVO_MIN;
	if (servo > SERVO_MAX) servo = SERVO_MAX;
	gt_vesc_set_servo(servo);

	float v = c->accel_mps2;			/* = target speed [m/s], see safety_envelope.h */
	if (v > -GT_STOP_SPEED && v < GT_STOP_SPEED) gt_vesc_set_rpm(0);
	else gt_vesc_set_rpm((W)(v * ERPM_PER_MPS));
	st.servo = servo; st.speed = v;
}

LOCAL void emergency_stop(const char *why)
{
	gt_vesc_set_servo(SERVO_CENTER);
	gt_vesc_set_rpm(0);
	gt_vesc_set_brake(GT_BRAKE_AMPS);
	st.servo = SERVO_CENTER; st.speed = 0.0f;
	if (!st.stopped) tm_printf((UB*)"[gate] STOP: %s\n", why);
	st.stopped = 1;
}

EXPORT const gt_gate_status_t *gt_gate_status(void) { return &st; }

/* ---- task ---- */
EXPORT void gt_gate_task(INT stacd, void *exinf)
{
	UB   rx[64], frame[GK_CMD_FRAME_LEN];
	INT  have = 0;
	UW   t_rx = 0, last_cmd_ms = 0, last_report_ms = 0, last_alive_ms = 0;

	dwt_init();
	gk_core_init(&gk);
	gt_uart_init(GT_UART_JETSON, 115200);
	gt_uart_puts(GT_UART_JETSON, "\r\nGT:BOOT gatekeeper microT-Kernel 3.0 / STM32N657\r\n");
	st.stopped = 1;					/* nothing moves until the first approved CMD */
	last_cmd_ms = now_ms();

	while (1) {
		INT n = gt_uart_read(GT_UART_JETSON, rx, sizeof rx, 20);
		for (INT i = 0; i < n; i++) {
			UB c = rx[i];
			if (have == 0) {
				if (c != GK_CMD_STX) continue;	/* resync on STX */
				t_rx = now_us();
			}
			frame[have++] = c;
			if (have < GK_CMD_FRAME_LEN) continue;

			if (frame[GK_CMD_FRAME_LEN - 1] != GK_ETX) {	/* lost framing: slide by one */
				for (INT k = 1; k < GK_CMD_FRAME_LEN; k++) frame[k - 1] = frame[k];
				have = GK_CMD_FRAME_LEN - 1;
				while (have > 0 && frame[0] != GK_CMD_STX) {
					for (INT k = 1; k < have; k++) frame[k - 1] = frame[k];
					have--;
				}
				continue;
			}

			gk_verdict_frame_t resp; gk_cmd_t applied;
			gk_core_process(&gk, frame, t_rx, now_us(), &resp, &applied);
			have = 0;

			gt_local_safety_t ls = gt_local_safety_poll();
			if (ls.hazard) {
				resp.verdict = GK_VERDICT_VETO;	/* Jetson learns that STM32 overrode it */
				emergency_stop("local safety monitor");
			} else if (resp.verdict != GK_VERDICT_MALFORMED) {
				apply_cmd(&applied);
				st.stopped = 0;
				last_cmd_ms = now_ms();
			}

			UB out[GK_VERDICT_FRAME_LEN];
			gk_encode_verdict(&resp, out);
			gt_uart_write(GT_UART_JETSON, out, sizeof out);
			st.last_verdict = resp.verdict; st.last_latency_us = resp.latency_us;
		}

		UW ms = now_ms();
		if (!st.stopped && ms - last_cmd_ms > GT_WATCHDOG_MS) emergency_stop("Jetson link timeout");
		if (st.stopped && ms - last_alive_ms >= 100) {		/* keep VESC braking while stopped */
			gt_vesc_set_rpm(0); last_alive_ms = ms;
		}
		if (ms - last_report_ms >= 1000) {
			last_report_ms = ms;
			tm_printf((UB*)"[gate] n=%d ok=%d veto=%d bad=%d replay=%d maxlat=%dus | servo=%d v=%d mm/s %s\n",
				  (INT)gk.n_total, (INT)gk.n_approved, (INT)gk.n_veto, (INT)gk.n_malformed,
				  (INT)gk.n_replay_or_reorder, (INT)gk.max_latency_us,
				  (INT)(st.servo * 1000), (INT)(st.speed * 1000), st.stopped ? "STOPPED" : "DRIVING");
		}
	}
}

/* ---- local safety monitor: camera/NPU + ToF land here (stub: no hazard) ---- */
__attribute__((weak)) gt_local_safety_t gt_local_safety_poll(void)
{
	gt_local_safety_t r = { .hazard = 0, .collision_probability = 0.0f };
	return r;
}
