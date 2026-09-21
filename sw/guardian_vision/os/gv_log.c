/*
 * gv_log.c - non-blocking console for Guardian-TRON (NON-CRIT data logger task)
 *
 * The T-Monitor console (tm_putstring) sends the whole string by polling USART1 with
 * interrupts DISABLED: a 200-character status line at 115200 baud masks every interrupt,
 * including the kernel tick, for ~17 ms, and the task that prints (the gatekeeper!) is
 * blocked for as long. So tm_printf/tm_putstring are wrapped at link time: task-context
 * calls only format into a RAM ring (a few µs with interrupts masked for the copy), and
 * the lowest-priority logger task drains it through the USART1 TX FIFO, sleeping
 * whenever the FIFO is full. Exception handlers and pre-kernel code still print directly.
 */
#include <tk/tkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include "stm32n6xx_hal.h"

INT __real_tm_putstring(const UB *buff);

#define LOG_SZ		8192			/* power of 2 */
#define LOG_MASK	(LOG_SZ - 1)
#define CON		USART1			/* T-Monitor console (ST-LINK VCP) */

LOCAL UB  ring[LOG_SZ];
LOCAL volatile UW head, tail;
LOCAL volatile INT running;
volatile UW gv_log_dropped;

LOCAL void put(const UB *s, INT n)
{
	UINT imask;
	DI(imask);
	UW free = LOG_SZ - 1 - ((head - tail) & LOG_MASK);
	if ((UW)n > free) { gv_log_dropped += n; EI(imask); return; }
	for (INT i = 0; i < n; i++) ring[(head + i) & LOG_MASK] = s[i];
	head = (head + n) & LOG_MASK;
	EI(imask);
}

LOCAL INT direct(void) { return !running || __get_IPSR() != 0; }

INT __wrap_tm_putstring(const UB *buff)
{
	if (direct()) return __real_tm_putstring(buff);
	INT n = 0; while (buff[n]) n++;
	put(buff, n);
	return 0;
}

INT __wrap_tm_printf(const UB *format, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, format);
	INT n = vsnprintf(buf, sizeof buf, (const char *)format, ap);
	va_end(ap);
	if (n < 0) return n;
	if (n >= (INT)sizeof buf) n = sizeof buf - 1;
	if (direct()) { __real_tm_putstring((UB *)buf); return n; }
	put((UB *)buf, n);
	return n;
}

void gv_log_task(INT stacd, void *exinf)
{
	/* enable the 8-byte TX FIFO (FIFOEN may only change while UE = 0) */
	while (!(CON->ISR & USART_ISR_TC)) ;
	CON->CR1 &= ~USART_CR1_UE;
	CON->CR1 |= USART_CR1_FIFOEN;
	CON->CR1 |= USART_CR1_UE;
	running = 1;
	while (1) {
		if (head == tail) { tk_dly_tsk(10); continue; }
		while (head != tail) {
			if (!(CON->ISR & USART_ISR_TXE_TXFNF)) { tk_dly_tsk(1); continue; }
			CON->TDR = ring[tail];
			tail = (tail + 1) & LOG_MASK;
		}
	}
}
