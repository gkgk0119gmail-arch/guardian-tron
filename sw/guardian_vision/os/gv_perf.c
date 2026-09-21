/*
 * gv_perf.c - Guardian-TRON timing instrumentation on μT-Kernel 3.0
 *
 * The BSP2 kernel has no td_hok_dsp() implementation, so the ARMv8-M dispatcher
 * (mtk3_bsp2/sysdepend/stm32_cube/cpu/core/armv8m/dispatch.S) calls gv_dsp_hook()
 * at every task switch, with interrupts masked. The hook
 *   - charges the elapsed cycles to the task that was running (CPU share per task),
 *   - keeps a short ring of the last switches (kernel-monitor style timeline),
 *   - switches the per-task MPU view (gv_mpu_on_dispatch).
 * Interrupt time is charged to the task it interrupted.
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <kernel.h>
#include "gv_perf.h"

void gv_mpu_on_dispatch(ID tskid);

#define NSLOT		(CNF_MAX_TSKID + 1)	/* slot 0 = idle (no runnable task) */
#define TRACE_N		64

LOCAL UD   run_cyc[NSLOT];			/* cycles charged per task */
LOCAL UD   run_prev[NSLOT];			/* snapshot at the last report */
LOCAL const char *slot_name[NSLOT];
LOCAL INT  cur_slot;
LOCAL UW   last_cyc;
LOCAL UW   n_switch, n_switch_prev;
LOCAL UD   total_prev;
LOCAL struct { UW cyc; UB slot; } trace[TRACE_N];
LOCAL UW   trace_i;
volatile UW gv_last_dsp_cyc;

gv_stat_t gv_perf_gate_cmd, gv_perf_irq2task, gv_perf_hazard2brake;

/* called from dispatch.S: next = TCB about to run, NULL = no runnable task (idle) */
void gv_dsp_hook(TCB *next)
{
	UW now = DWT->CYCCNT;
	INT slot = next ? next->tskid : 0;
	if (slot == cur_slot) return;		/* idle loop re-entering after an interrupt */
	run_cyc[cur_slot] += (UW)(now - last_cyc);
	last_cyc = now;
	cur_slot = slot;
	n_switch++;
	trace[trace_i & (TRACE_N - 1)].cyc = now;
	trace[trace_i & (TRACE_N - 1)].slot = (UB)slot;
	trace_i++;
	gv_last_dsp_cyc = now;
	gv_mpu_on_dispatch(slot);
}

void gv_perf_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	last_cyc = DWT->CYCCNT;
	slot_name[0] = "idle";
}

void gv_perf_name(ID tskid, const char *name) { if (tskid > 0 && tskid < NSLOT) slot_name[tskid] = name; }

/* CPU share per task over the window since the previous call */
void gv_perf_report(void)
{
	UD snap[NSLOT], total = 0;
	UW sw; UINT imask;
	DI(imask);
	UW now = DWT->CYCCNT;
	run_cyc[cur_slot] += (UW)(now - last_cyc); last_cyc = now;
	for (INT i = 0; i < NSLOT; i++) { snap[i] = run_cyc[i] - run_prev[i]; run_prev[i] = run_cyc[i]; total += snap[i]; }
	sw = n_switch - n_switch_prev; n_switch_prev = n_switch;
	EI(imask);
	(void)total_prev;
	if (total == 0) return;

	UW secs_x10 = (UW)(total * 10 / SystemCoreClock); if (secs_x10 == 0) secs_x10 = 1;
	tm_printf((UB*)"[perf] cpu");
	for (INT i = 0; i < NSLOT; i++) {
		if (snap[i] == 0 && !slot_name[i]) continue;
		UW pm = (UW)(snap[i] * 1000 / total);		/* per mille */
		tm_printf((UB*)" %s %d.%d%%", slot_name[i] ? slot_name[i] : "tsk", (INT)(pm / 10), (INT)(pm % 10));
	}
	tm_printf((UB*)" | switches %d/s\n", (INT)(sw * 10 / secs_x10));
}

/* ---- context-switch benchmark (boot) ---------------------------------------------------- */
#define BENCH_N		2000
LOCAL volatile UW b_t0, b_t1;
LOCAL gv_stat_t b_wup, b_slp;
LOCAL ID b_main;

LOCAL void bench_task(INT stacd, void *exinf)
{
	for (INT i = 0; i < BENCH_N; i++) {
		tk_slp_tsk(TMO_FEVR);
		b_t1 = DWT->CYCCNT;
		gv_stat_add(&b_wup, b_t1 - b_t0);	/* tk_wup_tsk() -> this task running */
		gv_metric_add(GV_M_CTXSW, GV_CYC2NS(b_t1 - b_t0));
		b_t0 = DWT->CYCCNT;			/* tk_slp_tsk() -> caller running again */
	}
	tk_ext_tsk();
}

void gv_perf_ctxsw_bench(void)
{
	T_RTSK r;
	b_main = tk_get_tid();
	tk_ref_tsk(b_main, &r);
	PRI mypri = r.tskpri;
	tk_chg_pri(TSK_SELF, 10);
	T_CTSK c = { .itskpri = 5, .stksz = 1024, .task = bench_task, .tskatr = TA_HLNG | TA_RNG0 };
	ID id = tk_cre_tsk(&c);
	tk_sta_tsk(id, 0);				/* runs now and sleeps */
	gv_stat_reset(&b_wup); gv_stat_reset(&b_slp);
	for (INT i = 0; i < BENCH_N; i++) {
		b_t0 = DWT->CYCCNT;
		tk_wup_tsk(id);				/* higher priority: preempts inside this call */
		UW t2 = DWT->CYCCNT;
		gv_stat_add(&b_slp, t2 - b_t0);
	}
	tk_dly_tsk(2); tk_del_tsk(id);
	tk_chg_pri(TSK_SELF, mypri);
	tm_printf((UB*)"[perf] context switch (tk_wup_tsk -> higher-priority task running, n=%d): min %d ns avg %d ns max %d ns\n",
		  (INT)b_wup.n, (INT)GV_CYC2NS(b_wup.min), (INT)GV_CYC2NS((UW)(b_wup.sum / b_wup.n)), (INT)GV_CYC2NS(b_wup.max));
	tm_printf((UB*)"[perf] context switch (tk_slp_tsk -> lower-priority task resumes, n=%d): min %d ns avg %d ns max %d ns\n",
		  (INT)b_slp.n, (INT)GV_CYC2NS(b_slp.min), (INT)GV_CYC2NS((UW)(b_slp.sum / b_slp.n)), (INT)GV_CYC2NS(b_slp.max));
}

/* ---- probe pins ---------------------------------------------------------------------- */
LOCAL GPIO_TypeDef *const probe_port[3] = { GPIOD, GPIOE, GPIOH };
LOCAL const UW probe_pin[3] = { GPIO_PIN_0, GPIO_PIN_9, GPIO_PIN_5 };

void gv_probe_init(void)
{
	__HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOE_CLK_ENABLE(); __HAL_RCC_GPIOH_CLK_ENABLE();
	GPIO_InitTypeDef g = { .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_HIGH };
	for (INT i = 0; i < 3; i++) {
		probe_port[i]->BSRR = (UW)probe_pin[i] << 16;
		g.Pin = probe_pin[i]; HAL_GPIO_Init(probe_port[i], &g);
	}
}
void gv_probe_set(INT pin, INT on) { probe_port[pin]->BSRR = on ? probe_pin[pin] : ((UW)probe_pin[pin] << 16); }
void gv_probe_toggle(INT pin) { probe_port[pin]->ODR ^= probe_pin[pin]; }

/* ---- cumulative metrics ------------------------------------------------------------ */
#define SUBB	5				/* 32 sub-buckets per octave: ~3 % resolution */
#define NSUB	(1 << SUBB)
#define HB	((32 - SUBB + 1) * NSUB)
typedef struct { const char *name; UW target_ns; UW hist[HB]; UW n, miss, max; UD sum; } metric_t;
LOCAL metric_t M[GV_M_N] = {
	[GV_M_CTXSW]      = { "ctx_switch",        5700 },
	[GV_M_IRQ2TASK]   = { "irq_to_task",       50000 },
	[GV_M_GATE_CMD]   = { "gate_cmd_verdict",  1000000 },
	[GV_M_HAZ2BRAKE]  = { "hazard_to_brake",   100000 },
	[GV_M_FRAME2DET]  = { "frame_to_decision", 100000000 },
	[GV_M_NPU]        = { "npu_inference",     50000000 },
	[GV_M_IMU_JITTER] = { "monitor_jitter",    100000 },
	[GV_M_IMU_STEP]   = { "monitor_step",      1000000 },
	[GV_M_LINK_LOSS]  = { "link_loss_to_safe", 250000000 },
};
LOCAL INT bucket(UW v)
{
	if (v < NSUB) return (INT)v;
	INT e = 31 - __builtin_clz(v);			/* >= SUBB */
	INT b = (e - SUBB + 1) * NSUB + (INT)((v >> (e - SUBB)) & (NSUB - 1));
	return b < HB ? b : HB - 1;
}
LOCAL UW bucket_mid(INT b)				/* centre of a bucket (for percentiles) */
{
	if (b < NSUB) return (UW)b;
	INT e = b / NSUB + SUBB - 1, m = b % NSUB;
	UD lo = (UD)(NSUB + m) << (e - SUBB), w = (UD)1 << (e - SUBB);
	return (UW)(lo + w / 2);
}
void gv_metric_add(INT m, UW ns)
{
	metric_t *x = &M[m];
	UINT imask; DI(imask);
	x->hist[bucket(ns)]++; x->n++; x->sum += ns; if (ns > x->max) x->max = ns;
	if (ns > x->target_ns) x->miss++;
	EI(imask);
}
void gv_metric_miss(INT m) { M[m].miss++; }
LOCAL UW pct(const metric_t *x, UW per_mille)
{
	UW want = (UW)(((UD)x->n * per_mille + 999) / 1000), acc = 0;
	for (INT b = 0; b < HB; b++) { acc += x->hist[b]; if (acc >= want) { UW h = bucket_mid(b); return h < x->max ? h : x->max; } }
	return x->max;
}
LOCAL void pr_ns(const char *label, UW ns)
{
	if (ns < 10000) tm_printf((UB*)" %s %dns", label, (INT)ns);
	else if (ns < 10000000) tm_printf((UB*)" %s %d.%dus", label, (INT)(ns / 1000), (INT)(ns / 100 % 10));
	else tm_printf((UB*)" %s %d.%dms", label, (INT)(ns / 1000000), (INT)(ns / 100000 % 10));
}
void gv_metric_report(void)
{
	tm_printf((UB*)"[metrics] ---- since boot: n, mean, p50, p95, p99, max | target, over-target count, verdict ----\n");
	for (INT i = 0; i < GV_M_N; i++) {
		metric_t *x = &M[i];
		if (x->n == 0) { tm_printf((UB*)"[metric] %s: no samples yet\n", x->name); continue; }
		tm_printf((UB*)"[metric] %s: n=%d", x->name, (INT)x->n);
		pr_ns("mean", (UW)(x->sum / x->n)); pr_ns("p50", pct(x, 500)); pr_ns("p95", pct(x, 950));
		pr_ns("p99", pct(x, 990)); pr_ns("max", x->max); pr_ns("| target <", x->target_ns);
		tm_printf((UB*)" over %d -> %s\n", (INT)x->miss, x->max <= x->target_ns ? "PASS" : "FAIL");
	}
}

/* ---- stack high-water marks --------------------------------------------------------- */
#define STK_PAT	0xCCU
LOCAL struct { const char *name; UB *stk; UW size; } stk[8];
LOCAL INT n_stk;
void gv_stack_watch(const char *name, void *s, UW size)
{
	if (n_stk >= 8) return;
	UB *b = (UB *)s;
	for (UW i = 0; i < size; i++) b[i] = STK_PAT;
	stk[n_stk].name = name; stk[n_stk].stk = b; stk[n_stk].size = size; n_stk++;
}
void gv_stack_report(void)
{
	tm_printf((UB*)"[stack]");
	for (INT i = 0; i < n_stk; i++) {
		UW free = 0;
		while (free < stk[i].size && stk[i].stk[free] == STK_PAT) free++;
		UW used = stk[i].size - free;
		tm_printf((UB*)" %s %d/%d B (%d%%)", stk[i].name, (INT)used, (INT)stk[i].size, (INT)(used * 100 / stk[i].size));
	}
	tm_printf((UB*)"\n");
}
