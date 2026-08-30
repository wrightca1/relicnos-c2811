// SPDX-License-Identifier: GPL-2.0
/*
 * Cisco 2811 interrupt handling -- CP0 only, for now.
 *
 * The MV64340 has a full interrupt controller (main_int_cause_{l,h},
 * int0_mask_{l,h}, int0_selcause, GPP) and its register map is recovered in
 * SYSTEM_CONTROLLER.md, but a console + initramfs boot does not need it:
 * serial8250 runs in polled mode when .irq == 0, and the timer is CP0
 * count/compare.  Wiring up the MV64340 irqchip is deliberately deferred so
 * that first boot has as little new code in it as possible.
 *
 * ROMMON leaves IP7, IP5 and IP3 unmasked (Status 0x3040a801, DYNAMIC_ANALYSIS
 * sec 6); IP7 is the CP0 timer.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <asm/time.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>

#include "cisco2811.h"

static void mv64340_cascade_runaway(unsigned int pending);

asmlinkage void plat_irq_dispatch(void)
{
	unsigned int pending = read_c0_status() & read_c0_cause() & ST0_IM;

	if (pending & CAUSEF_IP7) {		/* CP0 timer */
		do_IRQ(MIPS_CPU_IRQ_BASE + 7);
		return;
	}

	/*
	 * Anything else is the MV64340.  We do not know which CP0 IP line it
	 * asserts -- ROMMON leaves IP7/IP5/IP3 unmasked and IP7 is the timer,
	 * but that is not proof -- so rather than hardcode a guess, hand any
	 * non-timer assertion to the demux.  It returns false if the controller
	 * had nothing pending, which correctly falls through to spurious.
	 */
	if (pending & (CAUSEF_IP6 | CAUSEF_IP5 | CAUSEF_IP4 |
		       CAUSEF_IP3 | CAUSEF_IP2)) {
		if (mv64340_irq_dispatch())
			return;

		/*
		 * The line is asserted but the controller has nothing pending
		 * that we can service.  Returning here would hand back to a CPU
		 * that immediately re-takes the same interrupt -- a livelock
		 * that costs a PDU power cycle, and the reason claiming a
		 * cascade line hung this board twice.
		 *
		 * Mask the offending CP0 line instead.  Whatever drives it is
		 * something we do not yet understand; disabling it loses that
		 * source but keeps the board alive and, crucially, lets the
		 * deferred report say which line it was.
		 */
		mv64340_cascade_runaway(pending);
		return;
	}

	spurious_interrupt();
}

/*
 * Mask a CP0 line that keeps asserting with nothing serviceable behind it, and
 * record it for the deferred report.  Called from the dispatch path, so it must
 * not print or sleep -- it only touches Status.IM and a bitmap.
 */
static unsigned long cascade_runaway_mask;

static void mv64340_cascade_runaway(unsigned int pending)
{
	int ip;

	for (ip = 2; ip <= 6; ip++) {
		if (!(pending & (1u << (CAUSEB_IP + ip))))
			continue;
		clear_c0_status(1u << (STATUSB_IP0 + ip));
		set_bit(ip, &cascade_runaway_mask);
	}
}

/* Reported once things are running; see the note in mv64340_irq.c. */
static void cascade_runaway_report(struct work_struct *w)
{
	int ip;

	for_each_set_bit(ip, &cascade_runaway_mask, 8) {
		pr_warn("MV64340: CP0 IP%d asserted with nothing pending in the "
			"controller -- line masked. Remove it from cascade_ip[].\n",
			ip);
		clear_bit(ip, &cascade_runaway_mask);
	}
}
static DECLARE_DELAYED_WORK(cascade_runaway_work, cascade_runaway_report);

/*
 * CP0 IP lines the MV64340 drives.  Which one it actually uses is not documented
 * in anything we have, so claim every line that is free (IP7 is the CP0 timer,
 * IP0/IP1 are software) and let the one that fires identify itself in
 * /proc/interrupts.  This is only safe because mv64340_cascade_runaway() masks a
 * line that asserts with nothing serviceable behind it -- without that guard,
 * claiming a line the controller does not drive livelocks the board.
 */
static const int cascade_ip[] = { 2, 3, 4, 5, 6 };

void __init arch_init_irq(void)
{
	int i;

	mips_cpu_irq_init();
	mv64340_irq_init();

	/*
	 * Claim the CP0 line(s) the MV64340 cascades into.  Without this the CP0
	 * bits stay masked in Status.IM, the demux never fires, and eth0 can
	 * transmit but never receive.
	 *
	 * This is only safe now that mv64340_irq_init() masks BOTH cpu interrupt
	 * outputs (the chip has cpu_int0 and cpu_int1; we previously masked only
	 * cpu_int0, so a line driven by cpu_int1 asserted with nothing pending in
	 * the registers we consult -- a livelock) and the demux masks sources no
	 * driver has claimed.  Both hangs that cost PDU power cycles came from
	 * claiming a line before those two protections existed.
	 *
	 * IP3 and IP5: ROMMON leaves IP7/IP5/IP3 unmasked (Status 0x3040a801) and
	 * IP7 is the CP0 timer, so these are the controller's two outputs.
	 */
	schedule_delayed_work(&cascade_runaway_work, 5 * HZ);

	for (i = 0; i < ARRAY_SIZE(cascade_ip); i++) {
		if (request_irq(MIPS_CPU_IRQ_BASE + cascade_ip[i], no_action,
				IRQF_NO_THREAD, "mv64340 cascade", NULL))
			pr_err("failed to request CP0 IRQ %d as cascade\n",
			       cascade_ip[i]);
	}

}
