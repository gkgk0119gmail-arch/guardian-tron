#include "safety_envelope.h"

/* Guardian-TRON variant of eq. (4): instead of holding u_safe(k-1) forever after a
 * violation (which on a real car means "keep driving the old command while the
 * Jetson is rejected"), the command is CLAMPED to the envelope and to the rate limit
 * around u_safe(k-1). The verdict is still VETO whenever clamping changed the value,
 * so the statistics and the Jetson-side threat demos keep their meaning, but the
 * output always converges to what the Jetson asks for at a bounded rate. */

static float fabs_f(float v) { return v < 0.0f ? -v : v; }
static int   is_nan(float v) { return v != v; }

void gk_envelope_init(gk_envelope_state_t *st) {
    st->last_safe.seq = 0;
    st->last_safe.steer_deg = 0.0f;
    st->last_safe.accel_mps2 = 0.0f;
    st->has_last_safe = 0;
}

static int clamp_abs(float *v, float lo, float hi) {
    if (*v < lo) { *v = lo; return 1; }
    if (*v > hi) { *v = hi; return 1; }
    return 0;
}

static int clamp_rate(float *v, float ref, float rate) {
    if (*v > ref + rate) { *v = ref + rate; return 1; }
    if (*v < ref - rate) { *v = ref - rate; return 1; }
    return 0;
}

gk_verdict_t gk_envelope_check(gk_envelope_state_t *st,
                                const gk_cmd_t *cmd_in,
                                gk_cmd_t *out) {
    gk_cmd_t c = *cmd_in;
    int clamped = 0;

    /* NaN/Inf never reach the actuator: treat as a full violation, hold last safe. */
    if (is_nan(c.steer_deg) || is_nan(c.accel_mps2) ||
        fabs_f(c.steer_deg) > 1e6f || fabs_f(c.accel_mps2) > 1e6f) {
        if (st->has_last_safe) { *out = st->last_safe; }
        else { out->seq = c.seq; out->steer_deg = 0.0f; out->accel_mps2 = 0.0f; }
        return GK_VERDICT_VETO;
    }

    clamped |= clamp_abs(&c.steer_deg, -GK_STEER_ABS_LIMIT_DEG, GK_STEER_ABS_LIMIT_DEG);
    clamped |= clamp_abs(&c.accel_mps2, GK_ACCEL_MIN_MPS2, GK_ACCEL_MAX_MPS2);

    if (st->has_last_safe) {
        clamped |= clamp_rate(&c.steer_deg,  st->last_safe.steer_deg,  GK_STEER_RATE_LIMIT_DEG);
        clamped |= clamp_rate(&c.accel_mps2, st->last_safe.accel_mps2, GK_ACCEL_RATE_LIMIT_MPS2);
    }

    st->last_safe = c;
    st->has_last_safe = 1;
    *out = c;
    return clamped ? GK_VERDICT_VETO : GK_VERDICT_APPROVED;
}
