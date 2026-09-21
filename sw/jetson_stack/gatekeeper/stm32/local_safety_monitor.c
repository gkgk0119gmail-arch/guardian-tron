#include "local_safety_monitor.h"

/* Threshold for "obstacle imminent" -- placeholder, matches the paper's
 * example NPU collision-probability target (0.85) but should be tuned
 * once the real model is trained/deployed. */
#define COLLISION_PROB_THRESHOLD 0.85f

void local_safety_init(void) {
    /* STUB: this is where CubeMX's camera/ISP init (e.g. BSP_CAMERA_Init)
     * and the X-CUBE-AI generated model init (e.g. network_init()) would
     * be called, once that CubeMX integration exists. Nothing to init
     * yet -- no-op. */
}

local_safety_reading_t local_safety_poll(void) {
    /* STUB: replace this body with:
     *   1. grab the latest camera frame (or let DMA/ISP double-buffering
     *      hand you the last completed one -- don't block here),
     *   2. run the X-CUBE-AI generated inference entrypoint on it,
     *   3. read the model's collision-probability output.
     * Until then this always reports "clear," so the gatekeeper's
     * Jetson-link envelope check (gk_core_process) is the only active
     * safety layer -- exactly like the current SIM/UDP testing setup,
     * just running on real MCU hardware. */
    local_safety_reading_t r;
    r.collision_probability = 0.0f;
    r.obstacle_imminent = (r.collision_probability >= COLLISION_PROB_THRESHOLD);
    return r;
}
