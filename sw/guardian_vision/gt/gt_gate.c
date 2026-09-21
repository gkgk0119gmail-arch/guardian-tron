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
#include <kernel.h>		/* TCB access for the vision stall probe */
#include "../mtk3_bsp2/mtkernel/kernel/tkernel/task.h"
#include <tm/tmonitor.h>
#include <math.h>
#include "stm32n6xx_hal.h"
#include "gt_uart.h"
#include "gt_vesc.h"
#include "gt_gate.h"
#include "protocol.h"
#include "gatekeeper_core.h"
#include "gv_vision.h"
#include "../os/gv_perf.h"
IMPORT ID gv_vision_tid;
IMPORT volatile UW gv_irq_csi, gv_irq_dcmipp, gt_irq_usart2;
IMPORT volatile UW gv_mpu_violations, gv_mpu_fault_addr, gv_mpu_fault_pc, gv_mpu_fault_task;
IMPORT volatile float gv_imu_roll, gv_imu_pitch;

/* ---- car2 actuator calibration (from the Jetson F1TENTH vesc.yaml, verified on the stand) ---- */
#define SERVO_CENTER		0.423f		/* straight */
#define SERVO_GAIN		(-0.89f)	/* servo units per rad; lower value = LEFT */
#define SERVO_MIN		0.15f
#define SERVO_MAX		0.85f
#define ERPM_PER_MPS		(-4403.0f)	/* forward = negative erpm */
#define DEG2RAD			0.017453292f

#define GT_WATCHDOG_MS		200
#define GT_VISION_TIMEOUT_MS	500		/* camera monitor must deliver a frame this often */
#define GT_BRAKE_AMPS		5.0f
#define GT_STOP_SPEED		0.03f		/* |v| below this -> hold with rpm 0 */
#define GT_MPU_HOLD_MS		3000		/* car held after an MPU violation */

/* ---- person distance governor ----
 * Monocular range from the person box: d = GT_DIST_K / h (pinhole; h = box height /
 * frame height). The gate caps the Jetson's speed so the car can always stop before the
 * hard-stop line: v_cap = sqrt(2 * a_plan * (d - d_stop)), d_stop = K / GV_MIN_BOX_H.
 * GT_DIST_K depends on the target's real height and the lens (calibrate: K = h * d). */
#define GT_DIST_K		0.84f		/* m: real person, box h 0.42 at 2.0 m (measured 2026-09-21) */
#define GT_STOP_BOX_H		0.42f		/* = GV_MIN_BOX_H in gv_task.c: hard stop at ~2.0 m */
#define GT_PLAN_DECEL		0.10f		/* m/s^2: cap 0.5 m/s from ~3.25 m, 0.3 m/s at ~2.45 m */
#define GT_GOV_MIN_H		0.12f		/* beyond ~7 m: too far / too noisy to govern */
#define GT_GOV_STALE_MS		1000		/* hold the cap across detection flicker */

/* gatekeeper private state: writable only while this task runs (MPU region 0, gv_mpu.c) */
LOCAL gk_core_state_t	gk __attribute__((section(".bss.gate_priv")));
LOCAL gt_gate_status_t	st __attribute__((section(".bss.gate_priv")));
EXPORT void *gt_gate_private_addr(void) { return &gk; }

/* ---- microsecond clock from the Cortex-M55 cycle counter (kernel owns SysTick) ---- */
LOCAL UD cyc64; LOCAL UW cyc_last;	/* CYCCNT wraps every 5.4 s at 800 MHz: extend it (gate task only) */
LOCAL void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;		/* already running (gv_perf_init); never reset it */
	cyc_last = DWT->CYCCNT;
}
LOCAL UW now_us(void)
{
	UW c = DWT->CYCCNT; cyc64 += (UW)(c - cyc_last); cyc_last = c;
	return (UW)(cyc64 / (SystemCoreClock / 1000000U));
}
LOCAL UW now_ms(void) { SYSTIM t; tk_get_tim(&t); return t.lo; }

/* ---- actuator ---- */
/* VESC link is 38400 baud (~260 us/byte): servo + rpm frames cost ~4.7 ms. Only send when
 * the target changed or every GT_VESC_REFRESH_MS, so the gate keeps up with 100 Hz CMDs. */
#define GT_VESC_REFRESH_MS	50
LOCAL float sent_servo = -1.0f; LOCAL W sent_erpm = 0x7FFFFFFF; LOCAL UW sent_ms;
LOCAL void apply_cmd(const gk_cmd_t *c)
{
	float servo = SERVO_CENTER + SERVO_GAIN * (c->steer_deg * DEG2RAD);
	if (servo < SERVO_MIN) servo = SERVO_MIN;
	if (servo > SERVO_MAX) servo = SERVO_MAX;
	float v = c->accel_mps2;			/* = target speed [m/s], see safety_envelope.h */
	W erpm = (v > -GT_STOP_SPEED && v < GT_STOP_SPEED) ? 0 : (W)(v * ERPM_PER_MPS);
	UW ms = now_ms();
	float ds = servo - sent_servo; if (ds < 0) ds = -ds;
	W de = erpm - sent_erpm; if (de < 0) de = -de;
	INT refresh = (ms - sent_ms) >= GT_VESC_REFRESH_MS;
	if (ds > 0.002f || refresh) { gt_vesc_set_servo(servo); sent_servo = servo; }
	if (de > 30 || refresh)     { gt_vesc_set_rpm(erpm); sent_erpm = erpm; }
	if (refresh) sent_ms = ms;
	st.servo = servo; st.speed = v;
}

LOCAL void emergency_stop(const char *why)
{
	/* brake frame first: at 38400 baud every VESC frame costs ~2.6 ms on the wire */
	gt_vesc_set_brake(GT_BRAKE_AMPS);
	gt_vesc_set_servo(SERVO_CENTER);
	st.servo = SERVO_CENTER; st.speed = 0.0f;
	sent_servo = -1.0f; sent_erpm = 0x7FFFFFFF;
	if (!st.stopped) tm_printf((UB*)"[gate] STOP: %s\n", why);
	st.stopped = 1;
}

/* camera/NPU hazard onset: brake now and log how long every stage took */
LOCAL void hazard_stop(const gt_local_safety_t *ls)
{
	UW c_start = DWT->CYCCNT;
	gv_probe_set(GV_PROBE_HAZARD, 1);
	if (ls->source == GT_SRC_CAMERA_LOST) { emergency_stop("camera monitor lost"); return; }
	if (ls->source == GT_SRC_MPU) {
		emergency_stop("MPU violation");
		tm_printf((UB*)"[gate] STOP: MPU blocked a write to gatekeeper memory by task %d (addr %x, pc %x) -> car held %d ms\n",
			  (INT)gv_mpu_fault_task, (UW)gv_mpu_fault_addr, (UW)gv_mpu_fault_pc, GT_MPU_HOLD_MS);
		return;
	}
	gt_vesc_set_brake(GT_BRAKE_AMPS);		/* the frame that actually stops the car */
	UW c_brake = DWT->CYCCNT;
	gt_vesc_set_servo(SERVO_CENTER);
	st.servo = SERVO_CENTER; st.speed = 0.0f;
	sent_servo = -1.0f; sent_erpm = 0x7FFFFFFF;
	st.stopped = 1;
	UW mhz = SystemCoreClock / 1000000U;
	if (c_start - ls->detect_cyc > 100U * 1000U * mhz) {	/* raised while another hazard held the car */
		tm_printf((UB*)"[gate] STOP: %s hazard still active after the previous one cleared -> car stays held\n",
			  ls->source == GT_SRC_PERSON ? "person" : "IMU");
		return;
	}
	gv_stat_add(&gv_perf_hazard2brake, c_start - ls->detect_cyc);
	gv_metric_add(GV_M_HAZ2BRAKE, GV_CYC2NS(c_start - ls->detect_cyc));
	if (ls->source == GT_SRC_IMPACT || ls->source == GT_SRC_TILT) {
		tm_printf((UB*)"[gate] STOP: IMU %s (%d%s) | sample->detect %d us | detect->gate %d us | brake frame queued in %d ns (+2.6 ms on the 38400 wire)\n",
			  ls->source == GT_SRC_IMPACT ? "IMPACT" : "TILT",
			  ls->source == GT_SRC_IMPACT ? (INT)(ls->box_h * 1000) : (INT)ls->box_h,
			  ls->source == GT_SRC_IMPACT ? " mg" : " deg",
			  (INT)((ls->detect_cyc - ls->frame_cyc) / mhz), (INT)((c_start - ls->detect_cyc) / mhz),
			  (INT)((c_brake - c_start) * 1000 / mhz));
		return;
	}
	tm_printf((UB*)"[gate] STOP #%d: PERSON AHEAD (conf %d%%, h %d%%) | frame->detect %d us | detect->gate %d us | brake frame queued in %d ns (+2.6 ms on the 38400 wire)\n",
		  (INT)ls->event_id, (INT)(ls->collision_probability * 100), (INT)(ls->box_h * 100),
		  (INT)((ls->detect_cyc - ls->frame_cyc) / mhz), (INT)((c_start - ls->detect_cyc) / mhz),
		  (INT)((c_brake - c_start) * 1000 / mhz));
}

EXPORT void gt_gate_kick(void) { gt_uart_kick(GT_UART_JETSON); }

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

	UW hazard_event = 0, link_lost_ms = 0, last_rx_ms = 0;
	INT link_up = 0;
	INT hazard_active = 0;

	UW loops = 0, p_loops = 0, p_csi = 0, p_dcm = 0, p_u2 = 0;
	while (1) {
		loops++;
		INT n = gt_uart_read(GT_UART_JETSON, rx, sizeof rx, 20);

		/* local safety (camera/NPU) has priority over anything the Jetson says */
		gt_local_safety_t lsw = gt_local_safety_poll();

		/* fail-safe: no camera frames for GT_VISION_TIMEOUT_MS (ribbon loose, sensor dead,
		 * task stuck) means the person monitor is blind -> hold the car */
		{
			static UW vf_last, vf_ms; static INT blind = -1;
			UW now = now_ms();
			if (gv_frames != vf_last) { vf_last = gv_frames; vf_ms = now; }
			INT is_blind = !gv_ready || (now - vf_ms) > GT_VISION_TIMEOUT_MS;
			if (is_blind != blind) {
				blind = is_blind;
				tm_printf((UB*)"[gate] camera monitor %s\n", blind ? "LOST -> holding the car" : "OK -> commands accepted");
			}
			if (blind) { lsw.hazard = 1; lsw.source = GT_SRC_CAMERA_LOST; lsw.event_id = 0xFFFFFFFFU; }
		}
		/* IMU impact / tilt monitor */
		{
			static UW imu_noted;
			gt_local_safety_t im = gv_imu_safety_poll();
			if (im.hazard && !lsw.hazard) { lsw = im; lsw.event_id |= 0x40000000U; imu_noted = im.event_id; }
			else if (im.hazard && im.event_id != imu_noted) {	/* car already held by another hazard */
				imu_noted = im.event_id;
				tm_printf((UB*)"[gate] IMU %s while already stopped (%d)\n",
					  im.source == GT_SRC_IMPACT ? "IMPACT" : "TILT",
					  im.source == GT_SRC_IMPACT ? (INT)(im.box_h * 1000) : (INT)im.box_h);
			}
		}
		/* MPU violation (a task tried to write gatekeeper memory): hold the car */
		{
			static UW mpu_seen, mpu_until;
			if (gv_mpu_violations != mpu_seen) { mpu_seen = gv_mpu_violations; mpu_until = now_ms() + GT_MPU_HOLD_MS; }
			if (mpu_seen && (W)(mpu_until - now_ms()) > 0) {
				lsw.hazard = 1; lsw.source = GT_SRC_MPU; lsw.event_id = 0x80000000U | mpu_seen;
			}
		}
		if (lsw.hazard) {
			if (!hazard_active || lsw.event_id != hazard_event) {
				hazard_stop(&lsw);
				hazard_active = 1; hazard_event = lsw.event_id;
			}
		} else if (hazard_active) {
			hazard_active = 0;
			gv_probe_set(GV_PROBE_HAZARD, 0);
			tm_printf((UB*)"[gate] hazard clear -> accepting Jetson commands again\n");
		}
		{	/* interrupt -> task latency of the blocking read above */
			UW l = gt_uart_take_wake_latency(GT_UART_JETSON);
			if (l) { gv_stat_add(&gv_perf_irq2task, l); gv_metric_add(GV_M_IRQ2TASK, GV_CYC2NS(l)); }
		}
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

			UW c_cmd = DWT->CYCCNT;
			gv_probe_set(GV_PROBE_GATE, 1);
			gk_verdict_frame_t resp; gk_cmd_t applied;
			gk_core_process(&gk, frame, t_rx, now_us(), &resp, &applied);
			have = 0;
			if (resp.verdict != GK_VERDICT_MALFORMED) {
				last_rx_ms = now_ms();
				if (!link_up) {
					link_up = 1;
					if (link_lost_ms)
						tm_printf((UB*)"[fault] Jetson link restored after %d ms -> commands accepted again\n", (INT)(last_rx_ms - link_lost_ms));
					else
						tm_printf((UB*)"[gate] Jetson link up\n");
					link_lost_ms = 0;
				}
			}

			/* person distance governor: cap the speed so the car can stop before the line */
			if (!lsw.hazard && resp.verdict != GK_VERDICT_MALFORMED) {
				static INT gov_on; static float gov_last;
				float h = gv_person_h, cap = 1e9f;
				if (h >= GT_GOV_MIN_H && (now_ms() - gv_person_ms) < GT_GOV_STALE_MS) {
					float d = GT_DIST_K / h, d_stop = GT_DIST_K / GT_STOP_BOX_H;
					float room = d - d_stop; if (room < 0) room = 0;
					cap = sqrtf(2.0f * GT_PLAN_DECEL * room);
				}
				if (applied.accel_mps2 > cap) {
					applied.accel_mps2 = cap;
					gk.env.last_safe.accel_mps2 = cap;
					resp.verdict = GK_VERDICT_VETO;
					if (!gov_on || gov_last - cap > 0.10f || cap - gov_last > 0.10f) {
						tm_printf((UB*)"[gate] SLOW: person ~%d cm ahead (box h %d%%) -> speed cap %d mm/s\n",
							  (INT)(GT_DIST_K / h * 100), (INT)(h * 100), (INT)(cap * 1000));
						gov_last = cap;
					}
					gov_on = 1;
				} else if (gov_on && cap > 1e8f) {
					gov_on = 0;
					tm_printf((UB*)"[gate] SLOW released: no person ahead\n");
				}
			}

			if (lsw.hazard) {
				resp.verdict = GK_VERDICT_VETO;	/* Jetson learns that STM32 overrode it */
				/* the car is held still: make the envelope ramp up from 0 when the hazard clears */
				gk.env.last_safe.accel_mps2 = 0.0f;
				gk.env.last_safe.steer_deg = 0.0f;
			} else if (resp.verdict != GK_VERDICT_MALFORMED) {
				apply_cmd(&applied);
				st.stopped = 0;
				last_cmd_ms = now_ms();
			}

			UB out[GK_VERDICT_FRAME_LEN];
			gk_encode_verdict(&resp, out);
			gt_uart_write(GT_UART_JETSON, out, sizeof out);
			st.last_verdict = resp.verdict; st.last_latency_us = resp.latency_us;
			gv_probe_set(GV_PROBE_GATE, 0);
			gv_stat_add(&gv_perf_gate_cmd, DWT->CYCCNT - c_cmd);
			gv_metric_add(GV_M_GATE_CMD, GV_CYC2NS(DWT->CYCCNT - c_cmd));
		}

		UW ms = now_ms();
		/* Jetson link watchdog: no valid CMD for GT_WATCHDOG_MS, whatever the car is doing */
		if (link_up && ms - last_rx_ms > GT_WATCHDOG_MS) {
			UW lost_ms = ms - last_rx_ms;
			link_up = 0; link_lost_ms = ms;
			gv_metric_add(GV_M_LINK_LOSS, lost_ms * 1000000U);
			if (!st.stopped) emergency_stop("Jetson link timeout");
			tm_printf((UB*)"[fault] Jetson silent for %d ms (watchdog %d ms) -> safe state (brake + center)\n", (INT)lost_ms, GT_WATCHDOG_MS);
		}
		if (!st.stopped && ms - last_cmd_ms > GT_WATCHDOG_MS) emergency_stop("no approved command");
		if (st.stopped && ms - last_alive_ms >= 100) {		/* keep VESC braking while stopped */
			gt_vesc_set_rpm(0); last_alive_ms = ms;
		}
		static UW last_perf_ms, last_metric_ms;
		if (ms - last_metric_ms >= 10000) {
			last_metric_ms = ms;
			gv_metric_report();
			gv_stack_report();
		}
		if (ms - last_perf_ms >= 5000) {
			last_perf_ms = ms;
			gv_perf_report();
			gv_stat_t g = gv_perf_gate_cmd, q = gv_perf_irq2task, z = gv_perf_hazard2brake;
			gv_stat_reset(&gv_perf_gate_cmd); gv_stat_reset(&gv_perf_irq2task);
			tm_printf((UB*)"[perf] gate CMD verdict+actuation n=%d avg %d us max %d us | USART2 irq->gate task n=%d avg %d ns max %d ns | hazard->brake (since boot) n=%d max %d us\n",
				  (INT)g.n, g.n ? (INT)GV_CYC2US((UW)(g.sum / g.n)) : 0, (INT)GV_CYC2US(g.max),
				  (INT)q.n, q.n ? (INT)GV_CYC2NS((UW)(q.sum / q.n)) : 0, (INT)GV_CYC2NS(q.max),
				  (INT)z.n, (INT)GV_CYC2US(z.max));
		}
		if (ms - last_report_ms >= 1000) {
			last_report_ms = ms;
			T_RTSK vr = {0}; if (gv_vision_tid > 0) tk_ref_tsk(gv_vision_tid, &vr);
			tm_printf((UB*)"[gate] n=%d ok=%d veto=%d bad=%d replay=%d maxlat=%dus | servo=%d v=%d mm/s %s | vis st=%d fr=%d camerr=%d tsk=%x/%x | rx ovr=%d | /s loops %d csi %d dcmipp %d u2 %d\n",
				  (INT)gk.n_total, (INT)gk.n_approved, (INT)gk.n_veto, (INT)gk.n_malformed,
				  (INT)gk.n_replay_or_reorder, (INT)gk.max_latency_us,
				  (INT)(st.servo * 1000), (INT)(st.speed * 1000), st.stopped ? "STOPPED" : "DRIVING",
				  (INT)gv_stage, (INT)gv_frames, (INT)gv_cam_err, (INT)vr.tskstat, (INT)vr.tskwait, (INT)gt_uart_overruns(GT_UART_JETSON),
				  (INT)(loops - p_loops), (INT)(gv_irq_csi - p_csi), (INT)(gv_irq_dcmipp - p_dcm), (INT)(gt_irq_usart2 - p_u2));
			p_loops = loops; p_csi = gv_irq_csi; p_dcm = gv_irq_dcmipp; p_u2 = gt_irq_usart2;
			/* vision stall probe: frame counter frozen for 3 s -> dump the vision task's saved
			 * context once (look for 0x340xxxxx code addresses, then addr2line) */
			static UW last_fr, same_fr, dumped;
			if (gv_frames == last_fr) same_fr++; else { same_fr = 0; dumped = 0; }
			last_fr = gv_frames;
			if (same_fr >= 3 && !dumped && gv_vision_tid > 0) {
				dumped = 1;
				TCB *t = get_tcb(gv_vision_tid);
				UW *sp = (UW *) t->tskctxb.ssp;
				tm_printf((UB*)"[gate] VISION STALL st=%d ssp=%x:", (INT)gv_stage, (UW)sp);
				for (INT k = 0; k < 48; k++) tm_printf((UB*)" %x", sp[k]);
				tm_printf((UB*)"\n");
			}
		}
	}
}

/* ---- local safety monitor: camera/NPU + ToF land here (stub: no hazard) ---- */
__attribute__((weak)) gt_local_safety_t gt_local_safety_poll(void)
{
	gt_local_safety_t r = { .hazard = 0, .collision_probability = 0.0f };
	return r;
}
