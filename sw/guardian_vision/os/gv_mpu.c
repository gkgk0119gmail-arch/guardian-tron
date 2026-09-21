/*
 * gv_mpu.c - Guardian-TRON spatial isolation with the Cortex-M55 MPU (freedom from
 * interference between the low-criticality vision task and the gatekeeper)
 *
 *   region 0  gatekeeper private state (.bss.gate_priv: envelope state, verdict counters,
 *             actuator status). Read-write ONLY while the gate task runs; read-only for
 *             every other task, switched by the dispatch hook (gv_dsp_hook -> here).
 *   region 1+ stack guards: the lowest 32 bytes of each task stack are read-only, so a
 *             stack overflow faults instead of silently corrupting the neighbour.
 *   last      null guard: writes to 0x00000000..0x000003FF fault.
 * Everything else keeps the default memory map (PRIVDEFENA), i.e. the same attributes
 * as with the MPU off.
 *
 * A violation raises MemManage. The handler records who/where, skips the faulting store
 * and lets the gatekeeper stop the car (it polls gv_mpu_violations): the offending write
 * never lands and the car ends in a safe state.
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <kernel.h>
#include "stm32n6xx_hal.h"

extern UB __gate_priv_start[], __gate_priv_end[];

#define ATTR_NORMAL	0		/* MAIR index: normal memory, write-back, read/write allocate */
#define MAX_GUARDS	4

volatile UW gv_mpu_violations, gv_mpu_fault_addr, gv_mpu_fault_pc, gv_mpu_fault_task, gv_mpu_on;
LOCAL ID  gate_tid;
LOCAL UW  rbar_rw, rbar_ro;
LOCAL INT n_regions;

LOCAL void set_region(UW rnr, UW base, UW limit, UW ro)
{
	ARM_MPU_SetRegion(rnr, ARM_MPU_RBAR(base, ARM_MPU_SH_NON, ro, 0 /* privileged */, 1 /* XN */),
			  ARM_MPU_RLAR(limit, ATTR_NORMAL));
}

/* stack buffers must be 32-byte aligned */
void gv_mpu_add_stack_guard(void *stack)
{
	if (n_regions >= 1 + MAX_GUARDS) return;
	UW b = (UW)stack;
	set_region(n_regions++, b, b + 31, 1);
}

void gv_mpu_init(ID gate_task_id)
{
	gate_tid = gate_task_id;
	ARM_MPU_Disable();
	ARM_MPU_SetMemAttr(ATTR_NORMAL, ARM_MPU_ATTR(ARM_MPU_ATTR_MEMORY_(1, 1, 1, 1), ARM_MPU_ATTR_MEMORY_(1, 1, 1, 1)));
	UW b = (UW)__gate_priv_start, l = (UW)__gate_priv_end - 1;
	rbar_rw = ARM_MPU_RBAR(b, ARM_MPU_SH_NON, 0, 0, 1);
	rbar_ro = ARM_MPU_RBAR(b, ARM_MPU_SH_NON, 1, 0, 1);
	ARM_MPU_SetRegion(0, rbar_ro, ARM_MPU_RLAR(l, ATTR_NORMAL));
	if (n_regions == 0) n_regions = 1;
}

void gv_mpu_enable(void)
{
	set_region(n_regions, 0x00000000, 0x000003FF, 1);	/* null guard */
	SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
	ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
	gv_mpu_on = 1;
	tm_printf((UB*)"[mpu] on: gatekeeper state %x..%x writable by gate task only, %d stack guards, null guard\n",
		  (UW)__gate_priv_start, (UW)__gate_priv_end, n_regions - 1);
}

/* from gv_dsp_hook (dispatcher, interrupts masked) */
void gv_mpu_on_dispatch(ID next)
{
	if (!gv_mpu_on) return;
	MPU->RNR = 0;
	MPU->RBAR = (next == gate_tid) ? rbar_rw : rbar_ro;
	__DSB(); __ISB();
}

/* MemManage: record, skip the faulting instruction, let the gatekeeper stop the car */
void gv_mpu_fault_c(UW *frame)
{
	UW cfsr = SCB->CFSR;
	gv_mpu_fault_addr = (cfsr & SCB_CFSR_MMARVALID_Msk) ? SCB->MMFAR : 0xFFFFFFFFU;
	gv_mpu_fault_pc = frame[6];
	gv_mpu_fault_task = knl_ctxtsk ? (UW)knl_ctxtsk->tskid : 0;
	UH hw = *(volatile UH *)frame[6];
	frame[6] += ((hw & 0xE000U) == 0xE000U && (hw & 0x1800U)) ? 4 : 2;	/* 32- or 16-bit Thumb */
	SCB->CFSR = cfsr & 0xFFU;					/* clear MMFSR (write 1 to clear) */
	gv_mpu_violations++;
}

__attribute__((naked)) void knl_memmanage_handler(void)
{
	__asm volatile(
		"tst   lr, #4      \n"
		"ite   eq          \n"
		"mrseq r0, msp     \n"
		"mrsne r0, psp     \n"
		"push  {r4, lr}    \n"
		"bl    gv_mpu_fault_c \n"
		"pop   {r4, pc}    \n");
}
