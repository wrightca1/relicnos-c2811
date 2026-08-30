// SPDX-License-Identifier: GPL-2.0
/*
 * Cisco 2811 UART registration.
 *
 * Two 16550s on MV64340 DevCS2: the console ("line con 0" / CTY) and the AUX
 * port ("line aux 0"), 0x40 apart.  Both were found by disassembling ROMMON's
 * own putchar at 0xBFC108C0 and confirmed on the hardware by a bare-metal
 * payload (tools/relicnos/uart.S) -- see DYNAMIC_ANALYSIS.md sec 9 and 10.
 *
 * .irq = 0 puts serial8250 in polled mode.  That is deliberate for first boot:
 * the UART's interrupt is routed through the MV64340 interrupt controller,
 * which we have not wired up yet.  Polled is slower but needs no new code, and
 * an interactive shell does not care.
 */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>

#include "cisco2811.h"

static struct plat_serial8250_port c2811_serial8250_port[] = {
	{	/* CTY -- the console */
		.mapbase	= C2811_UART0_BASE,
		.irq		= 0,			/* polled; see above */
		.uartclk	= C2811_UART_CLK,
		.iotype		= UPIO_MEM,
		.regshift	= C2811_UART_REGSHIFT,
		.flags		= UPF_IOREMAP | UPF_BOOT_AUTOCONF | UPF_SKIP_TEST,
	},
	{	/* AUX */
		.mapbase	= C2811_UART1_BASE,
		.irq		= 0,
		.uartclk	= C2811_UART_CLK,
		.iotype		= UPIO_MEM,
		.regshift	= C2811_UART_REGSHIFT,
		.flags		= UPF_IOREMAP | UPF_BOOT_AUTOCONF | UPF_SKIP_TEST,
	},
	{},
};

static struct platform_device c2811_serial8250_device = {
	.name	= "serial8250",
	.id	= PLAT8250_DEV_PLATFORM,
	.dev	= {
		.platform_data = c2811_serial8250_port,
	},
};

static int __init c2811_serial_init(void)
{
	return platform_device_register(&c2811_serial8250_device);
}
device_initcall(c2811_serial_init);
