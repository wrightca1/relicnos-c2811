// SPDX-License-Identifier: GPL-2.0
/*
 * Cisco 2811 (relicnos) board support.
 *
 * Every constant here was measured on the hardware, not guessed --
 * see devices/c2811/notes/DYNAMIC_ANALYSIS.md:
 *
 *   CPU        QED/PMC-Sierra RM52xx "Nevada", CP0 PRId 0x00002831, MIPS IV
 *              -> CONFIG_CPU_NEVADA                                    (sec 6)
 *   DRAM       256 MB at physical 0, already initialised by ROMMON     (sec 2)
 *   Console    16550 at phys 0x1FA80000, regshift 3, uartclk 3686400   (sec 9, 10)
 *   Timer      CP0 count/compare at 175 MHz (measured)                 (sec 12)
 *
 * The system controller is a Marvell MV64340 at phys 0x14000000, but nothing
 * here needs it: DRAM is already up, the console is on the device bus rather
 * than the controller's MPSC, and interrupts start out CP0-only.  That is why
 * this board file is as small as it is.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>

#include <asm/reboot.h>
#include <asm/addrspace.h>
#include <asm/bootinfo.h>
#include <asm/setup.h>

#include "cisco2811.h"

const char *get_system_type(void)
{
	return "Cisco 2811 (RM52xx Nevada, Marvell MV64340)";
}

/*
 * Reboot.
 *
 * Without these hooks `reboot` is a no-op -- the kernel has nothing to call and
 * simply returns, so the only way to restart the board is the PDU.  That makes
 * every iteration depend on out-of-band power control, which is worth avoiding.
 *
 * There is no documented soft-reset register for the MV96340 in the sources we
 * have (include/linux/mv643xx.h carries offsets but no bit definitions, and the
 * watchdog at 0xb410 needs an arming sequence we have not established).  Jumping
 * to the MIPS reset vector is the dependable alternative: on this board it is
 * BootCS, i.e. ROMMON, which re-initialises the hardware and then boots whatever
 * its BOOT variable names.
 *
 * This is a warm restart -- devices are left as the kernel had them and ROMMON
 * re-initialises what it needs.  It is not equivalent to a power cycle, which
 * still matters for anything that latches (the NM slot's CPLD state, for one).
 */
static void c2811_machine_restart(char *command)
{
	void (*rommon)(void) = (void (*)(void))0xBFC00000;

	pr_info("cisco2811: restarting via ROMMON reset vector\n");
	local_irq_disable();
	rommon();
	unreachable();
}

static void c2811_machine_halt(void)
{
	pr_info("cisco2811: halted; power-cycle or reset to restart\n");
	local_irq_disable();
	while (1)
		cpu_relax();
}

void __init plat_mem_setup(void)
{
	_machine_restart = c2811_machine_restart;
	_machine_halt = c2811_machine_halt;

	/*
	 * Reserve a DMA scratch region inside the FIRST 8 MB of DRAM.
	 *
	 * The NM-32A's CD2481s bus-master into host memory -- they never use
	 * their transmit FIFO (DYNAMIC_ANALYSIS.md sec 97) -- so a driver needs
	 * physically contiguous memory the card can actually reach.  The
	 * MV96340 exposes DRAM to PCI through chip-select BARs, and on this
	 * board only CS_0 is enabled (PCI_1_BASE_ADDR_REG_ENABLE = 0xFFFFFDFE,
	 * bit 0 clear), mapping PCI 0x00000000-0x007FFFFF onto physical
	 * 0x00000000-0x007FFFFF.  Bus address equals physical address, but only
	 * inside that 8 MB.
	 *
	 * A descriptor pointing anywhere else does not fail politely: the card
	 * masters, takes a master abort, and the whole board wedges with no
	 * oops and no console.  Buffers at 0x0E000000 (mem=224M) and 0x01800000
	 * (mem=24M) both did exactly that -- the second one despite CS_3's
	 * remap register covering 24-32 MB, because that BAR is not enabled.
	 *
	 * Reserving here rather than passing mem= keeps all 256 MB usable and
	 * puts the scratch where the card can see it.
	 */
	memblock_reserve(C2811_DMA_BASE, C2811_DMA_SIZE);
	pr_info("cisco2811: reserved %u KB at 0x%08x for card DMA "
		"(inside the 8 MB PCI inbound window)\n",
		C2811_DMA_SIZE >> 10, C2811_DMA_BASE);

	/*
	 * No PCI I/O space is wired up.  The MV64340 has two PCI host bridges
	 * (windows at phys 0x10000000 and 0x18000000), but a console + initramfs
	 * boot needs neither, and mainline dropped the mv64x60 host-bridge glue
	 * long ago -- see DYNAMIC_ANALYSIS.md sec 11.
	 */
}

void __init prom_init(void)
{
	/*
	 * ROMMON has already initialised and sized DRAM before handing us
	 * control, so there is nothing to probe: 256 MB at physical 0.
	 *
	 * IOS reports this as "249856K/12288K bytes of memory" -- the top 12 MB
	 * is Cisco's iomem carve-out for packet buffers, a software convention
	 * of theirs and not a hardware limit, so Linux claims the whole 256 MB.
	 *
	 * (The IOS decompressor glue clamps itself to 128 MB; that clamp is
	 * likewise Cisco's, not the board's.)
	 */
	memblock_add(0, C2811_DRAM_SIZE);

	/*
	 * Bring up earlyprintk on the console UART immediately.  Without this a
	 * failure before serial8250 probes is completely silent, which on this
	 * board is indistinguishable from "the kernel never started" -- and we
	 * only get one console.
	 */
	setup_8250_early_printk_port(CKSEG1ADDR(C2811_UART0_BASE),
				     C2811_UART_REGSHIFT, 0);
}

void __init prom_free_prom_memory(void)
{
	/* ROMMON lives in boot flash (phys 0x1FC00000), not in DRAM. */
}
