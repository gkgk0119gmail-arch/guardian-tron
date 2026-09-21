#ifndef GATEKEEPER_LOCAL_SAFETY_MONITOR_H
#define GATEKEEPER_LOCAL_SAFETY_MONITOR_H

/* STM32's OWN independent hazard sensor -- the poster's "SafetyMonitor" /
 * "CamVision" tasks: the STM32N6570-DK's ONBOARD camera + Neural-ART NPU
 * running a lightweight collision-probability model, completely separate
 * from the Jetson's LiDAR (jetson/ecu/lidar.py). The Jetson keeps using
 * its own LiDAR for path planning; this is STM32's independent second
 * opinion, so its safety judgment doesn't depend on any sensor data that
 * passes through the (potentially compromised/frozen) Jetson.
 *
 * STUB STATUS: local_safety_poll() always reports "no hazard" until the
 * real camera + NPU pipeline replaces the body. That pipeline needs:
 *   1. CubeMX: enable the camera peripheral (DCMIPP) + ISP middleware for
 *      this board's onboard camera module.
 *   2. X-CUBE-AI: a trained+quantized TinyML collision model (same idea as
 *      the paper's CAN-IDS model, but image input) deployed via
 *      STM32Cube.AI, generating network.c/network.h you'd call from here.
 *   3. Wire the model's output score into local_safety_poll() below.
 * All three are a separate, substantial CubeMX/X-CUBE-AI integration step
 * -- this stub keeps the rest of the firmware (gatekeeper_uart_bridge.c)
 * fully wired and buildable/runnable NOW without blocking on it. Search
 * for "STUB:" in local_safety_monitor.c for exactly what to replace.
 */

typedef struct {
    int obstacle_imminent;        /* 1 = collision_probability >= COLLISION_PROB_THRESHOLD */
    float collision_probability;  /* NPU model output, 0.0-1.0, 0 while stubbed */
} local_safety_reading_t;

void local_safety_init(void);

/* Call once per gatekeeper cycle (or at the camera/NPU's own native rate,
 * e.g. 5Hz per the poster's CamVision task, whichever is slower --
 * caching the last inference result between calls if needed). Must not
 * block the gatekeeper's microsecond-scale response budget; a real NPU
 * inference (per the paper, ~90us-class latency on similar models) should
 * run asynchronously and this function should just read the latest
 * completed result. */
local_safety_reading_t local_safety_poll(void);

#endif /* GATEKEEPER_LOCAL_SAFETY_MONITOR_H */
