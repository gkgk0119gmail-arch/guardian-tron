/*
 * gt_uart.c - Guardian-TRON UART driver (register level)
 * RX: interrupt -> ring buffer -> event flag.  TX: polling (short frames only).
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "stm32n6xx_hal.h"
#include "gt_uart.h"

#define RXBUF_SZ	512			/* power of 2 */
#define RXBUF_MASK	(RXBUF_SZ - 1)
#define FLG_RX		(1U << 0)

typedef struct {
	USART_TypeDef	*dev;
	IRQn_Type	irqn;
	volatile UW	head;			/* ISR writes */
	volatile UW	tail;			/* task reads */
	UB		rxbuf[RXBUF_SZ];
	ID		flgid;
	volatile UW	overrun;
	UW		clk_hz;
	BOOL		swap;			/* USART_CR2_SWAP: exchange TX/RX pins */
} gt_uart_t;

LOCAL gt_uart_t uarts[2] = {
	{ .dev = USART2, .irqn = USART2_IRQn },
	{ .dev = USART3, .irqn = USART3_IRQn },
};

LOCAL void rx_isr(gt_uart_t *u)
{
	UW isr = u->dev->ISR;
	if (isr & USART_ISR_ORE) { u->dev->ICR = USART_ICR_ORECF; u->overrun++; }
	if (isr & (USART_ISR_FE | USART_ISR_NE)) u->dev->ICR = USART_ICR_FECF | USART_ICR_NECF;
	while (u->dev->ISR & USART_ISR_RXNE_RXFNE) {
		UB c = (UB)u->dev->RDR;
		UW next = (u->head + 1) & RXBUF_MASK;
		if (next != u->tail) { u->rxbuf[u->head] = c; u->head = next; }
		else u->overrun++;			/* ring full: drop */
	}
	if (u->flgid > 0) tk_set_flg(u->flgid, FLG_RX);
}

void USART2_IRQHandler(void) { rx_isr(&uarts[GT_UART_JETSON]); }
void USART3_IRQHandler(void) { rx_isr(&uarts[GT_UART_VESC]); }

LOCAL UW clk_init(INT unit)
{
	RCC_PeriphCLKInitTypeDef p = {0};
	if (unit == GT_UART_JETSON) {
		p.PeriphClockSelection = RCC_PERIPHCLK_USART2;
		p.Usart2ClockSelection = RCC_USART2CLKSOURCE_CLKP;	/* CLKP = HSI (set by PeriphCommonClock_Config) */
	} else {
		p.PeriphClockSelection = RCC_PERIPHCLK_USART3;
		p.Usart3ClockSelection = RCC_USART3CLKSOURCE_CLKP;
	}
	if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) return 0;
	if (unit == GT_UART_JETSON) __HAL_RCC_USART2_CLK_ENABLE(); else __HAL_RCC_USART3_CLK_ENABLE();
	return HAL_RCCEx_GetPeriphCLKFreq(unit == GT_UART_JETSON ? RCC_PERIPHCLK_USART2 : RCC_PERIPHCLK_USART3);
}

LOCAL void gpio_init(INT unit)
{
	GPIO_InitTypeDef g = {0};
	g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_PULLUP; g.Speed = GPIO_SPEED_FREQ_LOW;
	if (unit == GT_UART_JETSON) {
		__HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOF_CLK_ENABLE();
		g.Alternate = GPIO_AF7_USART2;
		g.Pin = GPIO_PIN_5;  HAL_GPIO_Init(GPIOD, &g);	/* PD5  USART2_TX  (Arduino D1) */
		g.Pin = GPIO_PIN_6;  HAL_GPIO_Init(GPIOF, &g);	/* PF6  USART2_RX  (Arduino D0) */
	} else {
		__HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_GPIOE_CLK_ENABLE();
		g.Alternate = GPIO_AF7_USART3;
		g.Pin = GPIO_PIN_10; HAL_GPIO_Init(GPIOB, &g);	/* PB10 USART3_TX  (Arduino A5) */
		g.Pin = GPIO_PIN_10; HAL_GPIO_Init(GPIOE, &g);	/* PE10 USART3_RX  (Arduino D5) */
	}
}

EXPORT ER gt_uart_init(INT unit, UW baud)
{
	if (unit < 0 || unit > 1) return E_PAR;
	gt_uart_t *u = &uarts[unit];
	T_CFLG cflg = { .flgatr = TA_TFIFO | TA_WMUL, .iflgptn = 0 };
	if (u->flgid <= 0) { u->flgid = tk_cre_flg(&cflg); if (u->flgid < 0) return u->flgid; }

	u->clk_hz = clk_init(unit);
	if (u->clk_hz == 0) return E_SYS;
	gpio_init(unit);

	USART_TypeDef *d = u->dev;
	d->CR1 = 0;						/* UE=0, 8N1, oversampling 16, FIFO off */
	d->CR2 = u->swap ? USART_CR2_SWAP : 0;
	d->CR3 = 0;
	d->PRESC = 0;						/* /1 */
	d->BRR = (u->clk_hz + baud/2) / baud;
	d->ICR = 0xFFFFFFFFU;
	d->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;
	d->CR1 |= USART_CR1_UE;

	u->head = u->tail = 0;
	HAL_NVIC_SetPriority(u->irqn, 6, 0);			/* same level BSP uses for I2C1 */
	HAL_NVIC_EnableIRQ(u->irqn);
	return E_OK;
}

EXPORT INT gt_uart_read(INT unit, UB *buf, INT len, TMO tmo)
{
	gt_uart_t *u = &uarts[unit];
	INT n = 0;
	if (u->head == u->tail) {
		UINT ptn; ER er = tk_wai_flg(u->flgid, FLG_RX, TWF_ORW | TWF_BITCLR, &ptn, tmo);
		if (er == E_TMOUT) return 0;
		if (er < E_OK) return er;
	}
	while (n < len && u->head != u->tail) { buf[n++] = u->rxbuf[u->tail]; u->tail = (u->tail + 1) & RXBUF_MASK; }
	return n;
}

EXPORT INT gt_uart_write(INT unit, const UB *buf, INT len)
{
	USART_TypeDef *d = uarts[unit].dev;
	for (INT i = 0; i < len; i++) {
		while (!(d->ISR & USART_ISR_TXE_TXFNF)) ;
		d->TDR = buf[i];
	}
	while (!(d->ISR & USART_ISR_TC)) ;
	return len;
}

EXPORT void gt_uart_puts(INT unit, const char *s)
{
	INT n = 0; while (s[n]) n++;
	gt_uart_write(unit, (const UB*)s, n);
}

/* Takes effect on the next gt_uart_init() (every re-init keeps it). */
EXPORT void gt_uart_set_swap(INT unit, BOOL swap) { uarts[unit].swap = swap; }

EXPORT UW gt_uart_overruns(INT unit) { return uarts[unit].overrun; }
EXPORT UW gt_uart_clock_hz(INT unit) { return uarts[unit].clk_hz; }
