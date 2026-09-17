/*
 * usermain.c - Guardian-TRON on μT-Kernel 3.0 / STM32N6570-DK
 *
 * Tasks:
 *   gate   (pri 8)  Jetson CMD -> safety envelope -> VESC servo/erpm, watchdog, local-safety hook
 *   cam    (pri 12) camera module probe (IMX335 / IMU / ToF on I2C1)  -- pipeline TODO
 *   led    (pri 20) alive blink
 *
 * Wiring (verified 2026-09-13):
 *   USART2 Jetson  : D0 PF6 RX, D1 PD5 TX, 115200
 *   USART3 VESC    : SWAPPED -> TX on D5/PE10 (red -> VESC RX), RX on A5/PB10 (brown <- VESC TX), 38400
 *                    (A5 sits behind the DK analog input network: input-only, <= 38400 baud)
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "stm32n6xx_hal.h"
#include "gt_uart.h"
#include "gt_vesc.h"
#include "gt_gate.h"

/* ---- LED task (board alive indicator) ---- */
LOCAL void led_task(INT stacd, void *exinf)
{
	while (1) {
		out_w(GPIO_ODR(O), (in_w(GPIO_ODR(O))) ^ (1 << 1));
		tk_dly_tsk(500);
	}
}
LOCAL T_CTSK ctsk_led  = { .itskpri = 20, .stksz = 1024, .task = led_task,     .tskatr = TA_HLNG | TA_RNG3 };
LOCAL T_CTSK ctsk_gate = { .itskpri = 8,  .stksz = 4096, .task = gt_gate_task, .tskatr = TA_HLNG | TA_RNG3 };

/* ---- Onboard camera module (MB1854: IMX335 @0x1A, IMU @0x6A, ToF VL53L5CX-class @0x29) ---- */
#define CAM_I2C_SADR	0x1A

LOCAL ER cam_read16(ID dd, UH reg, UB *val)
{
	UB r[2] = { (UB)(reg >> 8), (UB)reg };
	SZ asz;
	ER er = tk_swri_dev(dd, CAM_I2C_SADR, r, 2, &asz);
	if (er < E_OK) return er;
	return tk_srea_dev(dd, CAM_I2C_SADR, val, 1, &asz);
}

LOCAL void cam_task(INT stacd, void *exinf)
{
	/* power-up order from BSP_CAMERA_HwReset: PC8 = 2V8 enable, PD2 = NRST */
	GPIO_InitTypeDef g = {0};
	__HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE();
	g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
	g.Pin = GPIO_PIN_8; HAL_GPIO_Init(GPIOC, &g);
	g.Pin = GPIO_PIN_2; HAL_GPIO_Init(GPIOD, &g);
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET); tk_dly_tsk(100);
	HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2, GPIO_PIN_RESET); tk_dly_tsk(100);
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);   tk_dly_tsk(100);
	HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2, GPIO_PIN_SET);   tk_dly_tsk(100);

	/* hiica completes via HAL IT callbacks; CubeMX did not enable the I2C1 IRQs */
	HAL_NVIC_SetPriority(I2C1_EV_IRQn, 6, 0); HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
	HAL_NVIC_SetPriority(I2C1_ER_IRQn, 6, 0); HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

	ID dd = tk_opn_dev((UB*)"hiica", TD_UPDATE);
	if (dd < E_OK) { tm_printf((UB*)"[cam] hiica open err %d\n", dd); tk_ext_tsk(); }

	UB id = 0xFF, who = 0xFF, r = 0x0F; SZ asz;
	ER e1 = cam_read16(dd, 0x3912, &id);
	ER e2 = tk_swri_dev(dd, 0x6A, &r, 1, &asz);
	if (e2 >= E_OK) e2 = tk_srea_dev(dd, 0x6A, &who, 1, &asz);
	tm_printf((UB*)"[cam] IMX335 id er=%d val=%02x | IMU who er=%d val=%02x -> %s\n",
		  e1, id, e2, who, (e1 >= E_OK && id == 0x00) ? "CAMERA MODULE OK" : "NO CAMERA");
	tk_ext_tsk();
}
LOCAL T_CTSK ctsk_cam = { .itskpri = 12, .stksz = 2048, .task = cam_task, .tskatr = TA_HLNG | TA_RNG3 };

EXPORT INT usermain(void)
{
	tm_putstring((UB*)"Guardian-TRON start (gatekeeper)\n");
	ID id;
	id = tk_cre_tsk(&ctsk_led);  tk_sta_tsk(id, 0);

	/* VESC link first: the gate task assumes it is up */
	gt_uart_set_swap(GT_UART_VESC, TRUE);
	gt_vesc_init(GT_VESC_BAUD);
	UB pl[64];
	INT n = gt_vesc_request(COMM_GET_FW_VERSION, pl, sizeof pl, 300);
	if (n > 0) tm_printf((UB*)"[vesc] fw %d.%d ready\n", pl[1], pl[2]);
	else tm_printf((UB*)"[vesc] NO REPLY (%d) - gate will run but VESC is silent\n", n);

	id = tk_cre_tsk(&ctsk_gate); tk_sta_tsk(id, 0);
	id = tk_cre_tsk(&ctsk_cam);  tk_sta_tsk(id, 0);
	tk_slp_tsk(TMO_FEVR);
	return 0;
}
