/*
 *	STM32N6570-DK Camera Example for μT-Kernel 3.0 BSP2
 *
 *    Copyright (C) 2025 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2025/07.
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include <main.h>
#include "isp_api.h"
#include "imx335_E27_isp_param_conf.h"

extern DCMIPP_HandleTypeDef	hdcmipp;
extern ISP_HandleTypeDef	hcamera_isp;

/* 
 * User Button Interrupt handler and task
 */
#define BTNINT_IRQ	33
#define BTNINT_EXTI	(MIN_EXTI_INTNO + 13)

LOCAL void inthdr_btn(UINT intno);		// Interrupt handler function
LOCAL T_DINT	dint_btn = {			// Interrupt handler definition information
	.intatr = TA_HLNG,
	.inthdr = inthdr_btn,
};

LOCAL void task_btn(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_btn;			// Task ID number
LOCAL T_CTSK ctsk_sw = {			// Task creation information
	.itskpri	= 9,
	.stksz		= 1024,
	.task		= task_btn,
	.tskatr		= TA_HLNG | TA_RNG3,
};

LOCAL void inthdr_btn(UINT intno)
{
	ClearInt(BTNINT_EXTI);
	tk_wup_tsk(tskid_btn);

	ClearInt(intno);
}
LOCAL void task_btn(INT stacd, void *exinf)
{
	BOOL	is_video_running = TRUE;

	/* Interrupt handler definition */
	tk_def_int(BTNINT_IRQ, &dint_btn);
	SetIntMode(BTNINT_EXTI, IM_EDGE | IM_LOW);
	EnableInt(BTNINT_EXTI, 0);
	EnableInt(BTNINT_IRQ, 10);

	while(1) {
		tk_slp_tsk(TMO_FEVR);

		if(is_video_running) {
			HAL_DCMIPP_CSI_PIPE_Stop(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0);
			is_video_running = FALSE;
		} else {
			HAL_DCMIPP_CSI_PIPE_Start(&hdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0 , BUFFER_ADDRESS, DCMIPP_MODE_CONTINUOUS);
			is_video_running = TRUE;
		}
	}
}

/*
 * Camera control task
 */
LOCAL void task_cam(INT stacd, void *exinf);	// task execution function
LOCAL ID	tskid_cam;			// Task ID number
LOCAL T_CTSK ctsk_cam = {			// Task creation information
	.itskpri	= 10,
	.stksz		= 1024,
	.task		= task_cam,
	.tskatr		= TA_HLNG | TA_RNG3,
};
LOCAL void task_cam(INT stacd, void *exinf)
{
	while(1) {
		if( ISP_BackgroundProcess(&hcamera_isp) != ISP_OK) break;
		tk_dly_tsk(100);
	}
	tk_ext_tsk();
}

/*
 * User Main function
 */
EXPORT INT usermain(void)
{
	tm_putstring((UB*)"Start User-main program.\n");

	/* Create & Start Tasks */
	tskid_btn = tk_cre_tsk(&ctsk_sw);
	tk_sta_tsk(tskid_btn, 0);

	tskid_cam = tk_cre_tsk(&ctsk_cam);
	tk_sta_tsk(tskid_cam, 0);

	tk_slp_tsk(TMO_FEVR);
	return 0;
}