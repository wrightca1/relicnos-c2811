// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell MV64340 interrupt controller for the Cisco 2811.
 *
 * Needed because mv643xx_eth uses NAPI and therefore genuinely needs an
 * interrupt -- unlike the console 16550, which we run in polled mode
 * (.irq = 0) precisely to avoid needing this on first boot.
 *
 * Register offsets come from include/linux/mv643xx.h, which is still in
 * mainline as of v6.6 (it disappears in v6.12 -- see DYNAMIC_ANALYSIS.md
 * sec 13).  The live values IOS leaves behind are in
 * analysis/dynamic_session_2026-08-25.log:
 *
 *     main_int_cause_l  0x00000000     cpu_int0_mask_l  0x00000F30
 *     main_int_cause_h  0x00000000     cpu_int0_mask_h  0x00000000
 *
 * i.e. IOS enables cause-low bits 4,5,8,9,10,11 and nothing in the high word.
 *
 * TWO THINGS ARE NOT KNOWN YET, and both are handled rather than guessed:
 *
 *  1. WHICH CP0 IP LINE the controller asserts.  ROMMON leaves IP7/IP5/IP3
 *     unmasked (Status 0x3040a801); IP7 is the CP0 timer, so it is one of the
 *     others -- but we have not confirmed which.  Rather than hardcode it,
 *     plat_irq_dispatch() routes *any* non-timer IP to this demux, which is
 *     correct whichever line it turns out to be.
 *
 *  2. WHICH CAUSE BIT is the Ethernet port.  The MV64340 datasheet assigns
 *     these; we do not have it, and mv643xx.h carries offsets but no bit map.
 *     So CONFIG_CISCO2811_IRQ_DISCOVERY logs every cause bit the first time it
 *     fires, which identifies the Ethernet bit empirically from link activity.
 *
 * ENDIANNESS: the MV64340 register file is little-endian while this CPU is
 * big-endian -- that is why IOS's own dump prints every register twice, raw
 * and byte-swapped.
 *
 * readl()/writel() only do that swap when the platform selects
 * CONFIG_SWAP_IO_SPACE, which this board therefore does.  Note the swap comes
 * from ioswabl(), NOT __mem_ioswabl() -- the __mem_ variants apply to the
 * string I/O helpers (readsl/writesl), not to readl/writel.  Getting that
 * backwards costs you every 16/32-bit register in this chip: mvmdio wrote
 * byte-reversed SMI commands and reported "SMI bus read not valid" until
 * SWAP_IO_SPACE was set.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/workqueue.h>
#include <linux/bitops.h>

#include <asm/addrspace.h>
#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>

#include "cisco2811.h"

/* from include/linux/mv643xx.h */
#define MV_MAIN_CAUSE_LOW	0x004
#define MV_MAIN_CAUSE_HIGH	0x00c
#define MV_CPU0_MASK_LOW	0x014
#define MV_CPU0_MASK_HIGH	0x01c
#define MV_CPU1_MASK_LOW	0x034
#define MV_CPU1_MASK_HIGH	0x03c

static void __iomem *mv_base;

/*
 * Sources that fired with no driver claiming them.  Recorded here and reported
 * from a workqueue -- NEVER printed inline: the console is serial8250 with
 * .irq = 0, i.e. polled at 9600 baud, and printing from the dispatch path is
 * far too slow to do with interrupts in that state.
 */
static DECLARE_BITMAP(mv_unclaimed, C2811_MV_NR_IRQS);

static void mv_unclaimed_report(struct work_struct *w)
{
	int bit;

	for_each_set_bit(bit, mv_unclaimed, C2811_MV_NR_IRQS) {
		pr_warn("MV64340: masked unclaimed interrupt, cause bit %d (irq %d). "
			"If this is the Ethernet port set CONFIG_CISCO2811_ETH_IRQ_BIT=%d\n",
			bit, C2811_MV_IRQ_BASE + bit, bit);
		clear_bit(bit, mv_unclaimed);
	}
}
static DECLARE_DELAYED_WORK(mv_unclaimed_work, mv_unclaimed_report);

static inline void __iomem *mv_reg(unsigned int off)
{
	return mv_base + off;
}

static void mv64340_irq_mask(struct irq_data *d)
{
	unsigned int bit = d->irq - C2811_MV_IRQ_BASE;
	void __iomem *r = mv_reg(bit < 32 ? MV_CPU0_MASK_LOW : MV_CPU0_MASK_HIGH);

	writel(readl(r) & ~BIT(bit & 31), r);
}

static void mv64340_irq_unmask(struct irq_data *d)
{
	unsigned int bit = d->irq - C2811_MV_IRQ_BASE;
	void __iomem *r = mv_reg(bit < 32 ? MV_CPU0_MASK_LOW : MV_CPU0_MASK_HIGH);

	writel(readl(r) | BIT(bit & 31), r);
}

static struct irq_chip mv64340_irq_chip = {
	.name		= "MV64340",
	.irq_mask	= mv64340_irq_mask,
	.irq_mask_ack	= mv64340_irq_mask,
	.irq_unmask	= mv64340_irq_unmask,
};

/*
 * Demux one controller interrupt.  Returns true if anything was dispatched, so
 * the caller can tell a real cascade from a spurious CPU-line assertion --
 * which also tells us, on the first boot, which IP line this thing uses.
 */
bool mv64340_irq_dispatch(void)
{
	u32 lo = readl(mv_reg(MV_MAIN_CAUSE_LOW))  & readl(mv_reg(MV_CPU0_MASK_LOW));
	u32 hi = readl(mv_reg(MV_MAIN_CAUSE_HIGH)) & readl(mv_reg(MV_CPU0_MASK_HIGH));
	unsigned int irq;
	int bit;

	if (lo) {
		bit = __ffs(lo);
	} else if (hi) {
		bit = 32 + __ffs(hi);
	} else {
		return false;
	}

	irq = C2811_MV_IRQ_BASE + bit;

	/*
	 * Mask any source no driver has claimed.  These are level triggered, so
	 * dispatching to an unclaimed irq means nothing clears the originating
	 * device and it re-asserts immediately -- an unrecoverable livelock.
	 * Masking turns a wrong CONFIG_CISCO2811_ETH_IRQ_BIT into a log line.
	 */
	if (!irq_has_action(irq)) {
		void __iomem *r = mv_reg(bit < 32 ? MV_CPU0_MASK_LOW
						  : MV_CPU0_MASK_HIGH);

		writel(readl(r) & ~BIT(bit & 31), r);
		set_bit(bit, mv_unclaimed);
		schedule_delayed_work(&mv_unclaimed_work, HZ);
		return true;
	}

	/*
	 * NOTE: an earlier revision masked unclaimed sources here and logged
	 * them, to turn a wrong CONFIG_CISCO2811_ETH_IRQ_BIT into a diagnostic
	 * rather than a livelock.  That is the right idea but the wrong place:
	 * pr_warn() from the dispatch path writes to a POLLED 9600-baud console
	 * (serial8250 .irq = 0), which is far too slow to do with interrupts in
	 * this state -- the board stopped booting entirely.
	 *
	 * If revisiting: mask the source here but defer the logging (e.g. record
	 * a bitmask and print it from a workqueue or on demand via debugfs),
	 * never print inline.
	 */
	do_IRQ(irq);
	return true;
}

void __init mv64340_irq_init(void)
{
	int i;

	mv_base = (void __iomem *)CKSEG1ADDR(C2811_MV64340_BASE);

	/*
	 * Mask everything.  IOS leaves 0x00000F30 enabled in the low word and we
	 * must not inherit its idea of which sources matter -- Linux will unmask
	 * what it actually requests.
	 */
	writel(0, mv_reg(MV_CPU0_MASK_LOW));
	writel(0, mv_reg(MV_CPU0_MASK_HIGH));

	/*
	 * Mask cpu_int1 as well.  The MV64340 has TWO CPU interrupt outputs and
	 * this demux only consults the cpu_int0 masks -- so if the board wires
	 * cpu_int1 to a CP0 line and IOS left sources enabled there, that line
	 * asserts while we find nothing pending, and claiming it as a cascade
	 * livelocks the board.  That is the most likely explanation for IP3/IP5
	 * hanging at init in the earlier attempt.
	 */
	writel(0, mv_reg(MV_CPU1_MASK_LOW));
	writel(0, mv_reg(MV_CPU1_MASK_HIGH));

	for (i = 0; i < C2811_MV_NR_IRQS; i++)
		irq_set_chip_and_handler(C2811_MV_IRQ_BASE + i,
					 &mv64340_irq_chip, handle_level_irq);

	pr_info("MV64340 interrupt controller at 0x%08x, irqs %d..%d\n",
		C2811_MV64340_BASE, C2811_MV_IRQ_BASE,
		C2811_MV_IRQ_BASE + C2811_MV_NR_IRQS - 1);
}
