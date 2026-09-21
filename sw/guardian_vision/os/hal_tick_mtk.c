/*
 * hal_tick_mtk.c - HAL time base under μT-Kernel.
 *
 * μT-Kernel owns SysTick, so HAL_IncTick() stops being called once the kernel starts.
 * Before that, HAL keeps its own uwTick; afterwards HAL_GetTick() follows the kernel
 * clock (offset so it stays monotonic) and HAL_Delay() sleeps the calling task
 * instead of spinning. The camera/ISP middleware relies on both.
 */
#include "stm32n6xx_hal.h"
#include <tk/tkernel.h>

static volatile int gv_kernel_running;
static uint32_t tick_offset;

void gv_hal_tick_attach(void)
{
  SYSTIM t;
  tk_get_tim(&t);
  tick_offset = uwTick - t.lo;
  gv_kernel_running = 1;
}

uint32_t HAL_GetTick(void)
{
  if (!gv_kernel_running) return uwTick;
  SYSTIM t;
  tk_get_tim(&t);
  return t.lo + tick_offset;
}

void HAL_Delay(uint32_t Delay)
{
  if (gv_kernel_running && __get_IPSR() == 0U)
  {
    tk_dly_tsk(Delay ? Delay : 1);
    return;
  }
  uint32_t start = HAL_GetTick();
  uint32_t wait = Delay;
  if (wait < HAL_MAX_DELAY) wait += (uint32_t) uwTickFreq;
  while ((HAL_GetTick() - start) < wait) { }
}

void gv_os_sleep_ms(uint32_t ms)
{
  tk_dly_tsk(ms ? ms : 1);
}
