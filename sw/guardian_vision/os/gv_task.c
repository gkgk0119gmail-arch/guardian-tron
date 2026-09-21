/*
 * gv_task.c - Guardian-TRON local safety: camera + NPU person detector (μT-Kernel task)
 *
 * The vision task runs at LOW priority: capture -> NPU inference (st_yolo_x_nano_480,
 * class "person") -> decision. When a person is in the driving corridor and close
 * enough, it raises the hazard and kicks the gatekeeper task (HIGH priority), which
 * preempts it immediately and puts the brake frame on the VESC link. The Jetson is not
 * in this loop at all.
 *
 * "Close enough" = the box is tall (a near person fills the frame height) and its
 * center is inside the corridor ahead of the car.
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "stm32n6xx_hal.h"
#include "gv_vision.h"
#include "gt_gate.h"
#include "gv_perf.h"

#define GV_MIN_CONF        0.60f   /* person score (post-processing already drops < 0.6) */
#define GV_MIN_BOX_H       0.42f   /* box height / frame height: real person within ~2.0 m (K = 0.84 m, measured 9/21) */
#define GV_CORRIDOR_HALF   0.35f   /* |x_center - 0.5| <= this: in front of the car */
#define GV_HOLD_MS         1500    /* keep the car stopped this long after the last sighting */

static volatile UW  hazard_until_ms;
static volatile INT hazard_latched;
static gt_local_safety_t onset;         /* published at hazard onset */
static UW stop_count;
volatile float gv_person_h;             /* distance governor input (gt_gate.c) */
volatile UW    gv_person_ms;

/* MPU fault injection: USER1 button (PC13) -> this task writes into the gatekeeper's
 * private memory, as a buggy/compromised AI task would. The MPU blocks the store and
 * the gatekeeper stops the car. */
static void rogue_write_check(void)
{
  static int prev;
  int now = (GPIOC->IDR & GPIO_PIN_13) != 0;
  if (now && !prev) {
    tm_printf((UB*)"[vision] FAULT INJECTION: writing into gatekeeper memory at %x\n", (UW)gt_gate_private_addr());
    *(volatile UW *)gt_gate_private_addr() = 0xDEADBEEF;
    tm_printf((UB*)"[vision] ...store returned (blocked by the MPU if it is on)\n");
  }
  prev = now;
}

static UW now_ms(void) { SYSTIM t; tk_get_tim(&t); return t.lo; }

/* called by the gatekeeper task (higher priority) */
gt_local_safety_t gt_local_safety_poll(void)
{
  gt_local_safety_t r = onset;
  r.hazard = hazard_latched && ((W)(hazard_until_ms - now_ms()) > 0);
  if (!r.hazard) hazard_latched = 0;
  return r;
}

void gv_vision_task(INT stacd, void *exinf)
{
  gv_result_t res;
  UW frames = 0, misses = 0, fps_x10 = 0, t_stat = now_ms(), infer_max_us = 0;

  /* camera bring-up with retries: the gate keeps the car stopped until frames flow */
  for (INT attempt = 1; gv_vision_init() != 0; attempt++) {
    tm_printf((UB*)"[vision] camera init attempt %d failed - retrying in 1 s (car held stopped)\n", attempt);
    tk_dly_tsk(1000);
  }
  { extern void gv_bc(unsigned long); gv_bc(0xB0070020); }
  tm_printf((UB*)"[vision] ready: person stop if conf>=%d%% h>=%d%% |x-0.5|<=%d%%, hold %d ms\n",
            (INT)(GV_MIN_CONF * 100), (INT)(GV_MIN_BOX_H * 100), (INT)(GV_CORRIDOR_HALF * 100), GV_HOLD_MS);

  while (1) {
    if (gv_vision_step(&res) < 0) { misses++; continue; }
    frames++;

    rogue_write_check();

    /* pick the most threatening person: tallest box inside the corridor */
    INT best = -1; float best_h = 0.0f, near_h = 0.0f;
    for (INT i = 0; i < res.nb; i++) {
      const gv_box_t *b = &res.box[i];
      float dx = b->x - 0.5f; if (dx < 0) dx = -dx;
      if (b->conf < GV_MIN_CONF || dx > GV_CORRIDOR_HALF) continue;
      if (b->h > near_h) near_h = b->h;
      if (b->h >= GV_MIN_BOX_H && b->h > best_h) { best = i; best_h = b->h; }
    }
    gv_person_h = near_h;                 /* far persons: the gate slows the car down */
    if (near_h > 0.0f) gv_person_ms = now_ms();

    if (best >= 0) {
      hazard_until_ms = now_ms() + GV_HOLD_MS;
      if (!hazard_latched) {
        onset.event_id++;
        onset.source = GT_SRC_PERSON;
        onset.frame_cyc = res.frame_cyc;
        onset.detect_cyc = res.done_cyc;
        onset.collision_probability = res.box[best].conf;
        onset.box_h = best_h;
        hazard_latched = 1;
        stop_count++;
        gt_gate_kick();           /* gatekeeper preempts this task right here */
      }
    }

    INT hazard_now = hazard_latched && ((W)(hazard_until_ms - now_ms()) > 0);
    gv_vision_draw(&res, hazard_now, stop_count, fps_x10);

    gv_metric_add(GV_M_NPU, GV_CYC2NS(res.infer_cyc));
    gv_metric_add(GV_M_FRAME2DET, GV_CYC2NS(res.done_cyc - res.frame_cyc));
    UW infer_us = (UW)(((UD)res.infer_cyc * 1000000ULL) / SystemCoreClock);
    if (infer_us > infer_max_us) infer_max_us = infer_us;
    UW ms = now_ms();
    if (ms - t_stat >= 1000) {
      fps_x10 = frames * 10000 / (ms - t_stat);
      float top_h = 0.0f;
      for (INT i = 0; i < res.nb; i++) if (res.box[i].h > top_h) top_h = res.box[i].h;
      tm_printf((UB*)"[vision] %d.%d fps | NPU %d us (max %d) | persons %d (tallest h %d%%) | %s | misses %d isp_err %d\n",
                (INT)(fps_x10 / 10), (INT)(fps_x10 % 10), (INT)infer_us, (INT)infer_max_us, res.nb,
                (INT)(top_h * 100), hazard_now ? "HAZARD" : "clear", (INT)misses, (INT)gv_isp_err);
      frames = 0; t_stat = ms; infer_max_us = 0;
    }
  }
}
