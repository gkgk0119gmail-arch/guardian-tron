/*
 * gv_imu.c - Guardian-TRON 100 Hz safety monitor: IMU (camera module, I2C1 @0x6A,
 * LSM6DSL/ISM330DLC register map) + Kalman filter -> impact / tilt hazards.
 *
 * Timing: a μT-Kernel cyclic handler (10 ms) wakes this task (priority 10: above the
 * vision task, below the gatekeeper). The period jitter of this task under full NPU load
 * is the RTOS control-loop jitter we report (and D3 toggles every period for a scope).
 *
 * Filter: the board orientation in the car is unknown, so at start the task averages
 * 1 s of samples (car still) and builds a rotation R that maps the measured gravity to
 * +Z. Every sample is rotated by R; roll and pitch relative to the start pose are then
 * estimated by one 2-state Kalman filter each (angle, gyro bias):
 *     predict  angle += (gyro - bias) * dt,  P += Q
 *     update   z = accel angle,  K = P H' / (H P H' + R),  x += K (z - angle)
 * R is inflated when |a| is far from 1 g (the car accelerating/braking/bumping), so the
 * filter trusts the gyro while the accelerometer does not measure gravity.
 *
 * Hazards (raise -> gate brakes, hold GV_IMU_HOLD_MS after the condition ends):
 *   IMPACT : horizontal acceleration > 2.5 g on 2 consecutive samples (collision)
 *   TILT   : |roll| or |pitch| > 30 deg for 100 ms (car lifted, rolled over, wheel off)
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <math.h>
#include "stm32n6xx_hal.h"
#include "stm32n6570_discovery_bus.h"
#include "gv_vision.h"
#include "gv_perf.h"
#include "gt_gate.h"

void gv_i2c_lock(void);
void gv_i2c_unlock(void);

#define IMU_ADDR	(0x6A << 1)
#define REG_WHO_AM_I	0x0F
#define REG_CTRL1_XL	0x10
#define REG_CTRL2_G	0x11
#define REG_CTRL3_C	0x12
#define REG_OUTX_L_G	0x22
#define ACC_LSB_G	(0.244e-3f)		/* +-8 g */
#define GYR_LSB_RADS	(17.5e-3f * 0.017453292f)	/* +-500 dps */

#define GV_IMU_PERIOD_MS	10
#define GV_IMPACT_G		2.5f
#define GV_TILT_DEG		30.0f
#define GV_TILT_SAMPLES		10		/* 100 ms */
#define GV_IMU_HOLD_MS		1000
#define RAD2DEG			57.29578f

typedef struct { float angle, bias, P[2][2]; } kf_t;

/* published to the gate (read in the gate task) */
volatile float gv_imu_roll, gv_imu_pitch, gv_imu_yaw_rate, gv_imu_ahoriz;
volatile UW    gv_imu_ok, gv_imu_samples;
gv_stat_t      gv_perf_imu_period;		/* measured period, cycles */
gv_stat_t      gv_perf_imu_step;		/* I2C read + filter + decision, cycles */

LOCAL volatile UW hazard_until_ms;
LOCAL volatile INT hazard_latched;
LOCAL gt_local_safety_t onset;
LOCAL ID imu_tid;
LOCAL volatile UW release_cyc;

LOCAL UW now_ms(void) { SYSTIM t; tk_get_tim(&t); return t.lo; }

LOCAL void cyc_handler(void *exinf)
{
	release_cyc = DWT->CYCCNT;
	tk_wup_tsk(imu_tid);
}

LOCAL INT rd(UB reg, UB *buf, UH len)
{
	gv_i2c_lock();
	INT r = BSP_I2C1_ReadReg(IMU_ADDR, reg, buf, len);
	gv_i2c_unlock();
	return r;
}
LOCAL INT wr(UB reg, UB val)
{
	gv_i2c_lock();
	INT r = BSP_I2C1_WriteReg(IMU_ADDR, reg, &val, 1);
	gv_i2c_unlock();
	return r;
}

LOCAL INT imu_setup(void)
{
	UB who = 0;
	if (rd(REG_WHO_AM_I, &who, 1) != 0 || who != 0x6A) return -1;
	if (wr(REG_CTRL3_C, 0x44) != 0) return -1;		/* BDU, register auto-increment */
	if (wr(REG_CTRL1_XL, 0x5C) != 0) return -1;		/* 208 Hz, +-8 g */
	if (wr(REG_CTRL2_G, 0x54) != 0) return -1;		/* 208 Hz, 500 dps */
	return 0;
}

LOCAL INT imu_read(float a[3], float w[3])
{
	UB b[12];
	if (rd(REG_OUTX_L_G, b, sizeof b) != 0) return -1;
	for (INT i = 0; i < 3; i++) {
		w[i] = (float)(H)(b[2 * i] | (b[2 * i + 1] << 8)) * GYR_LSB_RADS;
		a[i] = (float)(H)(b[6 + 2 * i] | (b[6 + 2 * i + 1] << 8)) * ACC_LSB_G;
	}
	return 0;
}

LOCAL void kf_init(kf_t *k) { k->angle = 0; k->bias = 0; k->P[0][0] = k->P[1][1] = 1e-3f; k->P[0][1] = k->P[1][0] = 0; }

LOCAL void kf_step(kf_t *k, float rate, float z, float dt, float r_meas)
{
	const float q_angle = 0.001f, q_bias = 0.003f;
	k->angle += dt * (rate - k->bias);
	k->P[0][0] += dt * (dt * k->P[1][1] - k->P[0][1] - k->P[1][0] + q_angle);
	k->P[0][1] -= dt * k->P[1][1];
	k->P[1][0] -= dt * k->P[1][1];
	k->P[1][1] += q_bias * dt;
	float s = k->P[0][0] + r_meas;
	float k0 = k->P[0][0] / s, k1 = k->P[1][0] / s;
	float y = z - k->angle;
	k->angle += k0 * y;
	k->bias  += k1 * y;
	float p00 = k->P[0][0], p01 = k->P[0][1];
	k->P[0][0] -= k0 * p00; k->P[0][1] -= k0 * p01;
	k->P[1][0] -= k1 * p00; k->P[1][1] -= k1 * p01;
}

/* rotation that maps unit vector g onto +Z (Rodrigues) */
LOCAL void rot_to_z(const float g[3], float R[3][3])
{
	float vx = g[1], vy = -g[0];			/* v = g x z */
	float c = g[2], s2 = vx * vx + vy * vy;
	for (INT i = 0; i < 3; i++) for (INT j = 0; j < 3; j++) R[i][j] = (i == j);
	if (s2 < 1e-8f) { if (c < 0) { R[1][1] = -1; R[2][2] = -1; } return; }
	float f = (1.0f - c) / s2;
	/* R = I + [v]x + [v]x^2 f, with v = (vx, vy, 0) */
	R[0][0] = 1 - f * vy * vy;  R[0][1] = f * vx * vy;     R[0][2] = vy;
	R[1][0] = f * vx * vy;      R[1][1] = 1 - f * vx * vx; R[1][2] = -vx;
	R[2][0] = -vy;              R[2][1] = vx;              R[2][2] = 1 - f * s2;
}
LOCAL void mul(float R[3][3], const float v[3], float o[3])
{
	for (INT i = 0; i < 3; i++) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}

/* called by the gatekeeper task */
gt_local_safety_t gv_imu_safety_poll(void)
{
	gt_local_safety_t r = onset;
	r.hazard = hazard_latched && ((W)(hazard_until_ms - now_ms()) > 0);
	if (!r.hazard) hazard_latched = 0;
	return r;
}

LOCAL void raise(INT source, float value, UW t_sample)
{
	hazard_until_ms = now_ms() + GV_IMU_HOLD_MS;
	if (!hazard_latched) {
		onset.event_id = (onset.event_id + 1) & 0x00FFFFFF;
		onset.source = source;
		onset.frame_cyc = t_sample;
		onset.detect_cyc = DWT->CYCCNT;
		onset.collision_probability = 1.0f;
		onset.box_h = value;			/* g (impact) or degrees (tilt) */
		hazard_latched = 1;
		gt_gate_kick();
	}
}

void gv_imu_task(INT stacd, void *exinf)
{
	float R[3][3], gyro_bias[3], a[3], w[3], ar[3], wr_[3];
	kf_t kroll, kpitch;
	imu_tid = tk_get_tid();

	/* the IMU sits on the camera module: wait until the camera bring-up owns a live I2C1 */
	while (!gv_ready) tk_dly_tsk(100);
	for (INT attempt = 1; imu_setup() != 0; attempt++) {
		if (attempt == 1 || attempt % 10 == 0) tm_printf((UB*)"[imu] not answering on I2C1 0x6A (attempt %d)\n", attempt);
		tk_dly_tsk(500);
	}
	tk_dly_tsk(50);

	/* 1 s calibration, car at rest */
	for (;;) {
		float sa[3] = {0}, sw[3] = {0}, sa2 = 0; INT n = 0;
		for (INT i = 0; i < 100; i++) {
			tk_dly_tsk(GV_IMU_PERIOD_MS);
			if (imu_read(a, w) != 0) continue;
			for (INT k = 0; k < 3; k++) { sa[k] += a[k]; sw[k] += w[k]; }
			sa2 += a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
			n++;
		}
		if (n < 80) { tm_printf((UB*)"[imu] calibration: read errors, retrying\n"); continue; }
		float m = sqrtf(sa[0] * sa[0] + sa[1] * sa[1] + sa[2] * sa[2]) / n;
		float var = sa2 / n - m * m;
		if (m < 0.8f || m > 1.2f || var > 0.01f) { tm_printf((UB*)"[imu] calibration: car moving (|g| %d mg), retrying\n", (INT)(m * 1000)); continue; }
		float g[3] = { sa[0] / n / m, sa[1] / n / m, sa[2] / n / m };
		rot_to_z(g, R);
		for (INT k = 0; k < 3; k++) gyro_bias[k] = sw[k] / n;
		tm_printf((UB*)"[imu] ready: gravity in board axes (%d,%d,%d) mg, gyro bias (%d,%d,%d) mdps | hazards: impact >%d.%d g, tilt >%d deg\n",
			  (INT)(g[0] * m * 1000), (INT)(g[1] * m * 1000), (INT)(g[2] * m * 1000),
			  (INT)(gyro_bias[0] * RAD2DEG * 1000), (INT)(gyro_bias[1] * RAD2DEG * 1000), (INT)(gyro_bias[2] * RAD2DEG * 1000),
			  (INT)GV_IMPACT_G, (INT)(GV_IMPACT_G * 10) % 10, (INT)GV_TILT_DEG);
		break;
	}
	kf_init(&kroll); kf_init(&kpitch);
	gv_imu_ok = 1;

	T_CCYC cc = { .cycatr = TA_HLNG | TA_STA, .cychdr = cyc_handler, .cyctim = GV_IMU_PERIOD_MS, .cycphs = GV_IMU_PERIOD_MS };
	tk_cre_cyc(&cc);

	UW prev_start = 0, prev_cyc = 0; INT impact_n = 0, tilt_n = 0, errs = 0;
	UW t_rep = now_ms();
	while (1) {
		tk_slp_tsk(TMO_FEVR);
		UW t0 = DWT->CYCCNT;
		gv_probe_toggle(GV_PROBE_TICK);
		if (prev_start) {
			gv_stat_add(&gv_perf_imu_period, t0 - prev_start);
			W dev = (W)GV_CYC2NS(t0 - prev_start) - GV_IMU_PERIOD_MS * 1000000;
			gv_metric_add(GV_M_IMU_JITTER, (UW)(dev < 0 ? -dev : dev));
		}
		prev_start = t0;

		if (imu_read(a, w) != 0) { errs++; continue; }
		UW t_sample = DWT->CYCCNT;
		for (INT k = 0; k < 3; k++) w[k] -= gyro_bias[k];
		mul(R, a, ar); mul(R, w, wr_);
		float dt = prev_cyc ? (float)(t_sample - prev_cyc) / (float)SystemCoreClock : GV_IMU_PERIOD_MS * 1e-3f;
		prev_cyc = t_sample;

		float an = sqrtf(ar[0] * ar[0] + ar[1] * ar[1] + ar[2] * ar[2]);
		float dev = an - 1.0f;
		float r_meas = 0.03f * (1.0f + 400.0f * dev * dev);	/* distrust accel angle off 1 g */
		kf_step(&kroll,  wr_[0], atan2f(ar[1], ar[2]), dt, r_meas);
		kf_step(&kpitch, wr_[1], atan2f(-ar[0], sqrtf(ar[1] * ar[1] + ar[2] * ar[2])), dt, r_meas);

		float roll = kroll.angle * RAD2DEG, pitch = kpitch.angle * RAD2DEG;
		float ah = sqrtf(ar[0] * ar[0] + ar[1] * ar[1]);
		gv_imu_roll = roll; gv_imu_pitch = pitch; gv_imu_yaw_rate = wr_[2] * RAD2DEG; gv_imu_ahoriz = ah;
		gv_imu_samples++;

		impact_n = (ah > GV_IMPACT_G) ? impact_n + 1 : 0;
		INT tilted = fabsf(roll) > GV_TILT_DEG || fabsf(pitch) > GV_TILT_DEG;
		tilt_n = tilted ? tilt_n + 1 : 0;
		if (impact_n >= 2) raise(GT_SRC_IMPACT, ah, t_sample);
		else if (tilt_n >= GV_TILT_SAMPLES) raise(GT_SRC_TILT, fabsf(roll) > fabsf(pitch) ? roll : pitch, t_sample);
		else if (hazard_latched && (tilted || impact_n)) hazard_until_ms = now_ms() + GV_IMU_HOLD_MS;

		gv_stat_add(&gv_perf_imu_step, DWT->CYCCNT - t0);
		gv_metric_add(GV_M_IMU_STEP, GV_CYC2NS(DWT->CYCCNT - t0));
		if ((UW)(DWT->CYCCNT - release_cyc) > GV_IMU_PERIOD_MS * (SystemCoreClock / 1000U))
			gv_metric_miss(GV_M_IMU_JITTER);	/* finished after the next release: deadline miss */

		UW ms = now_ms();
		if (ms - t_rep >= 5000) {
			t_rep = ms;
			gv_stat_t p = gv_perf_imu_period, s = gv_perf_imu_step;
			gv_stat_reset(&gv_perf_imu_period); gv_stat_reset(&gv_perf_imu_step);
			UW avg = p.n ? (UW)(p.sum / p.n) : 0;
			W jmax = (W)GV_CYC2NS(p.max) - GV_IMU_PERIOD_MS * 1000000, jmin = (W)GV_CYC2NS(p.min) - GV_IMU_PERIOD_MS * 1000000;
			tm_printf((UB*)"[imu] roll %d pitch %d deg, yaw %d dps, a_h %d mg | 100 Hz period n=%d avg %d us, jitter min %d ns max %d ns | step max %d us | i2c err %d\n",
				  (INT)roll, (INT)pitch, (INT)(wr_[2] * RAD2DEG), (INT)(ah * 1000),
				  (INT)p.n, (INT)GV_CYC2US(avg), (INT)jmin, (INT)jmax, (INT)GV_CYC2US(s.max), (INT)errs);
		}
	}
}
