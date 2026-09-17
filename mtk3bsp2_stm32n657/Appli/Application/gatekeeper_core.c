#include "gatekeeper_core.h"

void gk_core_init(gk_core_state_t *st) {
    gk_envelope_init(&st->env);
    st->last_seq = 0;
    st->has_last_seq = 0;
    st->last_rx_time_us = 0;
    st->has_last_rx_time = 0;
    st->n_total = 0;
    st->n_approved = 0;
    st->n_veto = 0;
    st->n_malformed = 0;
    st->n_replay_or_reorder = 0;
    st->max_latency_us = 0;
    st->sum_latency_us = 0;
}

void gk_core_process(gk_core_state_t *st,
                      const uint8_t raw_cmd[GK_CMD_FRAME_LEN],
                      uint32_t t_rx_us,
                      uint32_t t_verdict_us,
                      gk_verdict_frame_t *resp,
                      gk_cmd_t *applied) {
    st->n_total++;
    gk_cmd_t cmd;
    uint32_t latency_us = t_verdict_us - t_rx_us; /* wraps safely (unsigned) */

    if (!gk_decode_cmd(raw_cmd, &cmd)) {
        /* Corrupted / tampered frame (bad CRC or bad framing bytes):
         * definitionally rejected, hold last safe command. */
        st->n_malformed++;
        if (st->env.has_last_safe) {
            *applied = st->env.last_safe;
        } else {
            applied->seq = 0;
            applied->steer_deg = 0.0f;
            applied->accel_mps2 = 0.0f;
        }
        resp->seq = 0;
        resp->verdict = GK_VERDICT_MALFORMED;
        resp->latency_us = latency_us;
        goto stats;
    }

    /* Replay / out-of-order detection (threat-2-style injection often
     * replays a stale-but-otherwise-in-envelope frame). A non-monotonic seq
     * is rejected outright regardless of the envelope check -- an
     * envelope-only check would happily approve a replayed "safe-looking"
     * command, which is exactly the gap a replay attack exploits. */
    if (st->has_last_seq && cmd.seq <= st->last_seq) {
        st->n_replay_or_reorder++;
        st->n_veto++;
        if (st->env.has_last_safe) {
            *applied = st->env.last_safe;
        } else {
            *applied = cmd;
            applied->steer_deg = 0.0f;
            applied->accel_mps2 = 0.0f;
        }
        resp->seq = cmd.seq;
        resp->verdict = GK_VERDICT_VETO;
        resp->latency_us = latency_us;
        goto stats;
    }
    st->last_seq = cmd.seq;
    st->has_last_seq = 1;

    gk_verdict_t v = gk_envelope_check(&st->env, &cmd, applied);

    resp->seq = cmd.seq;
    resp->verdict = v;
    resp->latency_us = latency_us;

    if (v == GK_VERDICT_APPROVED) {
        st->n_approved++;
    } else {
        st->n_veto++;
    }

stats:
    if (latency_us > st->max_latency_us) {
        st->max_latency_us = latency_us;
    }
    st->sum_latency_us += latency_us;
}
