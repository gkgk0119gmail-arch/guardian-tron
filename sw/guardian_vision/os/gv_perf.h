/*
 * gv_perf.h - Guardian-TRON timing instrumentation (μT-Kernel dispatch trace, CPU load,
 * context-switch benchmark, WCET/jitter statistics, logic-analyzer probe pins)
 *
 * All timestamps are DWT->CYCCNT (800 MHz core clock, 1.25 ns resolution).
 */
#ifndef GV_PERF_H
#define GV_PERF_H
#include <tk/tkernel.h>
#include "stm32n6xx_hal.h"

/* running min/max/sum statistics in cycles */
typedef struct { UW n, min, max; UD sum; } gv_stat_t;
static inline void gv_stat_add(gv_stat_t *s, UW v)
{
	if (s->n == 0 || v < s->min) s->min = v;
	if (v > s->max) s->max = v;
	s->sum += v; s->n++;
}
static inline void gv_stat_reset(gv_stat_t *s) { s->n = 0; s->min = 0; s->max = 0; s->sum = 0; }

static inline UW gv_cyc(void) { return DWT->CYCCNT; }
#define GV_CYC2US(c)	((UW)((c) / (SystemCoreClock / 1000000U)))
#define GV_CYC2NS(c)	((UW)(((UD)(c) * 1000U) / (SystemCoreClock / 1000000U)))

/* logic-analyzer probe pins on the Arduino header (3.3 V push-pull):
 *   D2 = PD0 : HIGH while the car is held by a local-safety hazard (edge = brake frame start)
 *   D3 = PE9 : toggles every 100 Hz IMU/safety-monitor period (period jitter on the scope)
 *   D4 = PH5 : HIGH while the gatekeeper processes one Jetson CMD (verdict WCET) */
#define GV_PROBE_HAZARD		0
#define GV_PROBE_TICK		1
#define GV_PROBE_GATE		2
void gv_probe_init(void);
void gv_probe_set(INT pin, INT on);
void gv_probe_toggle(INT pin);

void gv_perf_init(void);			/* DWT on; before the first dispatch */
void gv_perf_name(ID tskid, const char *name);	/* label for the CPU report */
void gv_perf_ctxsw_bench(void);			/* boot benchmark: tk_wup_tsk -> higher-priority task running */
void gv_perf_report(void);			/* CPU share per task since the last call (gate task, every 5 s) */

/* ---- cumulative metrics with percentiles (since boot) ----
 * log-linear histogram: 8 sub-buckets per power of two (~9 % resolution), values in ns */
enum {
	GV_M_CTXSW,		/* tk_wup_tsk -> higher-priority task running (boot benchmark) */
	GV_M_IRQ2TASK,		/* USART2 RX interrupt -> gatekeeper task running */
	GV_M_GATE_CMD,		/* one Jetson CMD: verdict + actuation */
	GV_M_HAZ2BRAKE,		/* hazard raised (camera/IMU) -> brake issued */
	GV_M_FRAME2DET,		/* camera frame landed -> person decision (NPU + decode) */
	GV_M_NPU,		/* NPU inference */
	GV_M_IMU_JITTER,	/* |actual period - 10 ms| of the 100 Hz safety monitor */
	GV_M_IMU_STEP,		/* safety monitor step: I2C + Kalman + decision */
	GV_M_LINK_LOSS,		/* last Jetson CMD -> car in safe state (watchdog) */
	GV_M_N
};
void gv_metric_add(INT m, UW ns);
void gv_metric_miss(INT m);			/* deadline miss */
void gv_metric_report(void);			/* table on the console */
void gv_stack_watch(const char *name, void *stk, UW size);	/* fill pattern; report high-water mark */
void gv_stack_report(void);

extern gv_stat_t gv_perf_gate_cmd;		/* one CMD: gk_core_process + actuation + verdict TX */
extern gv_stat_t gv_perf_irq2task;		/* USART2 RX interrupt -> gate task running */
extern gv_stat_t gv_perf_hazard2brake;		/* hazard raised (camera/IMU) -> brake frame starts */

#endif
