#ifndef GK_GATEKEEPER_CORE_H
#define GK_GATEKEEPER_CORE_H

#include <stdint.h>
#include "protocol.h"
#include "safety_envelope.h"

typedef struct {
    gk_envelope_state_t env;
    uint32_t last_seq;
    int has_last_seq;
    uint32_t last_rx_time_us;
    int has_last_rx_time;

    /* stats, mirrors <표 2>/<표 4> of the paper */
    uint64_t n_total;
    uint64_t n_approved;
    uint64_t n_veto;
    uint64_t n_malformed;
    uint64_t n_replay_or_reorder;   /* threat-2-style anomaly */
    uint32_t max_latency_us;
    uint64_t sum_latency_us;
} gk_core_state_t;

void gk_core_init(gk_core_state_t *st);

/* Processes one already-framed 15-byte CMD buffer received at t_rx_us and
 * judged at t_verdict_us (both from a monotonic microsecond clock supplied
 * by the caller -- DWT cycle counter on the real MCU, clock_gettime() in the
 * SIM harness). Fills *resp (to send back over the link) and *applied (the
 * command that should actually reach the actuator this cycle). */
void gk_core_process(gk_core_state_t *st,
                      const uint8_t raw_cmd[GK_CMD_FRAME_LEN],
                      uint32_t t_rx_us,
                      uint32_t t_verdict_us,
                      gk_verdict_frame_t *resp,
                      gk_cmd_t *applied);

#endif /* GK_GATEKEEPER_CORE_H */
