#include "dwt_timing.h"

#include "cmsis_compiler.h"
#ifndef DWT
#include "core_cm55.h"
#endif

extern uint32_t SystemCoreClock; /* set by CubeMX clock config, ~600 MHz on this board */

void dwt_timing_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t now_us(void) {
    return (uint32_t)((uint64_t)DWT->CYCCNT * 1000000ull / SystemCoreClock);
}
