/*
 * This file is part of the Black Magic Debug project.
 *
 * Based on work that is Copyright (C) 2017 Black Sphere Technologies Ltd.
 * Copyright (C) 2017 Dave Marples <dave@marples.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.	 If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file implements capture of the Trace/SWO output using async signalling.
 *
 * ARM DDI 0403D - ARMv7M Architecture Reference Manual
 * ARM DDI 0337I - Cortex-M3 Technical Reference Manual
 * ARM DDI 0314H - CoreSight Components Technical Reference Manual
 */

/* TDO/TRACESWO signal comes into the SWOUSART RX pin. */

#include "general.h"
#include "platform.h"
#include "usb.h"
#include "swo.h"

#include <stdatomic.h>
#include <malloc.h>
#include <errno.h>

#include <libopencmsis/core_cm3.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/dma.h>

#if defined(DMA_STREAM0)
#define dma_channel_reset(dma, channel)   dma_stream_reset(dma, channel)
#define dma_enable_channel(dma, channel)  dma_enable_stream(dma, channel)
#define dma_disable_channel(dma, channel) dma_disable_stream(dma, channel)

#define DMA_PSIZE_8BIT DMA_SxCR_PSIZE_8BIT
#define DMA_MSIZE_8BIT DMA_SxCR_MSIZE_8BIT
#define DMA_PL_HIGH    DMA_SxCR_PL_HIGH
#else
#define DMA_PSIZE_8BIT DMA_CCR_PSIZE_8BIT
#define DMA_MSIZE_8BIT DMA_CCR_MSIZE_8BIT
#define DMA_PL_HIGH    DMA_CCR_PL_HIGH
#endif

static volatile uint32_t write_index; /* Packet currently received via UART */
static volatile uint32_t read_index;  /* Packet currently waiting to transmit to USB */
/* Packets arrived from the SWO interface */
static uint8_t *trace_rx_buf = NULL;
/* Packet pingpong buffer used for receiving packets */
static uint8_t pingpong_buf[2 * TRACE_ENDPOINT_SIZE];
/* SWO decoding */
static bool decoding = false;

void trace_buf_drain(usbd_device *dev, uint8_t ep)
{
	static atomic_flag reentry_flag = ATOMIC_FLAG_INIT;

	/* If we are already in this routine then we don't need to come in again */
	if (atomic_flag_test_and_set_explicit(&reentry_flag, memory_order_relaxed))
		return;
	/* Attempt to write everything we buffered */
	if (write_index != read_index) {
		uint16_t result;
		if (decoding)
			/* write decoded swo packets to the uart port */
			result = traceswo_decode(
				dev, CDCACM_UART_ENDPOINT, &trace_rx_buf[read_index * TRACE_ENDPOINT_SIZE], TRACE_ENDPOINT_SIZE);
		else
			/* write raw swo packets to the trace port */
			result =
				usbd_ep_write_packet(dev, ep, &trace_rx_buf[read_index * TRACE_ENDPOINT_SIZE], TRACE_ENDPOINT_SIZE);
		if (result)
			read_index = (read_index + 1U) % NUM_TRACE_PACKETS;
	}
	atomic_flag_clear_explicit(&reentry_flag, memory_order_relaxed);
}

uint32_t traceswo_get_baudrate(void)
{
	return usart_get_baudrate(SWO_UART);
}

void traceswo_setspeed(uint32_t baudrate)
{
	dma_disable_channel(SWO_DMA_BUS, SWO_DMA_CHAN);
	usart_disable(SWO_UART);
	bmd_usart_set_baudrate(SWO_UART, baudrate);
	usart_set_databits(SWO_UART, 8);
	usart_set_stopbits(SWO_UART, USART_STOPBITS_1);
	usart_set_mode(SWO_UART, USART_MODE_RX);
	usart_set_parity(SWO_UART, USART_PARITY_NONE);
	usart_set_flow_control(SWO_UART, USART_FLOWCONTROL_NONE);

	/* Set up DMA channel */
	dma_channel_reset(SWO_DMA_BUS, SWO_DMA_CHAN);
	dma_set_peripheral_address(SWO_DMA_BUS, SWO_DMA_CHAN, (uint32_t)&SWO_UART_DR);
#if defined(DMA_STREAM0)
	dma_set_transfer_mode(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_SxCR_DIR_PERIPHERAL_TO_MEM);
	dma_channel_select(SWO_DMA_BUS, SWO_DMA_CHAN, SWO_DMA_TRG);
	dma_set_dma_flow_control(SWO_DMA_BUS, SWO_DMA_CHAN);
	dma_enable_direct_mode(SWO_DMA_BUS, SWO_DMA_CHAN);
#else
	dma_set_read_from_peripheral(SWO_DMA_BUS, SWO_DMA_CHAN);
#endif
	dma_enable_memory_increment_mode(SWO_DMA_BUS, SWO_DMA_CHAN);
	dma_set_peripheral_size(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_PSIZE_8BIT);
	dma_set_memory_size(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_MSIZE_8BIT);
	dma_set_priority(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_PL_HIGH);
	dma_enable_transfer_complete_interrupt(SWO_DMA_BUS, SWO_DMA_CHAN);
	dma_enable_half_transfer_interrupt(SWO_DMA_BUS, SWO_DMA_CHAN);
	dma_enable_circular_mode(SWO_DMA_BUS, SWO_DMA_CHAN);

	usart_enable(SWO_UART);
	nvic_enable_irq(SWO_DMA_IRQ);
	write_index = read_index = 0;
	dma_set_memory_address(SWO_DMA_BUS, SWO_DMA_CHAN, (uint32_t)pingpong_buf);
	dma_set_number_of_data(SWO_DMA_BUS, SWO_DMA_CHAN, 2 * TRACE_ENDPOINT_SIZE);
	dma_enable_channel(SWO_DMA_BUS, SWO_DMA_CHAN);
	usart_enable_rx_dma(SWO_UART);
}

void SWO_DMA_ISR(void)
{
	if (dma_get_interrupt_flag(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_HTIF)) {
		dma_clear_interrupt_flags(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_HTIF);
		memcpy(&trace_rx_buf[write_index * TRACE_ENDPOINT_SIZE], pingpong_buf, TRACE_ENDPOINT_SIZE);
	}
	if (dma_get_interrupt_flag(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_TCIF)) {
		dma_clear_interrupt_flags(SWO_DMA_BUS, SWO_DMA_CHAN, DMA_TCIF);
		memcpy(
			&trace_rx_buf[write_index * TRACE_ENDPOINT_SIZE], &pingpong_buf[TRACE_ENDPOINT_SIZE], TRACE_ENDPOINT_SIZE);
	}
	write_index = (write_index + 1U) % NUM_TRACE_PACKETS;
	trace_buf_drain(usbdev, TRACE_ENDPOINT | USB_REQ_TYPE_IN);
}

void traceswo_init(uint32_t baudrate, uint32_t swo_chan_bitmask)
{
	/* Skip initial allocation on commands for mode change */
	if (trace_rx_buf == NULL) {
		/* Alignment (bytes): 1 for UART DMA, 2-4 for memcpy in usb code, 8 provided by malloc. Not 64 */
		uint8_t *const newbuf = malloc(NUM_TRACE_PACKETS * TRACE_ENDPOINT_SIZE);
		if (!newbuf) {
			DEBUG_ERROR("malloc: failed in %s\n", __func__);
			return;
		}
		trace_rx_buf = newbuf;
	}

	if (!baudrate)
		baudrate = SWO_DEFAULT_BAUD;

	rcc_periph_clock_enable(SWO_UART_CLK);
	rcc_periph_clock_enable(SWO_DMA_CLK);

#if defined(STM32F1)
	gpio_set_mode(SWO_UART_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, SWO_UART_RX_PIN);
	/* Pull SWO pin high to keep open SWO line ind uart idle state! */
	gpio_set(SWO_UART_PORT, SWO_UART_RX_PIN);
#else
	gpio_mode_setup(SWO_UART_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, SWO_UART_RX_PIN);
	gpio_set_output_options(SWO_UART_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ, SWO_UART_RX_PIN);
	gpio_set_af(SWO_UART_PORT, SWO_UART_PIN_AF, SWO_UART_RX_PIN);
#endif

	nvic_set_priority(SWO_DMA_IRQ, IRQ_PRI_SWO_DMA);
	nvic_enable_irq(SWO_DMA_IRQ);
	traceswo_setspeed(baudrate);
	traceswo_setmask(swo_chan_bitmask);
	decoding = (swo_chan_bitmask != 0);
}

void traceswo_deinit(void)
{
	/* Stop peripherals servicing */
	nvic_disable_irq(SWO_DMA_IRQ);
	dma_disable_channel(SWO_DMA_BUS, SWO_DMA_CHAN);
	usart_disable(SWO_UART);
	/* Dump the buffered remains */
	trace_buf_drain(usbdev, TRACE_ENDPOINT | USB_REQ_TYPE_IN);
	/* Return this contiguous chunk of SRAM to unshrinkable heap */
	if (trace_rx_buf != NULL) {
		free(trace_rx_buf);
		trace_rx_buf = NULL;
	}
}