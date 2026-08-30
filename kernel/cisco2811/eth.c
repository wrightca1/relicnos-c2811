// SPDX-License-Identifier: GPL-2.0
/*
 * FastEthernet0/0 and 0/1 on the Cisco 2811 -- the MV64340's integrated MACs.
 *
 * Identified live: "show controllers fastethernet 0/0" reports
 * "Hardware is MV96340", with the SMI block at 0xB4002000 and per-port control
 * registers at 0xB4002400, and 8 rx + 8 tx rings.  Those three facts together
 * are the mv643xx_eth register layout exactly (SMI at +0x2000, port n at
 * +0x2400 + n*0x400), so no custom driver is needed -- see
 * DYNAMIC_ANALYSIS.md sec 3.
 *
 * The PHY is a Broadcom BCM5221-family part (PHY ID 0x0040:0x61E4, OUI
 * 00-10-18) -- NOT the Marvell 88E1000 the IOS image's strings suggested.
 * drivers/net/phy/broadcom.c covers it.
 *
 * Its MDIO address is 1, from the live PHYAR register, not 0x0B.  See the note
 * in cisco2811.h: 0x0B was SMIR's DATA field being misread as the address.
 *
 * The MAC address is the chassis address from the board cookie,
 * 0022.5522.c3a8, which "show interfaces" reports as the bia.
 *
 * NOTE mv643xx_eth is gated in mainline by
 *     depends on PPC32 || PLAT_ORION || COMPILE_TEST
 * so a Kconfig change is required to build it for MIPS; install.sh patches it.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/mv643xx_eth.h>

#include "cisco2811.h"

#define MV_ETH_REGS_BASE	(C2811_MV64340_BASE + MV643XX_ETH_SHARED_REGS)

static struct resource c2811_eth_shared_resources[] = {
	{
		.name	= "ethernet shared base",
		.start	= MV_ETH_REGS_BASE,
		.end	= MV_ETH_REGS_BASE + MV643XX_ETH_SHARED_REGS_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct mv643xx_eth_shared_platform_data c2811_eth_shared_pd = {
	/*
	 * .dram is left NULL: that field describes MBUS address-decode windows
	 * for Orion-class SoCs.  On this board the Ethernet unit's own BAR is
	 * already programmed to cover all of DRAM -- the live SMI dump showed
	 * BAR0 = 0x00000E00 / SR0 = 0x0FFF0000, i.e. base 0, size 256 MB,
	 * target DRAM (DYNAMIC_ANALYSIS.md sec 3).
	 */
	.tx_csum_limit = 0,
};

static struct platform_device c2811_eth_shared_device = {
	.name		= MV643XX_ETH_SHARED_NAME,
	.id		= 0,
	.num_resources	= ARRAY_SIZE(c2811_eth_shared_resources),
	.resource	= c2811_eth_shared_resources,
	.dev = {
		.platform_data = &c2811_eth_shared_pd,
	},
};

/*
 * The MDIO bus is a SEPARATE platform driver ("orion-mdio", drivers/net/
 * ethernet/marvell/mvmdio.c) -- mv643xx_eth does not provide it, it only looks
 * the bus up by the id "orion-mdio-mii".  Without this device registered the
 * bus never exists and every PHY scan fails with
 *     libphy: PHY orion-mdio-mii:01 not found
 *
 * mvmdio's resource base IS the SMI register.  On the MV64340 the SMI block
 * starts at +0x2000 with PHYAR first and SMIR at +0x2004 -- matching the live
 * dump, which printed "Serial Management Interface Registers (0xB4002000)"
 * with PHYAR then SMIR.  Length 0x84 covers the error-interrupt registers the
 * driver also touches (MVMDIO_ERR_INT_CAUSE 0x7c, _MASK 0x80).
 */
#define MV_ETH_SMI_REG		(C2811_MV64340_BASE + 0x2004)

static struct resource c2811_mdio_resources[] = {
	{
		.name	= "orion-mdio smi",
		.start	= MV_ETH_SMI_REG,
		.end	= MV_ETH_SMI_REG + 0x83,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device c2811_mdio_device = {
	.name		= "orion-mdio",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(c2811_mdio_resources),
	.resource	= c2811_mdio_resources,
};

static struct mv643xx_eth_platform_data c2811_eth0_pd = {
	.shared		= &c2811_eth_shared_device,
	.port_number	= C2811_ETH_PORT0,
	.phy_addr	= MV643XX_ETH_PHY_ADDR(C2811_ETH_PHY_ADDR),
	.mac_addr	= { 0x00, 0x22, 0x55, 0x22, 0xc3, 0xa8 },
};

/*
 * The interrupt is supplied as a resource rather than baked in, because the
 * MV64340 cause bit for the Ethernet port is the one number we could not
 * derive: mv643xx.h carries register offsets but no bit map, and we do not
 * have the datasheet.  CONFIG_CISCO2811_ETH_IRQ_BIT is therefore a build-time
 * knob, and CONFIG_CISCO2811_IRQ_DISCOVERY logs the bit that actually fires so
 * it can be set from observation rather than guesswork.
 */
static struct resource c2811_eth0_resources[] = {
	{
		.name	= "eth0 irq",
		.start	= C2811_MV_IRQ_BASE + CONFIG_CISCO2811_ETH_IRQ_BIT,
		.end	= C2811_MV_IRQ_BASE + CONFIG_CISCO2811_ETH_IRQ_BIT,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device c2811_eth0_device = {
	.name		= MV643XX_ETH_NAME,
	.id		= 0,
	.num_resources	= ARRAY_SIZE(c2811_eth0_resources),
	.resource	= c2811_eth0_resources,
	.dev = {
		.platform_data = &c2811_eth0_pd,
	},
};

/*
 * Fa0/1: the same shape as port 0, PHY address 2 (see cisco2811.h).
 *
 * The MAC follows Fa0/0's from the board's own block rather than being
 * invented: Cisco allocates consecutive addresses to the onboard ports.
 *
 * The interrupt bit is a reasoned guess, not a measurement.  Bit 32 was
 * confirmed as port 0 by watching the cause register while the port was
 * pinged; the old PowerPC mv64x60 PIC numbered the Ethernet ports
 * consecutively, so port 1 should be the next bit.  It has not been verified
 * on the wire, because this port has never been cabled here -- with the link
 * down the PHY should still be detected, which is the part worth checking.
 */
static struct mv643xx_eth_platform_data c2811_eth1_pd = {
	.shared		= &c2811_eth_shared_device,
	.port_number	= C2811_ETH_PORT1,
	.phy_addr	= MV643XX_ETH_PHY_ADDR(C2811_ETH_PHY_ADDR + 1),
	.mac_addr	= { 0x00, 0x22, 0x55, 0x22, 0xc3, 0xa9 },
};

static struct resource c2811_eth1_resources[] = {
	{
		.name	= "eth1 irq",
		.start	= C2811_MV_IRQ_BASE + CONFIG_CISCO2811_ETH_IRQ_BIT + 1,
		.end	= C2811_MV_IRQ_BASE + CONFIG_CISCO2811_ETH_IRQ_BIT + 1,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device c2811_eth1_device = {
	.name		= MV643XX_ETH_NAME,
	.id		= 1,
	.num_resources	= ARRAY_SIZE(c2811_eth1_resources),
	.resource	= c2811_eth1_resources,
	.dev = {
		.platform_data = &c2811_eth1_pd,
	},
};

static struct platform_device *c2811_eth_devices[] __initdata = {
	&c2811_eth_shared_device,
	&c2811_mdio_device,		/* must exist before the ports probe */
	&c2811_eth0_device,
	&c2811_eth1_device,
};

static int __init c2811_eth_init(void)
{
	return platform_add_devices(c2811_eth_devices,
				    ARRAY_SIZE(c2811_eth_devices));
}
device_initcall(c2811_eth_init);
