#ifndef GATEKEEPER_DWT_TIMING_H
#define GATEKEEPER_DWT_TIMING_H

#include <stdint.h>

/* Mirrors the paper's eq. (1) measurement method: enables the Cortex-M
 * DWT cycle counter so L_fs = (C_verdict - C_rx) / f_clk can be computed
 * directly, the same way gatekeeper/sim/main_sim.c uses clock_gettime()
 * on Linux. Call once at startup, after SystemClock is configured. */
void dwt_timing_init(void);

/* Microseconds since dwt_timing_init(), derived from DWT->CYCCNT and
 * SystemCoreClock. Wraps around after ~7s at 600MHz in this 32-bit
 * counter -- fine here since callers only diff two timestamps taken
 * microseconds apart within the same call. */
uint32_t now_us(void);

#endif /* GATEKEEPER_DWT_TIMING_H */
