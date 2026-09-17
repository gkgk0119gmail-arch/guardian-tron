/* STM32N6570-DK UART bridge for the gatekeeper safety logic.
 *
 * Jetson link: USART2 on the Arduino connector (D0=RX/PF6, D1=TX/PD5),
 * confirmed against the official pinout and wired to the Jetson via a
 * USB-to-TTL adapter (Jetson opens it as /dev/ttyUSB0, see
 * jetson/ecu/uart_link.py). 115200 8N1, matches docs/PROTOCOL.md.
 *
 * VESC link: a SEPARATE UART peripheral (NOT USART2 -- that's taken by the
 * Jetson link, and STMod+'s "USART2" pins are electrically the same
 * peripheral, not a second one). Which exact USARTx/pins to use for VESC
 * is still being confirmed against CubeMX for this board (see
 * docs/STM32_PORT.md, VESC section) -- vesc_uart_init() takes whatever
 * UART_HandleTypeDef CubeMX generates for it.
 *
 * Architecture: VESC is wired ONLY to STM32, never to the Jetson, by
 * design -- the Jetson computes intended steer/accel and sends it over the
 * USART2 link, gk_core_process() judges it against the safety envelope,
 * and ONLY the resulting `applied` command (Jetson's command if approved,
 * the last known-safe command if vetoed) ever reaches VESC. This is what
 * makes the "gatekeeper survives a compromised/malfunctioning Jetson"
 * property real rather than aspirational: there is no code path left by
 * which the Jetson can drive the actuator directly.
 *
 * NOT YET BUILD-TESTED against a real CubeMX-generated project (no
 * firmware flashed yet). gk_core_process() itself (gatekeeper/src/
 * gatekeeper_core.c) is completely unaware of UART/HAL -- this file only
 * adapts the transport, exactly like gatekeeper/sim/main_sim.c does for
 * the Linux PTY SIM build.
 *
 * Framing: byte-at-a-time interrupt receive into a 15-byte ring position,
 * resyncing on STX (0xAA) the same way sim/main_sim.c does over a PTY --
 * necessary because UART is a byte stream with no built-in message
 * boundaries.
 */
#include "gatekeeper_uart_bridge.h"

#include <string.h>

#include "protocol.h"
#include "safety_envelope.h"
#include "gatekeeper_core.h"
#include "dwt_timing.h"
#include "vesc_uart.h"
#include "local_safety_monitor.h"

/* Placeholder calibration -- TUNE AGAINST THE REAL VEHICLE before trusting
 * this for anything. The safety envelope's own limits (+-540deg, [-10,4]
 * m/s^2, see safety_envelope.h) are generic bounds from the paper, not
 * this specific car's real steering/motor range. */
#define VESC_STEER_MAX_DEG   30.0f   /* real mechanical steering limit, guess */
#define VESC_ACCEL_TO_AMPS   2.0f    /* amps per (m/s^2) of requested accel, guess */

static UART_HandleTypeDef *s_huart;
static gk_core_state_t s_state;

static uint8_t s_rx_byte;               /* HAL_UART_Receive_IT target, one byte at a time */
static uint8_t s_frame_buf[GK_CMD_FRAME_LEN];
static size_t s_frame_have;
static uint32_t s_t_rx;                 /* timestamp latched when STX (byte 0) arrives */

static void arm_next_byte_rx(void) {
    HAL_UART_Receive_IT(s_huart, &s_rx_byte, 1);
}

void gatekeeper_uart_init(UART_HandleTypeDef *huart) {
    s_huart = huart;
    dwt_timing_init();
    gk_core_init(&s_state);
    local_safety_init();
    s_frame_have = 0;
    arm_next_byte_rx();
}

void gatekeeper_periodic_safety_check(void) {
    local_safety_reading_t r = local_safety_poll();
    if (!r.obstacle_imminent) {
        return; /* nothing to do -- the Jetson-link path (envelope check) governs as usual */
    }
    /* Emergency stop, independent of whatever the Jetson last said:
     * center the steering and apply a firm brake current. This is the
     * "STM32 survives even a fully silent/frozen Jetson" property. */
    vesc_set_servo_pos(0.5f);
    vesc_set_current_brake(10.0f /* amps, placeholder -- tune against real vehicle */);
}

void gatekeeper_uart_rx_cplt_callback(UART_HandleTypeDef *huart) {
    if (huart != s_huart) {
        return; /* not our USART -- CubeMX shares this callback across all UARTs */
    }

    uint8_t byte = s_rx_byte;

    if (s_frame_have == 0) {
        if (byte != GK_CMD_STX) {
            arm_next_byte_rx();
            return; /* resync: discard noise until STX, same as sim/main_sim.c */
        }
        s_t_rx = now_us();
    }

    s_frame_buf[s_frame_have++] = byte;

    if (s_frame_have == GK_CMD_FRAME_LEN) {
        if (s_frame_buf[GK_CMD_FRAME_LEN - 1] != GK_ETX) {
            /* Lost framing: shift by one and keep resyncing, exactly like
             * the SIM build's recovery path. */
            memmove(s_frame_buf, s_frame_buf + 1, GK_CMD_FRAME_LEN - 1);
            s_frame_have = GK_CMD_FRAME_LEN - 1;
            arm_next_byte_rx();
            return;
        }

        gk_verdict_frame_t resp;
        gk_cmd_t applied;
        uint32_t t_verdict = now_us();
        gk_core_process(&s_state, s_frame_buf, s_t_rx, t_verdict, &resp, &applied);

        /* This is the only place in the firmware that commands VESC --
         * `applied` is already gk_core_process()'s verdict-adjusted
         * command (Jetson's request if approved, else the held last-safe
         * command), so whatever Jetson sends, only a safety-envelope-
         * compliant value ever reaches the motor/servo. */
        float servo_pos = 0.5f + (applied.steer_deg / VESC_STEER_MAX_DEG) * 0.5f;
        if (servo_pos > 1.0f) servo_pos = 1.0f;
        if (servo_pos < 0.0f) servo_pos = 0.0f;
        vesc_set_servo_pos(servo_pos);
        vesc_set_current(applied.accel_mps2 * VESC_ACCEL_TO_AMPS);

        uint8_t out[GK_VERDICT_FRAME_LEN];
        gk_encode_verdict(&resp, out);
        HAL_UART_Transmit(s_huart, out, sizeof(out), 50 /* ms timeout */);

        s_frame_have = 0;
    }

    arm_next_byte_rx();
}
