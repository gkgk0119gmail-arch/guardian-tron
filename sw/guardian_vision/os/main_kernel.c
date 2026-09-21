/*
 * main_kernel.c - Guardian-TRON entry point.
 * ST hardware bring-up (clocks, caches, NPU RAM, xSPI, RIF) runs bare-metal first,
 * then μT-Kernel 3.0 takes over; everything else runs in tasks (see usermain.c).
 */
#include "stm32n6xx_hal.h"
#include "gv_vision.h"

void knl_start_mtkernel(void);

int main(void)
{
  gv_hw_init();

  /* cycle counter: microsecond timestamps for the latency logs (kernel owns SysTick) */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  knl_start_mtkernel();   /* no return */
  while (1);
}
