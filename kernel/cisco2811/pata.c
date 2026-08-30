// SPDX-License-Identifier: GPL-2.0
/*
 * CompactFlash (True IDE) support for the Cisco 2811.
 *
 * The card is an ATA device on the MV64340 device bus, chip select DevCS3 at
 * physical 0x1FB00000.  None of that is documented anywhere we have; it was
 * recovered from ROMMON's own driver and then confirmed on the hardware.
 *
 * Register layout
 * ---------------
 * The task file is not at a uniform stride, so this cannot use pata_platform's
 * ioport_shift.  ATA registers are grouped in PAIRS, one pair per 32-bit word:
 * pair k lives at word offset 4k, with register 2k+1 at byte 4k+2 and register
 * 2k at byte 4k+3.  That gives:
 *
 *	reg 7 status/command	0x0E	reg 6 device	0x0F
 *	reg 5 lbah		0x0A	reg 4 lbam	0x0B
 *	reg 3 lbal		0x06	reg 2 nsect	0x07
 *	reg 1 error/feature	0x02	reg 0 data	0x03
 *	device control / altstatus	0x1F
 *
 * plus a 16-bit data port at 0x12.
 *
 * Access width
 * ------------
 * Byte READS work on both lanes, but byte WRITES only take on ODD addresses --
 * the low byte of each pair.  Measured with libata's own ata_devchk() pattern:
 *
 *	wrote nsect(0x07)=55 lbal(0x06)=AA -> read back 55 / 00
 *	wrote nsect(0x07)=AA lbal(0x06)=55 -> read back AA / FF
 *
 * nsect (odd) takes the write; lbal (even) never does.  So lbal, lbah, feature
 * and the command register cannot be written a byte at a time, and every write
 * here goes out as a halfword covering the whole pair.  This is why ROMMON uses
 * nothing but `sh`/`lhu`, and why libata's stock SFF write paths report "no
 * device": ata_devchk()'s readback fails on the even register.
 *
 * ROMMON corroborates every offset: it polls status at +0x0E testing 0x88
 * (BSY|DRQ) and 0x50 (DRDY|DSC), issues commands as a halfword with the opcode
 * in the high byte, streams data at +0x12, and pulses SRST in the low byte at
 * +0x1E.
 *
 * Data byte order
 * ---------------
 * ATA delivers data words little-endian on the medium, so on this big-endian
 * CPU the two bytes of each word must be swapped when filling the buffer.  The
 * proof is the MBR: read without swapping, its signature comes back as 0xAA55
 * instead of 0x55AA.  IDENTIFY strings look correct *without* the swap only
 * because the ATA spec stores those the other way round -- which is exactly the
 * trap that makes this worth spelling out.
 *
 * No interrupt is wired to the CF, so the port runs polled.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ata.h>
#include <linux/libata.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <scsi/scsi_host.h>

#include "cisco2811.h"

#define DRV_NAME	"pata_cisco2811"

/* byte offsets of each ATA register within the DevCS3 window */
#define CF_DATA16	0x12	/* 16-bit data port */
#define CF_FEATURE	0x02
#define CF_ERROR	0x02
#define CF_NSECT	0x07
#define CF_LBAL		0x06
#define CF_LBAM		0x0B
#define CF_LBAH		0x0A
#define CF_DEVICE	0x0F
#define CF_STATUS	0x0E
#define CF_COMMAND	0x0E
#define CF_ALTSTATUS	0x1F
#define CF_CTL		0x1F

#define CF_WINDOW_LEN	0x40

/* Card detect lives in the CPLD on DevCS0, not on the CF window itself. */
#define CF_CD_PHYS	0x1FA00014
#define CF_CD_BIT	0x80	/* clear = card present */

/* Register PAIRS, as halfword offsets (the ^2 lane swap is already folded in). */
#define P_FEAT		0x02	/* hi = feature/error,     lo = data byte    */
#define P_NSECT_LBAL	0x06	/* hi = lbal,              lo = nsect        */
#define P_LBAM_LBAH	0x0A	/* hi = lbah,              lo = lbam         */
#define P_DEV_CMD	0x0E	/* hi = status/command,    lo = device       */
#define P_DEVCTL	0x1E	/* lo = device control / altstatus           */

static inline void cf_w16(void __iomem *base, unsigned int off, u16 v)
{
	__raw_writew(v, base + off);
}

/*
 * Load the taskfile using halfword writes.  Each write necessarily sets both
 * registers of its pair, which is harmless: libata hands us the whole taskfile
 * at once, so both halves are the values we want anyway.
 */
static void c2811_tf_load(struct ata_port *ap, const struct ata_taskfile *tf)
{
	void __iomem *base = ap->ioaddr.cmd_addr;
	unsigned int is_addr = tf->flags & ATA_TFLAG_ISADDR;

	if (tf->ctl != ap->last_ctl) {
		cf_w16(base, P_DEVCTL, tf->ctl);
		ap->last_ctl = tf->ctl;
		ata_wait_idle(ap);
	}

	if (is_addr && (tf->flags & ATA_TFLAG_LBA48)) {
		cf_w16(base, P_FEAT, (u16)tf->hob_feature << 8);
		cf_w16(base, P_NSECT_LBAL, ((u16)tf->hob_lbal << 8) | tf->hob_nsect);
		cf_w16(base, P_LBAM_LBAH, ((u16)tf->hob_lbah << 8) | tf->hob_lbam);
	}

	if (is_addr) {
		cf_w16(base, P_FEAT, (u16)tf->feature << 8);
		cf_w16(base, P_NSECT_LBAL, ((u16)tf->lbal << 8) | tf->nsect);
		cf_w16(base, P_LBAM_LBAH, ((u16)tf->lbah << 8) | tf->lbam);
	}

	/*
	 * The device register is the low byte of the command register's pair.
	 * It is written together with the command in c2811_exec_command() --
	 * writing it here would need a halfword whose high byte lands in the
	 * command register, i.e. would start a command.
	 */
	ata_wait_idle(ap);
}

static void c2811_exec_command(struct ata_port *ap, const struct ata_taskfile *tf)
{
	cf_w16(ap->ioaddr.cmd_addr, P_DEV_CMD,
	       ((u16)tf->command << 8) | tf->device);
	ata_sff_pause(ap);
}

/* Single device on the slot, and its byte rides along with the command. */
static void c2811_dev_select(struct ata_port *ap, unsigned int device)
{
}

static void c2811_set_devctl(struct ata_port *ap, u8 ctl)
{
	cf_w16(ap->ioaddr.cmd_addr, P_DEVCTL, ctl);
}

/*
 * ata_sff_softreset() cannot be used: it gates on ata_devchk(), which byte-
 * writes lbal and so always fails here.  Presence is already known from the
 * CPLD card-detect checked at probe, so pulse SRST -- exactly as ROMMON does --
 * and classify from the signature registers, which are readable a byte at a time.
 */
static int c2811_softreset(struct ata_link *link, unsigned int *classes,
			   unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	void __iomem *base = ap->ioaddr.cmd_addr;
	u8 err = 0;
	int rc;

	cf_w16(base, P_DEVCTL, ap->ctl | ATA_SRST);
	udelay(20);
	cf_w16(base, P_DEVCTL, ap->ctl);
	ap->last_ctl = ap->ctl;

	rc = ata_sff_wait_after_reset(link, 1, deadline);
	if (rc) {
		ata_link_err(link, "SRST failed (errno=%d)\n", rc);
		return rc;
	}

	classes[0] = ata_sff_dev_classify(&link->device[0], 1, &err);
	return 0;
}

/*
 * Transfer data through the 16-bit port, swapping each word into
 * little-endian byte order.  ata_sff_data_xfer() cannot be used: its
 * ioread16_rep()/iowrite16_rep() store raw words, which on a big-endian host
 * yields byte-swapped sectors.
 */
static unsigned int c2811_data_xfer(struct ata_queued_cmd *qc, unsigned char *buf,
				    unsigned int buflen, int rw)
{
	void __iomem *data = qc->ap->ioaddr.data_addr;
	unsigned int words = buflen >> 1;
	unsigned int i;

	if (rw == READ) {
		for (i = 0; i < words; i++) {
			u16 w = __raw_readw(data);

			buf[i * 2]     = w & 0xff;
			buf[i * 2 + 1] = w >> 8;
		}
	} else {
		for (i = 0; i < words; i++)
			__raw_writew(buf[i * 2] | (buf[i * 2 + 1] << 8), data);
	}

	/* Odd-length transfer: pad the final word, as libata's own helper does. */
	if (buflen & 1) {
		u16 w;

		if (rw == READ) {
			w = __raw_readw(data);
			buf[buflen - 1] = w & 0xff;
		} else {
			__raw_writew(buf[buflen - 1], data);
		}
	}

	return buflen;
}

static struct scsi_host_template c2811_sht = {
	ATA_PIO_SHT(DRV_NAME),
};

static struct ata_port_operations c2811_port_ops = {
	.inherits	= &ata_sff_port_ops,
	.sff_tf_load	= c2811_tf_load,
	.sff_exec_command = c2811_exec_command,
	.sff_dev_select	= c2811_dev_select,
	.sff_set_devctl	= c2811_set_devctl,
	.sff_data_xfer	= c2811_data_xfer,
	.softreset	= c2811_softreset,
	.cable_detect	= ata_cable_unknown,
};

static int c2811_pata_probe(struct platform_device *pdev)
{
	struct ata_host *host;
	struct ata_port *ap;
	struct ata_ioports *io;
	void __iomem *base, *cd;
	u8 cdreg;

	cd = ioremap(CF_CD_PHYS & ~0x3, 4);
	if (cd) {
		/*
		 * Probing an empty slot would leave libata polling a floating
		 * bus, so check the CPLD's card-detect first.  Active low.
		 */
		cdreg = __raw_readb(cd + (CF_CD_PHYS & 0x3));
		iounmap(cd);
		if (cdreg & CF_CD_BIT) {
			dev_info(&pdev->dev, "no CompactFlash card present\n");
			return -ENODEV;
		}
	}

	host = ata_host_alloc(&pdev->dev, 1);
	if (!host)
		return -ENOMEM;
	ap = host->ports[0];
	ap->ops = &c2811_port_ops;
	/*
	 * PIO0: the real bus timing is fixed by the MV64340 device-bank
	 * parameters for DevCS3 and cannot be renegotiated, so there is nothing
	 * to gain from advertising a faster mode.
	 */
	ap->pio_mask = ATA_PIO0;
	/*
	 * No interrupt is wired to the slot, so the port must be told to poll.
	 * ata_host_activate() accepts irq 0, but without this flag libata still
	 * waits on an interrupt that never arrives and every command times out.
	 */
	ap->flags |= ATA_FLAG_PIO_POLLING;
	c2811_port_ops.sff_irq_clear = NULL;

	base = devm_ioremap(&pdev->dev, C2811_CF_BASE, CF_WINDOW_LEN);
	if (!base)
		return -ENOMEM;

	io = &ap->ioaddr;
	io->cmd_addr		= base;
	io->data_addr		= base + CF_DATA16;
	io->error_addr		= base + CF_ERROR;
	io->feature_addr	= base + CF_FEATURE;
	io->nsect_addr		= base + CF_NSECT;
	io->lbal_addr		= base + CF_LBAL;
	io->lbam_addr		= base + CF_LBAM;
	io->lbah_addr		= base + CF_LBAH;
	io->device_addr		= base + CF_DEVICE;
	io->status_addr		= base + CF_STATUS;
	io->command_addr	= base + CF_COMMAND;
	io->altstatus_addr	= base + CF_ALTSTATUS;
	io->ctl_addr		= base + CF_CTL;

	ata_port_desc(ap, "cmd 0x%lx ctl 0x%lx (polled)",
		      (unsigned long)C2811_CF_BASE,
		      (unsigned long)C2811_CF_BASE + CF_CTL);

	/* irq 0 + NULL handler: no interrupt is wired, so libata polls. */
	return ata_host_activate(host, 0, NULL, 0, &c2811_sht);
}

static struct platform_driver c2811_pata_driver = {
	.probe	= c2811_pata_probe,
	.driver	= {
		.name = DRV_NAME,
	},
};

static struct resource c2811_cf_resources[] = {
	{
		.start	= C2811_CF_BASE,
		.end	= C2811_CF_BASE + CF_WINDOW_LEN - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device c2811_cf_device = {
	.name		= DRV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(c2811_cf_resources),
	.resource	= c2811_cf_resources,
};

static int __init c2811_pata_init(void)
{
	int ret;

	ret = platform_device_register(&c2811_cf_device);
	if (ret)
		return ret;
	return platform_driver_register(&c2811_pata_driver);
}
device_initcall(c2811_pata_init);
