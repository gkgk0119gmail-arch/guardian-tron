/*
 * gv_vision.h - Guardian-TRON camera + NPU person detector (ST OD pipeline, no OS calls)
 *
 * Plain-C boundary between the ST vision code (st_vision.c) and the μT-Kernel side
 * (gv_task.c). Everything here is called from one task; the only OS service the
 * ST side needs is gv_os_sleep_ms(), provided by the kernel side.
 */
#ifndef GV_VISION_H
#define GV_VISION_H
#include <stdint.h>

#define GV_MAX_BOXES 8

typedef struct {
  float x, y, w, h;          /* box center and size, normalized to the NN input (0..1) */
  float conf;                /* 0..1 */
} gv_box_t;

typedef struct {
  int      nb;               /* person detections after NMS (capped at GV_MAX_BOXES) */
  gv_box_t box[GV_MAX_BOXES];
  uint32_t frame_cyc;        /* DWT cycle count when the NN frame finished landing in RAM */
  uint32_t infer_cyc;        /* NPU inference duration, cycles */
  uint32_t done_cyc;         /* DWT cycle count when boxes were decoded */
} gv_result_t;

void gv_hw_init(void);                        /* clocks, caches, NPU RAM, xSPI NOR/PSRAM, RIF; before the kernel */
int  gv_vision_init(void);                    /* NN runtime, post-processing, camera, LCD; from the vision task */
int  gv_vision_step(gv_result_t *r);          /* one frame: capture -> NPU -> decode. 0 = ok, <0 = no frame */
void gv_vision_draw(const gv_result_t *r, int hazard, uint32_t stop_count, uint32_t fps_x10);

extern volatile uint32_t gv_frame_cyc;        /* stamped in the DCMIPP frame callback */
extern volatile uint32_t gv_stage;            /* where the vision task is (watched by the gate task) */
extern volatile uint32_t gv_frames;           /* completed frames */
extern volatile uint32_t gv_cam_err;          /* DCMIPP pipe error callbacks */
extern volatile uint32_t gv_isp_err;          /* sensor I2C failures during AE/AWB (non-fatal) */
extern volatile uint32_t gv_ready;            /* 1 once the camera + NPU pipeline produces frames */
enum { GV_ST_ISP = 1, GV_ST_START, GV_ST_WAIT, GV_ST_NPU, GV_ST_PP, GV_ST_DECIDE, GV_ST_DRAW };
extern void gv_os_sleep_ms(uint32_t ms);      /* kernel side */

#endif
