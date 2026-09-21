/*
 * usermain.c - Guardian-TRON tasks on μT-Kernel 3.0 / STM32N6570-DK
 *
 *   gate   (pri 8, HIGH)  Jetson CMD -> safety envelope -> VESC; watchdog; local-safety override
 *   vision (pri 20, LOW)  camera -> NPU person detector -> hazard -> kicks gate
 *
 * Mixed criticality: the NPU pipeline may take tens of ms per frame, but it never
 * delays the gatekeeper, and the moment it raises a hazard the gate preempts it.
 *
 * Wiring (verified 2026-09-13):
 *   USART2 Jetson : D0 PF6 RX, D1 PD5 TX, 115200
 *   USART3 VESC   : SWAPPED -> TX on D5/PE10 (red -> VESC RX), RX on A5/PB10 (brown <- VESC TX), 38400
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "stm32n6xx_hal.h"
#include "gt_uart.h"
#include "gt_vesc.h"
#include "gt_gate.h"
#include "gv_perf.h"

void gv_hal_tick_attach(void);
void gv_vision_task(INT stacd, void *exinf);
void gv_imu_task(INT stacd, void *exinf);
void gv_i2c_lock_init(void);
void gv_log_task(INT stacd, void *exinf);
void gv_mpu_init(ID gate_task_id);
void gv_mpu_add_stack_guard(void *stack);
void gv_mpu_enable(void);

EXPORT ID gv_vision_tid;

/* task stacks: static, 32-byte aligned; the lowest 32 bytes of each are an MPU guard.
 * TA_FPU: every task uses floats, so the dispatcher must save S16-S31 for it. */
#define GATE_STK	4096
#define IMU_STK		4096
#define VISION_STK	(32 * 1024)
LOCAL UB stk_gate[GATE_STK]     __attribute__((aligned(32)));
LOCAL UB stk_imu[IMU_STK]       __attribute__((aligned(32)));
LOCAL UB stk_vision[VISION_STK] __attribute__((aligned(32)));

#define TSK_ATR	(TA_HLNG | TA_RNG0 | TA_FPU | TA_USERBUF)
LOCAL T_CTSK ctsk_gate   = { .itskpri = 8,  .stksz = GATE_STK,   .bufptr = stk_gate,   .task = gt_gate_task,   .tskatr = TSK_ATR };
LOCAL T_CTSK ctsk_imu    = { .itskpri = 10, .stksz = IMU_STK,    .bufptr = stk_imu,    .task = gv_imu_task,    .tskatr = TSK_ATR };
#define LOG_STK		1024
LOCAL UB stk_log[LOG_STK]       __attribute__((aligned(32)));
LOCAL T_CTSK ctsk_log    = { .itskpri = 30, .stksz = LOG_STK, .bufptr = stk_log, .task = gv_log_task, .tskatr = TA_HLNG | TA_RNG0 | TA_USERBUF };
LOCAL T_CTSK ctsk_vision = { .itskpri = 20, .stksz = VISION_STK, .bufptr = stk_vision, .task = gv_vision_task, .tskatr = TSK_ATR };

EXPORT INT usermain(void)
{
	extern void gv_bc(unsigned long);
	gv_bc(0xB0070010);
	gv_hal_tick_attach();
	gv_perf_init();
	gv_probe_init();
	{	/* USER1 button (PC13) = MPU fault-injection trigger, read by the vision task */
		__HAL_RCC_GPIOC_CLK_ENABLE();
		GPIO_InitTypeDef g = { .Pin = GPIO_PIN_13, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_PULLDOWN };
		HAL_GPIO_Init(GPIOC, &g);
	}
	tm_putstring((UB*)"Guardian-TRON start (gatekeeper + camera/NPU person stop + IMU monitor)\n");
	gv_perf_ctxsw_bench();
	gv_stack_watch("gate", stk_gate, GATE_STK);
	gv_stack_watch("imu", stk_imu, IMU_STK);
	gv_stack_watch("vision", stk_vision, VISION_STK);
	gv_stack_watch("log", stk_log, LOG_STK);
	{	/* from here on console output goes through the logger task (gv_log.c) */
		ID lg = tk_cre_tsk(&ctsk_log); tk_sta_tsk(lg, 0); gv_perf_name(lg, "log");
	}
	gv_i2c_lock_init();

	/* VESC link first: the gate task assumes it is up */
	gt_uart_set_swap(GT_UART_VESC, TRUE);
	gt_vesc_init(GT_VESC_BAUD);
	UB pl[64];
	INT n = gt_vesc_request(COMM_GET_FW_VERSION, pl, sizeof pl, 300);
	if (n > 0) tm_printf((UB*)"[vesc] fw %d.%d ready\n", pl[1], pl[2]);
	else tm_printf((UB*)"[vesc] NO REPLY (%d) - gate will run but VESC is silent\n", n);

	ID gate = tk_cre_tsk(&ctsk_gate);
	ID imu  = tk_cre_tsk(&ctsk_imu);
	ID vis  = tk_cre_tsk(&ctsk_vision);
	if (gate < E_OK || imu < E_OK || vis < E_OK)
		tm_printf((UB*)"[main] task create failed: gate %d imu %d vision %d\n", gate, imu, vis);
	gv_vision_tid = vis;
	gv_perf_name(tk_get_tid(), "main"); gv_perf_name(gate, "gate"); gv_perf_name(imu, "imu"); gv_perf_name(vis, "vision");

	gv_mpu_init(gate);
	gv_mpu_add_stack_guard(stk_gate);
	gv_mpu_add_stack_guard(stk_imu);
	gv_mpu_add_stack_guard(stk_vision);
	gv_mpu_enable();

	{
		static const char *const tbl[] = {
			"[rtos] task set (fixed-priority preemptive, lower number = higher priority)\n",
			"[rtos]   task    pri  release                   deadline  criticality  on overrun / failure\n",
			"[rtos]   gate      8  Jetson CMD @100 Hz, kick  1 ms      HI-CRIT A    watchdog 200 ms -> brake\n",
			"[rtos]   imu      10  cyclic handler 10 ms      10 ms     HI-CRIT B    no sample -> keep last hazard state\n",
			"[rtos]   vision   20  camera frame (~15 fps)    100 ms    MED-CRIT     no frame 500 ms -> car held\n",
			"[rtos]   log      30  ring buffer non-empty     best eff  NON-CRIT     drop + count\n",
			"[rtos] kernel tick 1 ms, MPU on, dispatch hook on; metrics table every 10 s ([metric] lines)\n",
		};
		for (INT i = 0; i < (INT)(sizeof tbl / sizeof tbl[0]); i++) tm_putstring((UB*)tbl[i]);
	}
	gv_bc(0xB0070011);
	tk_sta_tsk(gate, 0);
	tk_sta_tsk(imu, 0);
	tk_sta_tsk(vis, 0);
	tk_slp_tsk(TMO_FEVR);
	return 0;
}
