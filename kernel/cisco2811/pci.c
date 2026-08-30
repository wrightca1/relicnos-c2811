// SPDX-License-Identifier: GPL-2.0
/*
 * PCI host-bridge support for the Cisco 2811 (MV96340 / "GT96K").
 *
 * plat_mem_setup() used to be empty: a console + initramfs boot needs no PCI, and
 * mainline dropped the mv64x60 host-bridge glue with the PowerPC purge.  That is
 * fine until a card has to come up.  The NM-32A appears on PCI1, and without this
 * the bridge sits in whatever state ROMMON left -- which is command register
 * 0x0000: no memory space, and no BUS MASTER.  A host bridge that is not a bus
 * master cannot start a transaction, so no configuration cycle ever reaches the
 * slot and every scan reports an empty bus.  See DYNAMIC_ANALYSIS.md sec 63.
 *
 * Three things are needed, all established by comparing against a running IOS:
 *
 *   1. the PCI1 memory window pointed somewhere usable and its remap matching,
 *   2. the bridge's own command register set to MEM | MASTER,
 *   3. the slot enable in the system CPLD -- DevCS0+0x13 = 0x9b (sec 68).
 *
 * (3) is a board detail rather than a PCI one, but it belongs here: without it
 * the slot is electrically dead and enumeration finds nothing.
 *
 * Config access is the usual Galileo address/data pair.  The MV96340's registers
 * are little-endian and the CPU is big-endian, so every access is byte-swapped.
 * Note this applies to the host bridge AND to devices behind it -- the widely
 * repeated "swapped for the bridge, raw for everything else" quirk is not true on
 * this part, and believing it makes a real device read as 0xee101340 instead of
 * 0x401310ee (sec 67).
 */
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "cisco2811.h"

/*
 * Offsets from include/linux/mv643xx.h.  That header is not included directly:
 * it also declares mv64340_irq_init(), which collides with our own declaration
 * in cisco2811.h.  Only these three are needed.
 */
#define MV64340_PCI_1_MEMORY0_BASE_ADDR		0x0a0
#define MV64340_PCI_1_MEMORY0_SIZE		0x0a8
#define MV64340_PCI_1_MEMORY0_LOW_ADDR_REMAP	0x110
#define MV64340_PCI_1_MEMORY0_HIGH_ADDR_REMAP	0x340
#define MV64340_PCI_1_IO_BASE_ADDR		0x090
#define MV64340_PCI_1_IO_SIZE			0x098
#define MV64340_PCI_1_IO_ADDR_REMAP		0x108
#define MV64340_PCI_0_COMMAND			0xc00
#define MV64340_PCI_1_COMMAND			0xc80
#define MV64340_BASE_ADDR_ENABLE		0x278
#define MV64340_PCI_0_BASE_ADDR_REG_ENABLE	0xc3c
#define MV64340_PCI_1_BASE_ADDR_REG_ENABLE	0xcbc

/*
 * Which of the controller's own slave BARs are visible on PCI; a SET bit
 * DISABLES that BAR.  ROMMON leaves most of them on, and the PCI core then sees
 * the bridge advertising 8 MB + 8 MB + a ROM and tries to place them in our
 * 64 MB window, leaving nothing for the card ("BAR 0: can't assign; no space").
 * This is IOS's value: almost everything off.
 */
#define MV_BAR_ENABLE_VAL			0xFFFFFDFEu

/*
 * CPU address-decode enables, one bit per window, where a SET bit DISABLES the
 * window.  ROMMON leaves 0x0007FE0E, which has bit 15 set -- the PCI1 memory
 * window is switched OFF.  A CPU access into it is then not decoded at all and
 * takes a data bus error rather than reaching PCI, which is exactly what happens
 * if you assign a BAR there and try to read it.  IOS runs 0x00077A0E, i.e. these
 * two cleared.
 */
#define BAE_PCI0_MEM0				(1u << 10)
#define BAE_PCI1_MEM0				(1u << 15)
#define BAE_PCI1_IO				(1u << 14)

/*
 * The controller's INTERNAL PCI command register (not the config-space one).
 * Among other things it controls byte swapping for PCI accesses, and ROMMON's
 * value gets the byte lanes wrong: a 32-bit read of the card returns the
 * byte-swapped ADDRESS rather than data, and a byte read of GFRCR lands one lane
 * off -- the chip's 0xE0 shows up at +0x82 instead of +0x81.  That mis-read is
 * what produced the bogus "GFRCR = 0xE0" in sec 72.  This is IOS's value.
 */
#define MV_PCI_CMD_VAL				0x00170F41u

#define MV_PHYS			0x14000000UL
#define MV_LEN			0x10000

#define PCI1_CONFIG_ADDR	0xc78
#define PCI1_CONFIG_DATA	0xc7c

/* CPU-side window for PCI1 memory, and the PCI address it maps to.  Both are
 * programmed here rather than inherited: ROMMON leaves the window at
 * 0x22000000/32 MB, and the values below are the ones proven against IOS. */
#define PCI1_MEM_CPU_BASE	0x18000000UL
#define PCI1_MEM_SIZE		0x04000000UL	/* 64 MB */

/*
 * PCI I/O space.  This board originally wired none -- a console plus initramfs
 * boot needs no I/O BAR, so the window was left out and the PCI core was told
 * the region was empty.
 *
 * The NM-32A needs it.  The card requests a 16-byte I/O BAR which, with no
 * window, Linux could not place ("BAR 1: can't assign; no space").  The CD2481
 * only opens its data registers inside an interrupt-acknowledge cycle, and a
 * sweep of the entire 1 MB memory BAR for the LIVR vector found nothing
 * (DYNAMIC_ANALYSIS.md sec 104) -- IACKIN* is a separate bus cycle, so that
 * acknowledge port is not in memory space.  Sixteen bytes of I/O is the one
 * part of the card's address map we had never mapped.
 */
#define PCI1_IO_CPU_BASE	0x1C000000UL
#define PCI1_IO_SIZE		0x00100000UL	/* 1 MB */
#define PCI1_IO_PCI_BASE	0x00000000UL	/* PCI-side address of that window */

/* System CPLD slot enable.  0x0f out of reset; 0x9b is what IOS leaves, and
 * bisecting the seven bytes that differ showed this one alone is what makes the
 * card answer (sec 68). */
#define CPLD_PHYS		0x1FA00000UL
#define CPLD_SLOT_ENABLE	0x13
#define CPLD_SLOT_ENABLE_VAL	0x9b

static void __iomem *mv_base;

static inline u32 mv_read(unsigned int off)
{
	return swab32(__raw_readl(mv_base + off));
}

static inline void mv_write(unsigned int off, u32 val)
{
	__raw_writel(swab32(val), mv_base + off);
}

static int c2811_pci_addr(struct pci_bus *bus, unsigned int devfn, int where)
{
	/* Only the bridge's own bus is reachable: a Type 1 cycle needs a
	 * downstream bridge to claim it, and this board has none. */
	if (bus->number != 0)
		return -1;

	mv_write(PCI1_CONFIG_ADDR,
		 0x80000000u | (bus->number << 16) | (PCI_SLOT(devfn) << 11) |
		 (PCI_FUNC(devfn) << 8) | (where & 0xfc));
	return 0;
}

static int c2811_pci_read(struct pci_bus *bus, unsigned int devfn, int where,
			  int size, u32 *val)
{
	u32 data;

	if (c2811_pci_addr(bus, devfn, where)) {
		*val = 0xffffffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	/* IOS flushes the posted address write before touching DATA; the read
	 * below of the same register block has the same effect. */
	(void)mv_read(PCI1_CONFIG_ADDR);
	data = mv_read(PCI1_CONFIG_DATA);

	if (size == 1)
		*val = (data >> ((where & 3) << 3)) & 0xff;
	else if (size == 2)
		*val = (data >> ((where & 2) << 3)) & 0xffff;
	else
		*val = data;

	return PCIBIOS_SUCCESSFUL;
}

static int c2811_pci_write(struct pci_bus *bus, unsigned int devfn, int where,
			   int size, u32 val)
{
	u32 data;

	if (c2811_pci_addr(bus, devfn, where))
		return PCIBIOS_DEVICE_NOT_FOUND;

	(void)mv_read(PCI1_CONFIG_ADDR);

	if (size == 4) {
		data = val;
	} else {
		int shift;

		data = mv_read(PCI1_CONFIG_DATA);
		if (size == 1) {
			shift = (where & 3) << 3;
			data = (data & ~(0xffu << shift)) |
			       ((val & 0xff) << shift);
		} else {
			shift = (where & 2) << 3;
			data = (data & ~(0xffffu << shift)) |
			       ((val & 0xffff) << shift);
		}
		c2811_pci_addr(bus, devfn, where);
		(void)mv_read(PCI1_CONFIG_ADDR);
	}

	mv_write(PCI1_CONFIG_DATA, data);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops c2811_pci_ops = {
	.read	= c2811_pci_read,
	.write	= c2811_pci_write,
};

static struct resource c2811_pci_mem = {
	.name	= "PCI1 memory",
	.start	= PCI1_MEM_CPU_BASE,
	.end	= PCI1_MEM_CPU_BASE + PCI1_MEM_SIZE - 1,
	.flags	= IORESOURCE_MEM,
};

/*
 * PCI I/O space, in PCI-bus coordinates.
 *
 * The NM-32A's 16-byte I/O BAR is where its interrupt-acknowledge port must
 * live: a sweep of the whole 1 MB memory BAR for the LIVR vector found nothing
 * (§104), and IACKIN* is a separate bus cycle from a register access.
 *
 * This resource and the MV96340 window in c2811_pci_init() are a pair.  The
 * resource alone is worse than neither: io_map_base then points at an address
 * the CPU does not decode, and Linux's I/O probing during enumeration takes a
 * bus error that aborts the entire scan (§105).  The window was therefore
 * landed first and proven on hardware -- enumeration intact, and a read of
 * 0x1C000000 returning 0xff rather than a bus error -- before this was enabled.
 *
 * Starts at 0x1000 so the core never hands out port 0, which drivers routinely
 * treat as "unassigned", and ends at IO_SPACE_LIMIT.
 *
 * That ceiling is not cosmetic.  MIPS defines IO_SPACE_LIMIT as 0xffff, so
 * ioport_resource spans 0-0xffff; register_pci_controller() calls
 * request_resource() against it and, on failure, gives up with
 *
 *     Skipping PCI bus scan due to resource conflict
 *
 * and no devices are enumerated at all -- not even the bridge's own functions.
 * An earlier attempt declared 0x1000-0xfffff (1 MB, matching the CPU window)
 * and lost the entire bus that way.  The CPU-side window may be 1 MB; the
 * resource may not.  The card wants sixteen bytes.
 */
static struct resource c2811_pci_io = {
	.name	= "PCI1 I/O",
	.start	= 0x1000,
	.end	= IO_SPACE_LIMIT,
	.flags	= IORESOURCE_IO,
};

static struct pci_controller c2811_pci_controller = {
	.pci_ops	= &c2811_pci_ops,
	.mem_resource	= &c2811_pci_mem,
	.io_resource	= &c2811_pci_io,
};

static void __init c2811_slot_enable(void)
{
	void __iomem *cpld = ioremap(CPLD_PHYS, 0x100);
	u8 before;

	if (!cpld) {
		pr_warn("cisco2811: cannot map CPLD; NM slot left disabled\n");
		return;
	}

	before = __raw_readb(cpld + CPLD_SLOT_ENABLE);
	__raw_writeb(CPLD_SLOT_ENABLE_VAL, cpld + CPLD_SLOT_ENABLE);
	pr_info("cisco2811: NM slot enable 0x%02x -> 0x%02x\n",
		before, __raw_readb(cpld + CPLD_SLOT_ENABLE));
	iounmap(cpld);
}

static int __init c2811_pci_init(void)
{
	u32 cmd;

	mv_base = ioremap(MV_PHYS, MV_LEN);
	if (!mv_base) {
		pr_err("cisco2811: cannot map MV96340 registers\n");
		return -ENOMEM;
	}

	c2811_slot_enable();

	/* Point the CPU window at PCI1 memory and make the remap match, so a CPU
	 * physical address in the window reaches the same PCI address. */
	mv_write(MV64340_PCI_1_MEMORY0_BASE_ADDR,
		 0x01000000u | (PCI1_MEM_CPU_BASE >> 16));
	mv_write(MV64340_PCI_1_MEMORY0_SIZE, (PCI1_MEM_SIZE >> 16) - 1);
	mv_write(MV64340_PCI_1_MEMORY0_LOW_ADDR_REMAP, PCI1_MEM_CPU_BASE >> 16);
	mv_write(MV64340_PCI_1_MEMORY0_HIGH_ADDR_REMAP, 0);

	/* Enable memory space and bus mastering on the bridge itself.  Without
	 * the master bit no configuration cycle leaves the chip. */
	mv_write(PCI1_CONFIG_ADDR, 0x80000000u | (0 << 16) | (0 << 11) | 0x04);
	(void)mv_read(PCI1_CONFIG_ADDR);
	cmd = mv_read(PCI1_CONFIG_DATA);
	/*
	 * PCI_COMMAND_IO is needed now that the NM-32A's I/O BAR is placed: with
	 * only MEM|MASTER the bridge read back 0x0006 and an inb() on the card's
	 * port never completed, hanging the board even with every interrupt
	 * disabled and nothing to acknowledge (§107).
	 *
	 * Deliberately NOT PCI_COMMAND_PARITY | PCI_COMMAND_SERR.
	 *
	 * The NM-32A bus-masters into host memory, and the MV96340 exposes only
	 * an 8 MB inbound window, so a descriptor pointing outside it takes a
	 * master abort.  With SERR enabled that abort escalates into a bus error
	 * that wedges the CPU: the board dies with no oops and no console, which
	 * cost several power cycles before the cause was identified.  Without
	 * it the transfer simply fails and the OWN bit stays set, which is
	 * observable and debuggable.
	 */
	cmd = (cmd & 0xffff0000u) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
	      PCI_COMMAND_IO;
	mv_write(PCI1_CONFIG_ADDR, 0x80000000u | (0 << 16) | (0 << 11) | 0x04);
	(void)mv_read(PCI1_CONFIG_ADDR);
	mv_write(PCI1_CONFIG_DATA, cmd);

	/* Byte-lane / swap behaviour for PCI accesses. */
	mv_write(MV64340_PCI_0_COMMAND, MV_PCI_CMD_VAL);
	mv_write(MV64340_PCI_1_COMMAND, MV_PCI_CMD_VAL);

	/* Enable the CPU decode for the PCI memory windows (bit set = disabled). */
	mv_write(MV64340_BASE_ADDR_ENABLE,
		 mv_read(MV64340_BASE_ADDR_ENABLE) &
		 ~(BAE_PCI0_MEM0 | BAE_PCI1_MEM0));

	/* Turn off the controller's own slave BARs so they stop competing for the
	 * window with the card we actually want to place in it. */
	mv_write(MV64340_PCI_0_BASE_ADDR_REG_ENABLE, MV_BAR_ENABLE_VAL);
	mv_write(MV64340_PCI_1_BASE_ADDR_REG_ENABLE, MV_BAR_ENABLE_VAL);

	pr_info("cisco2811: PCI1 bridge command 0x%04x, window 0x%08lx+0x%lx\n",
		cmd & 0xffff, PCI1_MEM_CPU_BASE, PCI1_MEM_SIZE);

	/*
	 * Program the PCI1 I/O window but do NOT declare the resource yet.
	 *
	 * §105: declaring the resource without a decoding window aborts the
	 * whole PCI scan, and doing both at once made the failure impossible to
	 * attribute.  So this step changes one thing -- the window -- and the
	 * question it answers is simply "does enumeration still work?".
	 *
	 * Whether these registers use the memory window's 64 KB unit encoding,
	 * and whether bit 14 of BASE_ADDR_ENABLE really is PCI1-I/O, are both
	 * assumptions; the readbacks below are logged so they can be checked
	 * rather than believed.  The window is not touched from the kernel
	 * afterwards: probing whether the CPU decodes it is done from userspace,
	 * where a bus error costs a SIGBUS instead of a panic.
	 */
	{
		u32 bae_before, bae_after;

		mv_write(MV64340_PCI_1_IO_BASE_ADDR, PCI1_IO_CPU_BASE >> 16);
		mv_write(MV64340_PCI_1_IO_SIZE, (PCI1_IO_SIZE >> 16) - 1);
		mv_write(MV64340_PCI_1_IO_ADDR_REMAP, PCI1_IO_PCI_BASE >> 16);

		bae_before = mv_read(MV64340_BASE_ADDR_ENABLE);
		mv_write(MV64340_BASE_ADDR_ENABLE, bae_before & ~BAE_PCI1_IO);
		bae_after = mv_read(MV64340_BASE_ADDR_ENABLE);

		pr_info("cisco2811: PCI1 I/O window base=%08x size=%08x remap=%08x\n",
			mv_read(MV64340_PCI_1_IO_BASE_ADDR),
			mv_read(MV64340_PCI_1_IO_SIZE),
			mv_read(MV64340_PCI_1_IO_ADDR_REMAP));
		pr_info("cisco2811: base_addr_enable %08x -> %08x (cleared bit 14)\n",
			bae_before, bae_after);
		pr_info("cisco2811: probe it from userspace: peek 0x%08lx\n",
			PCI1_IO_CPU_BASE);
	}

	/* Window proven above, so I/O ports resolve through it. */
	c2811_pci_controller.io_map_base = CKSEG1ADDR(PCI1_IO_CPU_BASE);
	set_io_port_base(CKSEG1ADDR(PCI1_IO_CPU_BASE));

	register_pci_controller(&c2811_pci_controller);
	return 0;
}
arch_initcall(c2811_pci_init);

/*
 * The MV96340 appears on its own bus as eight functions of 11ab:9634, and each
 * advertises BARs -- 8 MB windows, a register block at 0x14000000, a ROM at
 * 0x1f000000.  Those describe the controller's own decode, not devices that need
 * placing, and they all sit outside the window we hand the PCI core.  Left alone,
 * the core tries to relocate them INTO that window and then has nothing left for
 * the card:
 *
 *   pci 0000:00:01.0: BAR 0 [mem size 0x00100000]: can't assign; no space
 *
 * Disabling them in the controller's BAR-enable register is not enough -- that
 * stops them responding but they still read back in config space.  Drop the
 * resources instead, so the core ignores the bridge and places only real devices.
 */
static void c2811_bridge_no_resources(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < PCI_NUM_RESOURCES; i++)
		memset(&dev->resource[i], 0, sizeof(dev->resource[i]));

	/*
	 * Clearing the resources alone is not sufficient -- functions .3 and .4
	 * still ended up claiming 32 MB each and swallowing the whole window:
	 *
	 *   18000000-1bffffff : PCI1 memory
	 *     18000000-19ffffff : 0000:00:00.3
	 *     1a000000-1bffffff : 0000:00:00.4
	 *
	 * Re-classing them as the host bridge makes the PCI core skip resource
	 * assignment for them altogether, which is what they are.
	 */
	dev->class = PCI_CLASS_BRIDGE_HOST << 8;

	dev_info(&dev->dev, "host bridge: BARs dropped, window left for cards\n");
}
DECLARE_PCI_FIXUP_EARLY(0x11ab, 0x9634, c2811_bridge_no_resources);

/* No PCI interrupts are routed on this board yet; the NM-32A is polled until the
 * MV96340 cause bit for the slot is identified. */
int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	return 0;
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}
