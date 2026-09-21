#include "safety_envelope.h"

static float fabs_f(float v) { return v < 0.0f ? -v : v; }

void gk_envelope_init(gk_envelope_state_t *st) {
    st->last_safe.seq = 0;
    st->last_safe.steer_deg = 0.0f;
    st->last_safe.accel_mps2 = 0.0f;
    st->has_last_safe = 0;
}

static int within_absolute(const gk_cmd_t *c) {
    if (fabs_f(c->steer_deg) > GK_STEER_ABS_LIMIT_DEG) return 0;
    if (c->accel_mps2 < GK_ACCEL_MIN_MPS2 || c->accel_mps2 > GK_ACCEL_MAX_MPS2) return 0;
    return 1;
}

static int within_rate(const gk_envelope_state_t *st, const gk_cmd_t *c) {
    if (!st->has_last_safe) return 1; /* nothing to compare against yet */
    float dsteer = fabs_f(c->steer_deg - st->last_safe.steer_deg);
    float daccel = fabs_f(c->accel_mps2 - st->last_safe.accel_mps2);
    if (dsteer > GK_STEER_RATE_LIMIT_DEG) return 0;
    if (daccel > GK_ACCEL_RATE_LIMIT_MPS2) return 0;
    return 1;
}

gk_verdict_t gk_envelope_check(gk_envelope_state_t *st,
                                const gk_cmd_t *cmd_in,
                                gk_cmd_t *out) {
    int ok = within_absolute(cmd_in) && within_rate(st, cmd_in);

    if (ok) {
        st->last_safe = *cmd_in;
        st->has_last_safe = 1;
        *out = *cmd_in;
        return GK_VERDICT_APPROVED;
    }

    /* Violation: hold last known-safe command (fail-safe), reject this one. */
    if (st->has_last_safe) {
        *out = st->last_safe;
    } else {
        out->seq = cmd_in->seq;
        out->steer_deg = 0.0f;
        out->accel_mps2 = 0.0f;
    }
    return GK_VERDICT_VETO;
}
