/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cisco 2811 board constants -- all measured, see DYNAMIC_ANALYSIS.md.
 */
#ifndef _CISCO2811_H
#define _CISCO2811_H

/* DRAM: 256 MB at physical 0 (DYNAMIC_ANALYSIS.md sec 2, SCS[0]). */
#define C2811_DRAM_SIZE		(256UL << 20)

/*
 * Console + AUX 16550s, on MV64340 DevCS2 (phys 0x1FA80000, 128 KB window).
 * Register file is at 8-byte spacing -- LSR is at +0x28 = 5 << 3 -- confirmed
 * on silicon by a scratch-register round trip (DYNAMIC_ANALYSIS.md sec 10).
 */
#define C2811_UART0_BASE	0x1fa80000	/* CTY, "line con 0" */
#define C2811_UART1_BASE	0x1fa80040	/* AUX, "line aux 0" */
#define C2811_UART_REGSHIFT	3
#define C2811_UART_CLK		3686400		/* 9600 baud = divisor 24 */

/*
 * CP0 Count frequency, measured by timing 960 console characters at 9600 8N1
 * (exactly 1.000 s): 175,088,846 Hz.  Cross-checks against Config EC=5 ("mul
 * by 7") and the reported "PCI0 running at 50 MHz" -> system 50 MHz, CPU
 * 350 MHz, Count = CPU/2 = 175 MHz.  (DYNAMIC_ANALYSIS.md sec 12.)
 */
#define C2811_HPT_FREQUENCY	175000000

/* Marvell MV64340 internal register window (DYNAMIC_ANALYSIS.md sec 2). */
/*
 * CompactFlash ("flash:" to IOS): an ATA device on device chip-select DevCS3.
 * Recovered from ROMMON, which passes 0xBFB00000 as the device handle to its
 * own ATA routines, and confirmed on the hardware with an IDENTIFY DEVICE.
 */
#define C2811_CF_BASE		0x1FB00000

#define C2811_MV64340_BASE	0x14000000

/*
 * MV64340 interrupt controller.  Its 64 sources sit above the 8 CP0 irqs.
 * MIPS_CPU_IRQ_BASE is 0, so the CPU occupies 0..7.
 */
#define C2811_MV_IRQ_BASE	8
#define C2811_MV_NR_IRQS	64

/*
 * Ethernet: the MV64340's integrated MAC, confirmed live as "Hardware is
 * MV96340" with SMI at +0x2000 and per-port registers at +0x2400 + n*0x400,
 * 8 rx/tx queues -- the mv643xx_eth layout exactly (DYNAMIC_ANALYSIS.md sec 3).
 *
 * PHY addresses come from the live PHYAR register (0x00002841), which packs one
 * 5-bit MDIO address per port: bits[4:0] = port 0 -> 1, bits[9:5] = port 1 -> 2.
 *
 * NOT from SMIR's low byte.  An earlier reading took SMIR = 0x0FE1000B and used
 * 0x0B as the PHY address; in that register bits[15:0] are DATA and bits[20:16]
 * are the address, so 0x0B was the value read from PHY 1 register 31.  The
 * kernel caught it: "libphy: PHY orion-mdio-mii:0b not found".
 */
#define C2811_ETH_PHY_ADDR	1		/* Fa0/0; Fa0/1 is 2 */
#define C2811_ETH_PORT0		0		/* FastEthernet0/0 */
#define C2811_ETH_PORT1		1		/* FastEthernet0/1 */

#ifndef __ASSEMBLY__
bool mv64340_irq_dispatch(void);
void mv64340_irq_init(void);
#endif


/*
 * DMA scratch for the NM-32A.  Must live inside the 8 MB PCI inbound window
 * (CS_0, PCI 0 -> physical 0), because that is the only chip-select BAR this
 * board enables; a card DMA outside it master-aborts and hangs the machine.
 */
#define C2811_DMA_BASE	0x00600000
#define C2811_DMA_SIZE	0x00200000	/* 2 MB */

#endif /* _CISCO2811_H */
