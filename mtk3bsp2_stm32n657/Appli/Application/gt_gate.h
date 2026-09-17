/* gt_gate.h - Guardian-TRON gatekeeper task API */
#ifndef GT_GATE_H
#define GT_GATE_H
#include <tk/tkernel.h>

typedef struct {
	INT   hazard;			/* 1 = stop the car now */
	float collision_probability;	/* 0..1, from camera/NPU/ToF */
} gt_local_safety_t;

typedef struct {
	INT   stopped;
	INT   last_verdict;
	UW    last_latency_us;
	float servo;			/* last servo position sent (0..1) */
	float speed;			/* last target speed sent (m/s) */
} gt_gate_status_t;

void gt_gate_task(INT stacd, void *exinf);		/* create with itskpri ~8, stksz 4096 */
const gt_gate_status_t *gt_gate_status(void);
gt_local_safety_t gt_local_safety_poll(void);		/* weak stub in gt_gate.c; override in the camera task */

#endif
