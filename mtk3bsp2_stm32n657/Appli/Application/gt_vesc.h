/* gt_vesc.h - VESC packet protocol over UART (USART3) */
#ifndef GT_VESC_H
#define GT_VESC_H
#include <tk/tkernel.h>

/* VESC command ids (datatypes.h COMM_PACKET_ID) */
#define COMM_GET_FW_VERSION	0
#define COMM_GET_VALUES		4
#define COMM_SET_DUTY		5
#define COMM_SET_CURRENT	6
#define COMM_SET_CURRENT_BRAKE	7
#define COMM_SET_RPM		8
#define COMM_SET_SERVO_POS	12
#define COMM_ALIVE		30

typedef struct {
	H  temp_fet_d;		/* 0.1 degC */
	W  cur_motor_c;		/* 0.01 A */
	W  cur_in_c;		/* 0.01 A */
	H  duty_m;		/* 0.001 */
	W  rpm;			/* electrical rpm */
	H  v_in_d;		/* 0.1 V */
	W  tach;
	UB fault;
} gt_vesc_values_t;

void gt_vesc_set_duty(float duty);		/* [-1,1]  PHYSICALLY SPINS THE MOTOR */
void gt_vesc_set_current(float amps);		/* PHYSICALLY SPINS THE MOTOR */
void gt_vesc_set_brake(float amps);		/* brake current */
void gt_vesc_set_rpm(W erpm);			/* PHYSICALLY SPINS THE MOTOR */
void gt_vesc_set_servo(float pos);		/* [0,1] steering servo on VESC servo header */
void gt_vesc_alive(void);			/* keepalive; VESC timeout is 1000 ms */
INT  gt_vesc_get_values(gt_vesc_values_t *v, TMO tmo);	/* 0 ok, <0 err */

#define GT_VESC_BAUD		38400	/* VESC app_uart baud; A5 (USART3 RX after SWAP) is unreliable above this */

UH   gt_vesc_crc16(const UB *buf, INT len);
INT  gt_vesc_frame(UB *out, const UB *payload, INT plen);	/* build 0x02 .. 0x03 frame, returns length */
ER   gt_vesc_init(UW baud);
INT  gt_vesc_request(UB cmd, UB *rxpayload, INT maxlen, TMO tmo);  /* >0 payload len, 0 none, <0 err */

#endif
