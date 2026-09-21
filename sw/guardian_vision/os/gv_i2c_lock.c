/*
 * gv_i2c_lock.c - I2C1 is shared by the camera sensor (IMX335 exposure/gain updates from
 * the vision task) and the IMU (100 Hz safety monitor task). ST's BSP bus layer has no
 * locking, so the camera's calls are wrapped at link time (-Wl,--wrap=BSP_I2C1_*) and
 * both users take one μT-Kernel mutex. TA_INHERIT: when the IMU task (pri 10) waits for
 * the vision task (pri 20) holding the bus, the vision task is boosted until it releases
 * it, so no medium-priority work can stretch the wait (no unbounded priority inversion).
 */
#include <tk/tkernel.h>
#include <stdint.h>
#include "stm32n6xx_hal.h"

LOCAL ID i2c_mtx;

void gv_i2c_lock_init(void)
{
	T_CMTX c = { .mtxatr = TA_INHERIT };
	i2c_mtx = tk_cre_mtx(&c);
}

/* no-op before the kernel/mutex exists and in interrupt context */
void gv_i2c_lock(void)   { if (i2c_mtx > 0 && !__get_IPSR()) tk_loc_mtx(i2c_mtx, TMO_FEVR); }
void gv_i2c_unlock(void) { if (i2c_mtx > 0 && !__get_IPSR()) tk_unl_mtx(i2c_mtx); }

#define WRAP(ret, name, params, args) \
	ret __real_##name params; \
	ret __wrap_##name params { gv_i2c_lock(); ret _ret = __real_##name args; gv_i2c_unlock(); return _ret; }

WRAP(int32_t, BSP_I2C1_Init,       (void), ())
WRAP(int32_t, BSP_I2C1_DeInit,     (void), ())
WRAP(int32_t, BSP_I2C1_ReadReg16,  (uint16_t a, uint16_t r, uint8_t *p, uint16_t n), (a, r, p, n))
WRAP(int32_t, BSP_I2C1_WriteReg16, (uint16_t a, uint16_t r, uint8_t *p, uint16_t n), (a, r, p, n))
