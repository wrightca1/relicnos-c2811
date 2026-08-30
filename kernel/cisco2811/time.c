// SPDX-License-Identifier: GPL-2.0
/*
 * Cisco 2811 time init.
 *
 * The CP0 count frequency is hardcoded rather than calibrated because we have
 * no independent timer to calibrate against yet: the MV64340's ct0..ct3 timers
 * are mapped but not driven, and unlike Cobalt (which measures CP0 against a
 * GT641xx timer) there is nothing else running this early.
 *
 * The value is measured, not nominal -- see DYNAMIC_ANALYSIS.md sec 12.
 */
#include <linux/init.h>
#include <linux/printk.h>

#include <asm/time.h>

#include "cisco2811.h"

void __init plat_time_init(void)
{
	mips_hpt_frequency = C2811_HPT_FREQUENCY;

	pr_info("CP0 count frequency %u Hz (measured; CPU 350 MHz, sysclk 50 MHz)\n",
		mips_hpt_frequency);
}
