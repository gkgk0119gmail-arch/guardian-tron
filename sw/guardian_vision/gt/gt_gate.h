/* gt_gate.h - Guardian-TRON gatekeeper task API */
#ifndef GT_GATE_H
#define GT_GATE_H
#include <tk/tkernel.h>

enum { GT_SRC_PERSON = 0, GT_SRC_CAMERA_LOST, GT_SRC_IMPACT, GT_SRC_TILT, GT_SRC_MPU };

typedef struct {
	INT   hazard;			/* 1 = stop the car now */
	INT   source;			/* GT_SRC_* */
	float collision_probability;	/* 0..1, from camera/NPU/ToF */
	UW    event_id;			/* increments on every new hazard onset */
	UW    frame_cyc;		/* DWT cycles: camera frame landed (onset event) */
	UW    detect_cyc;		/* DWT cycles: NPU + decode done, hazard raised (onset event) */
	float box_h;			/* person box height, fraction of the frame (onset event) */
} gt_local_safety_t;

typedef struct {
	INT   stopped;
	INT   last_verdict;
	UW    last_latency_us;
	float servo;			/* last servo position sent (0..1) */
	float speed;			/* last target speed sent (m/s) */
} gt_gate_status_t;

void gt_gate_task(INT stacd, void *exinf);
void gt_report_task(INT stacd, void *exinf);		/* console reports; create with a low priority (25) */		/* create with itskpri ~8, stksz 4096 */
const gt_gate_status_t *gt_gate_status(void);
gt_local_safety_t gt_local_safety_poll(void);		/* weak stub in gt_gate.c; override in the camera task */
void gt_gate_kick(void);				/* make the gate re-check local safety now */
void *gt_gate_private_addr(void);			/* fault-injection target (MPU demo) */
gt_local_safety_t gv_imu_safety_poll(void);		/* IMU impact/tilt monitor (gv_imu.c) */

/* person distance governor input from the vision task: tallest person box in the corridor */
extern volatile float gv_person_h;			/* box height / frame height, 0 = none */
extern volatile UW    gv_person_ms;			/* when it was seen (kernel ms) */

#endif
