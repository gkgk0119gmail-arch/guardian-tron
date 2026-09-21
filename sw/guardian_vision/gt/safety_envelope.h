#ifndef GK_SAFETY_ENVELOPE_H
#define GK_SAFETY_ENVELOPE_H

#include <stdint.h>
#include "protocol.h"

/* Envelope for THIS vehicle (F1TENTH car2, measured 2026-09-13).
 * The CMD frame's second float is used as target SPEED [m/s] (field name kept as
 * accel_mps2 for byte compatibility with jetson/ecu/protocol.py). */

/* Absolute limits (eq. 2) */
#define GK_STEER_ABS_LIMIT_DEG   17.5f    /* car2 mechanical: +17.57 / -27.49 deg, symmetric soft cap */
#define GK_ACCEL_MIN_MPS2        (-0.5f)  /* speed: slow reverse allowed (stuck recovery) */
#define GK_ACCEL_MAX_MPS2        2.0f     /* speed hard cap for the demo */

/* Rate limits per control cycle at 100 Hz (eq. 3) */
#define GK_STEER_RATE_LIMIT_DEG  3.0f     /* 300 deg/s */
#define GK_ACCEL_RATE_LIMIT_MPS2 0.05f    /* 5 m/s^2 */

typedef struct {
    gk_cmd_t last_safe;   /* u_safe(k-1) */
    int has_last_safe;    /* 0 until first APPROVED command seen */
} gk_envelope_state_t;

void gk_envelope_init(gk_envelope_state_t *st);

/* Implements eq. (4): deterministic verdict + output command.
 * Returns the verdict and writes the command that should actually be
 * applied to the actuator (u_out(k)) into *out. */
gk_verdict_t gk_envelope_check(gk_envelope_state_t *st,
                                const gk_cmd_t *cmd_in,
                                gk_cmd_t *out);

#endif /* GK_SAFETY_ENVELOPE_H */
