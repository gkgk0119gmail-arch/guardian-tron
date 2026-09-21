#ifndef GATEKEEPER_UART_BRIDGE_H
#define GATEKEEPER_UART_BRIDGE_H

#include "stm32n6xx_hal.h"

/* Call once after MX_USART2_UART_Init() has run. Starts the interrupt-driven
 * receive chain and initializes the gatekeeper safety-logic state. `huart`
 * is the handle CubeMX generated for USART2 (Arduino D0/D1 -- confirmed
 * pinout: D0=USART2_RX/PF6, D1=USART2_TX/PD5). */
void gatekeeper_uart_init(UART_HandleTypeDef *huart);

/* Must be called from HAL_UART_RxCpltCallback() in main.c (or wherever
 * CubeMX's USER CODE block for that callback lives) when it fires for
 * `huart`. This drives the byte-by-byte frame assembly state machine. */
void gatekeeper_uart_rx_cplt_callback(UART_HandleTypeDef *huart);

/* Independent local safety sweep -- the poster's "SafetyMonitor" task.
 * Wire this to a periodic timer (e.g. HAL_TIM_PeriodElapsedCallback for a
 * TIM configured at 50Hz), NOT to the Jetson-link RX callback: it must
 * keep running and able to trigger an emergency stop even if the Jetson
 * link goes completely silent (frozen/crashed Jetson, cut cable, etc).
 * When local_safety_monitor.c reports an imminent obstacle, this commands
 * VESC directly (servo centered, hard brake), bypassing gk_core_process
 * entirely -- it does not need the Jetson's envelope-checked command to
 * already be "safe" first, because there may not be a recent one. */
void gatekeeper_periodic_safety_check(void);

#endif /* GATEKEEPER_UART_BRIDGE_H */
