/*
 * gt_uart.h - Guardian-TRON UART driver (register level, μT-Kernel 3.0 / STM32N6570-DK)
 *   unit 0 : USART2  Jetson link   Arduino D0 = RX (PF6), D1 = TX (PD5)
 *   unit 1 : USART3  VESC link     Arduino D5 = RX (PE10), A5 = TX (PB10)
 */
#ifndef GT_UART_H
#define GT_UART_H
#include <tk/tkernel.h>

#define GT_UART_JETSON	0
#define GT_UART_VESC	1

ER   gt_uart_init(INT unit, UW baud);
INT  gt_uart_read(INT unit, UB *buf, INT len, TMO tmo);	/* >=0 bytes read, waits up to tmo for first byte */
INT  gt_uart_write(INT unit, const UB *buf, INT len);	/* blocking, returns len */
void gt_uart_puts(INT unit, const char *s);
void gt_uart_set_swap(INT unit, BOOL swap);		/* exchange TX/RX pins from the next init */
UW   gt_uart_overruns(INT unit);
void gt_uart_kick(INT unit);                         /* wake a blocked gt_uart_read() */
UW   gt_uart_clock_hz(INT unit);

UW   gt_uart_take_wake_latency(INT unit);

#endif
