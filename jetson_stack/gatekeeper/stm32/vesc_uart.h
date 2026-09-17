#ifndef GATEKEEPER_VESC_UART_H
#define GATEKEEPER_VESC_UART_H

#include "stm32n6xx_hal.h"

/* VESC motor controller UART protocol, C port of jetson/ecu/vesc.py
 * (which was verified against real hardware: fw 6.6, v_in=15.1V read
 * correctly). Field layout matches modern VESC firmware (id/iq FOC
 * currents present, rpm is int32).
 *
 * This is the ONLY path that is allowed to drive the motor in the
 * production build: the Jetson's vesc.py set_duty()/set_current() methods
 * are bench-test tools only (see jetson/ecu/vesc.py docstring) -- VESC is
 * wired exclusively to STM32 now, and STM32 is the sole authority that
 * actually commands it, after gk_core_process() has judged the command.
 *
 * Call vesc_uart_init() once (separate UART handle from the Jetson link --
 * do NOT reuse huart2, that's the gatekeeper's link to the Jetson).
 * Wire the actual UART_HandleTypeDef* for VESC once CubeMX confirms which
 * USART is free (see docs/STM32_PORT.md VESC section).
 */

void vesc_uart_init(UART_HandleTypeDef *huart);

/* PHYSICALLY MOVES THE STEERING SERVO. pos in [0, 1] (0=one end of travel,
 * 1=the other, 0.5=center) -- VESC's COMM_SET_SERVO_POS (id 12), verified
 * against the official vedderb/bldc firmware source. Calibrate 0/0.5/1
 * against your actual servo's mechanical range before trusting this for
 * anything -- VESC applies its own configured min/max/center clamps too. */
void vesc_set_servo_pos(float pos);

/* PHYSICALLY SPINS THE MOTOR. duty in [-1, 1]. */
void vesc_set_duty(float duty);

/* PHYSICALLY SPINS THE MOTOR. */
void vesc_set_current(float amps);

/* Regenerative/plugging brake current. PHYSICALLY AFFECTS THE MOTOR. */
void vesc_set_current_brake(float amps);

/* Keepalive heartbeat -- call periodically (e.g. every gatekeeper cycle)
 * even when not otherwise sending a drive command, so VESC firmware
 * timeout-based cutoffs (if configured) don't trip during normal
 * passthrough operation. */
void vesc_alive(void);

#endif /* GATEKEEPER_VESC_UART_H */
