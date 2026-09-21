/* gt_vesc.c - VESC packet protocol (CRC16-CCITT, 0x02 short frame) */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "gt_uart.h"
#include "gt_vesc.h"

LOCAL const UH crc16_tab[16] = {
	0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
	0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef
};

EXPORT UH gt_vesc_crc16(const UB *buf, INT len)
{
	UH crc = 0;
	for (INT i = 0; i < len; i++) {
		crc = (UH)((crc << 4) ^ crc16_tab[((crc >> 12) ^ (buf[i] >> 4)) & 0x0F]);
		crc = (UH)((crc << 4) ^ crc16_tab[((crc >> 12) ^ (buf[i] & 0x0F)) & 0x0F]);
	}
	return crc;
}

EXPORT INT gt_vesc_frame(UB *out, const UB *payload, INT plen)
{
	INT n = 0;
	out[n++] = 0x02;			/* short frame: 1-byte length */
	out[n++] = (UB)plen;
	for (INT i = 0; i < plen; i++) out[n++] = payload[i];
	UH c = gt_vesc_crc16(payload, plen);
	out[n++] = (UB)(c >> 8);
	out[n++] = (UB)(c & 0xFF);
	out[n++] = 0x03;
	return n;
}

EXPORT ER gt_vesc_init(UW baud) { return gt_uart_init(GT_UART_VESC, baud); }

/* send 1-byte command, wait for a well-formed reply; returns payload length */
EXPORT INT gt_vesc_request(UB cmd, UB *rxpayload, INT maxlen, TMO tmo)
{
	UB tx[8], rxb[256], *rx = rxb;
	INT n = gt_vesc_frame(tx, &cmd, 1);
	while (gt_uart_read(GT_UART_VESC, rxb, sizeof rxb, 0) > 0) ;	/* drop stale/glitch bytes */
	gt_uart_write(GT_UART_VESC, tx, n);

	/* Bytes trickle in one per ~260 us at 38400; keep reading until the frame is complete,
	 * giving up after `tmo` of total silence-or-not (idle gaps of 30 ms end the wait). */
	INT got = 0, idle = 0, waited = 0;
	while (waited < tmo && rx + got < rxb + sizeof rxb) {
		INT r = gt_uart_read(GT_UART_VESC, rx + got, (INT)(rxb + sizeof rxb - (rx + got)), 30);
		if (r > 0) { got += r; idle = 0; }			/* data: does not consume the budget */
		else { waited += 30; if (got > 0 && ++idle >= 2) break; }
		/* resync on the 0x02 start byte (the idle line can yield a leading 0x00) */
		while (got > 0 && rx[0] != 0x02) { rx++; got--; }
		if (got >= 3) {						/* enough to know the length */
			INT need = rx[1] + 5;
			if (got >= need) break;
		}
	}
	if (got == 0) return 0;

	if (rx[0] != 0x02) return -1;				/* not a VESC short frame */
	INT plen = rx[1];
	if (got < plen + 5) {					/* truncated */
		tm_printf((UB*)"[vesc] truncated: got=%d plen=%d\n", got, plen);
		return -2;
	}
	UH crc = gt_vesc_crc16(&rx[2], plen);
	UH rcv = (UH)((rx[2 + plen] << 8) | rx[3 + plen]);
	if (crc != rcv) return -3;				/* CRC mismatch */
	if (rx[4 + plen] != 0x03) return -4;
	INT cp = plen < maxlen ? plen : maxlen;
	for (INT i = 0; i < cp; i++) rxpayload[i] = rx[2 + i];
	return plen;
}

/* ---- drive commands (encodings match jetson/ecu/vesc.py and vedderb/bldc commands.c) ---- */
LOCAL void send_cmd_i32(UB cmd, W v)
{
	UB pl[5] = { cmd, (UB)(v >> 24), (UB)(v >> 16), (UB)(v >> 8), (UB)v }, tx[16];
	INT n = gt_vesc_frame(tx, pl, 5);
	gt_uart_write(GT_UART_VESC, tx, n);
}

EXPORT void gt_vesc_set_duty(float duty)		/* [-1, 1] */
{
	if (duty > 1.0f) duty = 1.0f; if (duty < -1.0f) duty = -1.0f;
	send_cmd_i32(COMM_SET_DUTY, (W)(duty * 100000.0f));
}
EXPORT void gt_vesc_set_current(float amps)	{ send_cmd_i32(COMM_SET_CURRENT, (W)(amps * 1000.0f)); }
EXPORT void gt_vesc_set_brake(float amps)	{ send_cmd_i32(COMM_SET_CURRENT_BRAKE, (W)(amps * 1000.0f)); }
EXPORT void gt_vesc_set_rpm(W erpm)		{ send_cmd_i32(COMM_SET_RPM, erpm); }

EXPORT void gt_vesc_set_servo(float pos)		/* [0, 1]; int16 BE x1000 */
{
	if (pos > 1.0f) pos = 1.0f; if (pos < 0.0f) pos = 0.0f;
	H v = (H)(pos * 1000.0f);
	UB pl[3] = { COMM_SET_SERVO_POS, (UB)(v >> 8), (UB)v }, tx[16];
	INT n = gt_vesc_frame(tx, pl, 3);
	gt_uart_write(GT_UART_VESC, tx, n);
}

EXPORT void gt_vesc_alive(void)
{
	UB c = COMM_ALIVE, tx[8];
	INT n = gt_vesc_frame(tx, &c, 1);
	gt_uart_write(GT_UART_VESC, tx, n);
}

LOCAL W be32(const UB *p) { return (W)(((UW)p[0] << 24) | ((UW)p[1] << 16) | ((UW)p[2] << 8) | p[3]); }
LOCAL H be16(const UB *p) { return (H)(((UH)p[0] << 8) | p[1]); }

/* COMM_GET_VALUES layout for fw 6.x (verified on this VESC via vesc.py):
 * temp_fet h/10, temp_motor h/10, cur_motor i/100, cur_in i/100, id i/100, iq i/100,
 * duty h/1000, rpm i, v_in h/10, ah i, ahc i, wh i, whc i, tach i, tach_abs i, fault b */
EXPORT INT gt_vesc_get_values(gt_vesc_values_t *v, TMO tmo)
{
	UB pl[80];
	INT n = gt_vesc_request(COMM_GET_VALUES, pl, sizeof pl, tmo);
	if (n < 60) return n > 0 ? -5 : n;
	const UB *b = pl + 1;
	v->temp_fet_d	= be16(b + 0);
	v->cur_motor_c	= be32(b + 4);
	v->cur_in_c	= be32(b + 8);
	v->duty_m	= be16(b + 20);
	v->rpm		= be32(b + 22);
	v->v_in_d	= be16(b + 26);
	v->tach		= be32(b + 44);
	v->fault	= b[52];
	return 0;
}
