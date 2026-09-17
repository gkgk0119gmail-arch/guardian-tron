/* UDP-mode gatekeeper: runs the exact same gatekeeper_core.c /
 * safety_envelope.c / protocol.c used everywhere else, but listens on a real
 * UDP socket over the actual Ethernet link between this Jetson and the
 * STM32N6570-DK -- matching the physical wiring (RJ45, not USB/UART) and the
 * paper's own safety-link design (raw UDP, port 5005).
 *
 * Each datagram IS one CMD frame (UDP already preserves message
 * boundaries, unlike a UART byte stream), so no STX/ETX resync loop is
 * needed here -- much simpler than sim/main_sim.c.
 *
 * Today this binds 0.0.0.0:5005 and answers whoever sends to it, so it can
 * run right here on the Jetson while the STM32 side isn't flashed yet
 * (jetson/ecu talks to 127.0.0.1:5005). Once the board is flashed with the
 * real firmware (see docs/STM32_PORT.md) and has an IP on the 192.168.0.x
 * link, point the Jetson scripts at that IP instead and retire this binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "protocol.h"
#include "safety_envelope.h"
#include "gatekeeper_core.h"

#define DEFAULT_PORT 5005

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint32_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    fprintf(stderr, "[gatekeeper-udp] listening on 0.0.0.0:%d (UDP)\n", port);
    fprintf(stderr, "[gatekeeper-udp] safety envelope: |steer|<=%.0fdeg accel in [%.0f,%.0f] "
                     "rate<=%.0fdeg/cycle,%.0fm/s^2/cycle\n",
            GK_STEER_ABS_LIMIT_DEG, GK_ACCEL_MIN_MPS2, GK_ACCEL_MAX_MPS2,
            GK_STEER_RATE_LIMIT_DEG, GK_ACCEL_RATE_LIMIT_MPS2);

    gk_core_state_t st;
    gk_core_init(&st);

    uint8_t rxbuf[256];
    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(fd, rxbuf, sizeof(rxbuf), 0,
                              (struct sockaddr *)&peer, &peer_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        uint32_t t_rx = now_us();

        gk_verdict_frame_t resp;
        gk_cmd_t applied;

        if (n != GK_CMD_FRAME_LEN) {
            /* wrong-sized datagram: can't even attempt gk_core_process
             * (it assumes a full GK_CMD_FRAME_LEN buffer) -- treat as
             * malformed directly. */
            st.n_total++;
            st.n_malformed++;
            resp.seq = 0;
            resp.verdict = GK_VERDICT_MALFORMED;
            resp.latency_us = now_us() - t_rx;
        } else {
            gk_cmd_t requested;
            int requested_ok = gk_decode_cmd(rxbuf, &requested);
            uint32_t t_verdict = now_us();
            gk_core_process(&st, rxbuf, t_rx, t_verdict, &resp, &applied);

            const char *vstr = resp.verdict == GK_VERDICT_APPROVED ? "APPROVED" :
                                resp.verdict == GK_VERDICT_VETO ? "VETO" : "MALFORMED";
            fprintf(stderr,
                "[gatekeeper-udp] seq=%u verdict=%-8s req_steer=%.2f req_accel=%.2f "
                "applied_steer=%.2f applied_accel=%.2f latency=%uus "
                "(total=%llu veto=%llu malformed=%llu reorder=%llu)\n",
                resp.seq, vstr,
                requested_ok ? requested.steer_deg : 0.0f,
                requested_ok ? requested.accel_mps2 : 0.0f,
                applied.steer_deg, applied.accel_mps2, resp.latency_us,
                (unsigned long long)st.n_total, (unsigned long long)st.n_veto,
                (unsigned long long)st.n_malformed, (unsigned long long)st.n_replay_or_reorder);
        }

        uint8_t out[GK_VERDICT_FRAME_LEN];
        gk_encode_verdict(&resp, out);
        (void)sendto(fd, out, sizeof(out), 0, (struct sockaddr *)&peer, peer_len);
    }

    fprintf(stderr, "\n[gatekeeper-udp] shutting down. summary:\n");
    fprintf(stderr, "  total=%llu approved=%llu veto=%llu malformed=%llu reorder=%llu\n",
            (unsigned long long)st.n_total, (unsigned long long)st.n_approved,
            (unsigned long long)st.n_veto, (unsigned long long)st.n_malformed,
            (unsigned long long)st.n_replay_or_reorder);
    if (st.n_total > 0) {
        fprintf(stderr, "  avg_latency=%.2fus max_latency=%uus\n",
                (double)st.sum_latency_us / (double)st.n_total, st.max_latency_us);
    }

    close(fd);
    return 0;
}
