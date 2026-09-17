/* SIM-mode gatekeeper: runs the exact same gatekeeper_core.c /
 * safety_envelope.c / protocol.c used on the real STM32N6570-DK, but talks
 * over a Linux pseudo-terminal (PTY) instead of a real UART peripheral.
 *
 * The Jetson-side ECU (jetson/ecu/uart_link.py) opens the PTY's slave path
 * exactly like it would open /dev/ttyACM0 on the real board, so the whole
 * command -> gatekeeper -> verdict -> actuator loop, and all three threat
 * scripts, work end-to-end today without the physical MCU flashed.
 *
 * Port to the real board: replace pty_open()/pty_read()/pty_write() and
 * now_us() in this file with STM32 HAL UART + DWT cycle counter calls; the
 * gatekeeper_core.c call sequence below is what a µT-Kernel task body would
 * wrap in a cyclic handler. Nothing in gatekeeper_core.c / safety_envelope.c
 * / protocol.c needs to change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>

#include "protocol.h"
#include "safety_envelope.h"
#include "gatekeeper_core.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint32_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}

static int pty_open(char *slave_path_out, size_t slave_path_len) {
    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("posix_openpt"); return -1; }
    if (grantpt(fd) != 0) { perror("grantpt"); return -1; }
    if (unlockpt(fd) != 0) { perror("unlockpt"); return -1; }
    const char *sp = ptsname(fd);
    if (!sp) { perror("ptsname"); return -1; }
    strncpy(slave_path_out, sp, slave_path_len - 1);
    slave_path_out[slave_path_len - 1] = '\0';

    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

/* Publishes a stable symlink (default /tmp/stm32_gatekeeper) to the PTY's
 * randomly-numbered slave device, so scripts don't have to scrape stdout. */
static void publish_symlink(const char *target, const char *link_path) {
    unlink(link_path);
    if (symlink(target, link_path) != 0) {
        perror("symlink");
    }
}

int main(int argc, char **argv) {
    const char *link_path = (argc > 1) ? argv[1] : "/tmp/stm32_gatekeeper";

    char slave[256];
    int fd = pty_open(slave, sizeof(slave));
    if (fd < 0) return 1;
    publish_symlink(slave, link_path);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    fprintf(stderr, "[gatekeeper-sim] PTY slave: %s\n", slave);
    fprintf(stderr, "[gatekeeper-sim] symlink:   %s\n", link_path);
    fprintf(stderr, "[gatekeeper-sim] safety envelope: |steer|<=%.0fdeg accel in [%.0f,%.0f] "
                     "rate<=%.0fdeg/cycle,%.0fm/s^2/cycle\n",
            GK_STEER_ABS_LIMIT_DEG, GK_ACCEL_MIN_MPS2, GK_ACCEL_MAX_MPS2,
            GK_STEER_RATE_LIMIT_DEG, GK_ACCEL_RATE_LIMIT_MPS2);

    gk_core_state_t st;
    gk_core_init(&st);

    uint8_t buf[GK_CMD_FRAME_LEN];
    size_t have = 0;

    while (!g_stop) {
        uint8_t byte;
        ssize_t n = read(fd, &byte, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        if (have == 0 && byte != GK_CMD_STX) {
            continue; /* resync: discard noise until STX */
        }
        buf[have++] = byte;

        if (have == GK_CMD_FRAME_LEN) {
            uint32_t t_rx = now_us();

            if (buf[GK_CMD_FRAME_LEN - 1] != GK_ETX) {
                /* lost framing: shift by one and keep resyncing */
                memmove(buf, buf + 1, GK_CMD_FRAME_LEN - 1);
                have = GK_CMD_FRAME_LEN - 1;
                continue;
            }

            gk_cmd_t requested;
            int requested_ok = gk_decode_cmd(buf, &requested);

            gk_verdict_frame_t resp;
            gk_cmd_t applied;
            uint32_t t_verdict = now_us();
            gk_core_process(&st, buf, t_rx, t_verdict, &resp, &applied);

            uint8_t out[GK_VERDICT_FRAME_LEN];
            gk_encode_verdict(&resp, out);
            (void)write(fd, out, sizeof(out));

            const char *vstr = resp.verdict == GK_VERDICT_APPROVED ? "APPROVED" :
                                resp.verdict == GK_VERDICT_VETO ? "VETO" : "MALFORMED";
            fprintf(stderr,
                "[gatekeeper-sim] seq=%u verdict=%-8s req_steer=%.2f req_accel=%.2f "
                "applied_steer=%.2f applied_accel=%.2f latency=%uus "
                "(total=%llu veto=%llu malformed=%llu reorder=%llu)\n",
                resp.seq, vstr,
                requested_ok ? requested.steer_deg : 0.0f,
                requested_ok ? requested.accel_mps2 : 0.0f,
                applied.steer_deg, applied.accel_mps2, resp.latency_us,
                (unsigned long long)st.n_total, (unsigned long long)st.n_veto,
                (unsigned long long)st.n_malformed, (unsigned long long)st.n_replay_or_reorder);

            have = 0;
        }
    }

    fprintf(stderr, "\n[gatekeeper-sim] shutting down. summary:\n");
    fprintf(stderr, "  total=%llu approved=%llu veto=%llu malformed=%llu reorder=%llu\n",
            (unsigned long long)st.n_total, (unsigned long long)st.n_approved,
            (unsigned long long)st.n_veto, (unsigned long long)st.n_malformed,
            (unsigned long long)st.n_replay_or_reorder);
    if (st.n_total > 0) {
        fprintf(stderr, "  avg_latency=%.2fus max_latency=%uus\n",
                (double)st.sum_latency_us / (double)st.n_total, st.max_latency_us);
    }

    unlink(link_path);
    close(fd);
    return 0;
}
