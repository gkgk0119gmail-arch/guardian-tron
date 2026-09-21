/* STM32N6570-DK / lwIP integration for the gatekeeper safety logic.
 *
 * This is written against lwIP's "raw" API (udp_new/udp_bind/udp_recv),
 * which is what STM32CubeMX's LWIP middleware generates by default and is
 * stable across lwIP versions -- unlike the BSD-socket wrapper, which is an
 * optional lwIP config you may or may not have enabled.
 *
 * NOT YET BUILD-TESTED against a real CubeMX-generated project (no board
 * flashed yet -- see docs/STM32_PORT.md). Two things you will likely need
 * to double-check/adjust once you have the generated project open:
 *   1. The exact CubeMX macro/header names below (LWIP_TIMEVAL_PRIVATE,
 *      lwip.h vs individual headers) can differ slightly by CubeMX/lwIP
 *      version -- if a header fails to resolve, check
 *      Core/Inc/lwipopts.h and Middlewares/Third_Party/LwIP for the actual
 *      paths CubeMX generated for you.
 *   2. `gatekeeper_udp_init()` must run AFTER MX_LWIP_Init() -- call it from
 *      Core/Src/main.c right after that line, inside the same
 *      USER CODE BEGIN/END block CubeMX preserves across regeneration.
 *
 * gk_core_process() itself (gatekeeper/src/gatekeeper_core.c) is completely
 * unaware of lwIP -- this file only adapts the transport.
 */
#include "gatekeeper_lwip_udp.h"

#include <string.h>

#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include "protocol.h"
#include "safety_envelope.h"
#include "gatekeeper_core.h"
#include "dwt_timing.h"

static struct udp_pcb *s_pcb;
static gk_core_state_t s_state;

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg;
    uint32_t t_rx = now_us();

    gk_verdict_frame_t resp;
    gk_cmd_t applied;

    if (p == NULL) {
        return;
    }

    if (p->len != GK_CMD_FRAME_LEN || p->tot_len != GK_CMD_FRAME_LEN) {
        /* Wrong-sized datagram -- mirrors gatekeeper/sim/main_udp.c's
         * handling of the same case. */
        s_state.n_total++;
        s_state.n_malformed++;
        resp.seq = 0;
        resp.verdict = GK_VERDICT_MALFORMED;
        resp.latency_us = now_us() - t_rx;
    } else {
        uint8_t raw[GK_CMD_FRAME_LEN];
        memcpy(raw, p->payload, GK_CMD_FRAME_LEN);
        uint32_t t_verdict = now_us();
        gk_core_process(&s_state, raw, t_rx, t_verdict, &resp, &applied);

        /* TODO once wired to a real actuator: apply `applied.steer_deg` /
         * applied.accel_mps2` here (PWM/CAN/whatever drives brake-by-wire
         * and steer-by-wire on this platform). For now this only proves the
         * safety judgment path end-to-end, same as the LED-actuator demo in
         * the paper's Ⅳ.2 section. */
    }

    uint8_t out[GK_VERDICT_FRAME_LEN];
    gk_encode_verdict(&resp, out);

    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, GK_VERDICT_FRAME_LEN, PBUF_RAM);
    if (reply != NULL) {
        memcpy(reply->payload, out, GK_VERDICT_FRAME_LEN);
        udp_sendto(pcb, reply, addr, port);
        pbuf_free(reply);
    }

    pbuf_free(p);
}

void gatekeeper_udp_init(void) {
    dwt_timing_init();
    gk_core_init(&s_state);

    s_pcb = udp_new();
    if (s_pcb == NULL) {
        return; /* out of PCBs -- shouldn't happen this early in boot */
    }
    udp_bind(s_pcb, IP_ADDR_ANY, 5005);
    udp_recv(s_pcb, udp_recv_cb, NULL);
}
