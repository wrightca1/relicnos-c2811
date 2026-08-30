// SPDX-License-Identifier: GPL-2.0
/*
 * Cisco NM-32A 32-port async network module.
 *
 * Eight Cirrus CD2481 quad UARTs behind a Xilinx XC4013E PCI interface.  The
 * whole reverse-engineering story is in DYNAMIC_ANALYSIS.md; the parts that
 * matter for this driver:
 *
 *  - The CD2481 has no protocol code in ROM.  It is inert until the host
 *    downloads 8192 18-bit microcode words through AIRl/AIRm/AIRh with
 *    MTCR/BTCR strobes (sec 91, sec 95).  The image is embedded here.
 *
 *  - Register addresses are the datasheet's MOTOROLA column: IOS's own
 *    download routine writes F2/F3/F0/F1/F6, which is that mapping (sec 91).
 *
 *  - The eight chips sit at fixed offsets in BAR0, taken from IOS's own table
 *    at 0x46581470 rather than guessed (sec 92).
 *
 *  - Data does NOT move through the transmit FIFO.  IOS drives the parts
 *    purely by bus-mastering into host memory through the A/B DMA buffer
 *    descriptors (sec 97).  TDR/RDR are not this card's data path.
 *
 *  - The card can only reach the first 8 MB of DRAM: the MV96340 enables only
 *    chip-select window CS_0, mapping PCI 0-8 MB onto physical 0-8 MB.  A
 *    descriptor pointing anywhere else master-aborts and hangs the board with
 *    no oops and no console (sec 99).  So the buffers come from the region
 *    plat_mem_setup() reserved at C2811_DMA_BASE, addressed through the
 *    uncached KSEG1 alias; bus address == physical address inside that window.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <asm/addrspace.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/serial.h>
#include "cisco2811.h"
#include "cd2481_ucode.h"

#define DRV "nm32a"

/* microcode download (Motorola mapping) */
#define AIRL	0xF2
#define AIRM	0xF3
#define AIRH	0xF0
#define MTCR	0xF1
#define BTCR	0xF6
/* global */
#define GFRCR	0x81
#define CAR	0xEE
#define TPR	0xDA
/* per channel */
#define CMR	0x1B
#define COR1	0x10
#define COR2	0x17
#define COR3	0x16
#define COR4	0x15
#define COR5	0x14
#define CCR	0x13
#define CSR	0x1A
#define LIVR	0x09
#define IER	0x11
#define TCOR	0xC0
#define TBPR	0xC3
#define RCOR	0xC8
#define RBPR	0xCB
#define DMR	0xF6
/* DMA descriptors */
#define ARBADRU	0x40
#define ARBADRL	0x42
#define ARBCNT	0x4A
#define ARBSTS	0x4F
/* interrupt service */
#define RPILR	0xE1
#define TPILR	0xE3
#define MPILR	0xE4
#define TISR	0x8A
#define TFTC	0x80
#define RFOC	0x30
#define RIR	0xED
#define RDR	0xF8
#define TDR	0xF8
#define RISRl	0x89
#define MSVR_RTS 0xDE
#define MSVR_DTR 0xDF
#define TIR	0xEC
#define REOIR	0x84
#define TEOIR	0x85
#define ATBADRU	0x50
#define ATBADRL	0x52
#define ATBCNT	0x5A
#define ATBSTS	0x5F

/*
 * Bit-rate settings, from the measurement in §110 rather than from §103's
 * arithmetic.  Timing one character on the wire gives CLK = 35.08 MHz -- the
 * datasheet's nominal 35 MHz.  §103 had derived ~10 MHz by pairing IOS's
 * retained TBPR with a TCOR that was a reset default.
 *
 * 9600 is therefore NOT reachable on clk0: CLK/8/9600 - 1 = 455, past eight
 * bits.  It needs clk1 (CLK/32):  1093750 / (0x71 + 1) = 9679 baud, 0.8% off.
 */
/*
 * ClkSel sits in DIFFERENT bits in the two clock option registers, which is a
 * trap the datasheet lays out plainly and this driver walked into:
 *
 *   TCOR:  bit7 ClkSel2 | bit6 ClkSel1 | bit5 ClkSel0 | bit3 Ext-1X | bit1 LLM
 *   RCOR:  bit7 TLVal (ro) | bit5 DpllEn | bit4:3 Dpllmd | bit2:0 ClkSel
 *
 * Writing the same value to both -- TCOR=0x01, RCOR=0x01 for "clk1" -- gives
 * RCOR clk1 but leaves TCOR at clk0, because 0x01 lands on a reserved bit
 * there.  The result was transmitting at ~38400 while receiving at ~9600: a
 * 4x mismatch, and 38400 into a 9600 console.  That is precisely the garbage
 * seen in §109-§110.
 */
#define TCOR_CLK(sel)	((sel) << 5)	/* ClkSel is bits 7:5 here */
#define RCOR_CLK(sel)	(sel)		/* ...and bits 2:0 here */
#define TCOR_LLM	0x02		/* local loopback */

#define CLKSEL_9600	0x01		/* clk1 = CLK/32 */
#define BPR_9600	0x71

#define OWN	0x01		/* 2481own */
#define D_INTR	0x02
#define D_EOF	0x40

#define CCR_INITCH	0x20
#define CCR_RSTALL	0x10
#define CCR_ENTX	0x08
#define CCR_ENRX	0x02

/*
 * Card-level registers in BAR0, outside the per-chip windows.
 *
 * IOS writes 0x37BB to +0x812 immediately after downloading microcode to all
 * eight chips (0x4035dcd4: li s2,14267; sh s2,2066(s4)).  Nothing else in the
 * driver touches it, and we had never replicated it -- which matters, because
 * the FPGA sits between the CD2481s and the PCI bus and this is the only
 * card-level write in the whole bring-up path.
 */
/*
 * Reading offset 0 is the interrupt-acknowledge cycle.  The FPGA turns that
 * access into IACKIN* for the chip that is requesting service: TIR goes
 * 0xC8 -> 0x48, i.e. Ten clears, which the datasheet defines as happening
 * "during a valid interrupt acknowledge cycle".  It does not hand the vector
 * back, which is why the §104 scan -- which looked for LIVR in the returned
 * data -- read this very address and discarded it.
 */
#define NM32A_ACK	0x000

#define NM32A_PORTS	0x800		/* IOS reads this for 16 vs 32 ports */
#define NM32A_CTL	0x812
#define NM32A_CTL_VAL	0x37BB

#define NM32A_CHIPS	8
static const unsigned int chip_off[NM32A_CHIPS] = {
	0x400, 0x500, 0x600, 0x700, 0x900, 0xA00, 0xB00, 0xC00
};

#define NM32A_PORTS_N	32
#define NM32A_TXBUF	1024

struct nm32a;

struct nm32a_port {
	struct tty_port	port;
	struct nm32a	*card;
	unsigned	chip, chan;
	bool		open;
	u8		tx[NM32A_TXBUF];
	unsigned	head, tail;		/* tx ring */
	spinlock_t	lock;
};

struct nm32a {
	void __iomem	*bar;
	void __iomem	*dma;		/* uncached alias of the reserved region */
	phys_addr_t	dma_phys;
	struct pci_dev	*pdev;

	struct tty_driver	*tty;
	struct nm32a_port	ports[NM32A_PORTS_N];
	struct task_struct	*poller;
	spinlock_t		hw_lock;	/* serialises CAR + service */
};

/*
 * Write a chip register.
 *
 * Deliberately NO read-back.  An earlier version read the register straight
 * after writing it "to push it out", which is harmless for ordinary registers
 * and actively wrong for the data register: TDR and RDR share address 0xF8, so
 * every transmitted character was immediately followed by a read of the
 * RECEIVE data register.  That disturbed the receive FIFO and put a spurious
 * byte after every character -- visible as the far end echoing "a^@d^@m^@i^@n"
 * when "admin" was sent, which is what stopped logins from working.
 *
 * PCI writes to the same device stay ordered, so nothing needs flushing here;
 * where a flush is genuinely wanted, read a harmless register such as CAR.
 */
static inline void cwr(struct nm32a *p, unsigned chip, unsigned reg, u8 v)
{
	__raw_writeb(v, p->bar + chip_off[chip] + reg);
}

static inline u8 crd(struct nm32a *p, unsigned chip, unsigned reg)
{
	return __raw_readb(p->bar + chip_off[chip] + reg);
}

/* descriptor addresses are written as two 16-bit halves, never a word */
static void cwr16(struct nm32a *p, unsigned chip, unsigned reg, u16 v)
{
	cwr(p, chip, reg, v >> 8);
	cwr(p, chip, reg + 1, v & 0xff);
}

static int ccr_wait(struct nm32a *p, unsigned chip)
{
	int n;

	for (n = 0; n < 200000; n++)
		if (crd(p, chip, CCR) == 0)
			return 0;
	return -ETIMEDOUT;
}

/* the i'th 18-bit instruction of the packed little-endian stream */
static u32 ucode_insn(unsigned int i)
{
	unsigned int bit = i * 18, byte = bit >> 3;
	u32 v = 0;
	int k;

	for (k = 0; k < 3; k++)
		if (byte + k < sizeof(cd2481_ucode))
			v |= (u32)cd2481_ucode[byte + k] << (8 * k);
	return (v >> (bit & 7)) & 0x3FFFF;
}

static int nm32a_download(struct nm32a *p, unsigned chip)
{
	unsigned int i;
	u8 before, after;
	int n;

	before = crd(p, chip, GFRCR);

	/* dl_setup: park the MPU on "jump 0x3fff", then data-write mode */
	cwr(p, chip, AIRL, 0xff);
	cwr(p, chip, AIRM, 0x3f);
	cwr(p, chip, AIRH, 0x03);
	cwr(p, chip, MTCR, 0x01);
	cwr(p, chip, BTCR, 0x01);
	cwr(p, chip, MTCR, 0x02);

	for (i = 0; i < CD2481_UCODE_INSNS; i++) {
		u32 insn = ucode_insn(i);

		cwr(p, chip, AIRL, insn & 0xff);
		cwr(p, chip, AIRM, (insn >> 8) & 0xff);
		cwr(p, chip, AIRH, (insn >> 16) & 0x03);
		cwr(p, chip, BTCR, 0x03);	/* single step -> store */
	}

	/* dl_start */
	cwr(p, chip, AIRL, 0xff);
	cwr(p, chip, AIRM, 0x3f);
	cwr(p, chip, AIRH, 0x03);
	cwr(p, chip, MTCR, 0x01);
	cwr(p, chip, BTCR, 0x03);
	cwr(p, chip, BTCR, 0x03);
	cwr(p, chip, MTCR, 0x03);
	cwr(p, chip, BTCR, 0x00);

	/*
	 * Reset All, then wait for the firmware revision.  CCR self-clearing is
	 * the proof the MPU is executing: a part without microcode just leaves
	 * whatever was written sitting there.
	 */
	cwr(p, chip, CCR, CCR_RSTALL);
	n = ccr_wait(p, chip);
	for (i = 0; i < 200000; i++) {
		after = crd(p, chip, GFRCR);
		if (after && after != 0xff)
			break;
	}

	pr_info(DRV ": chip %u @ +0x%03x: GFRCR %02x -> %02x, CCR %s\n",
		chip, chip_off[chip], before, after,
		n ? "never cleared" : "self-cleared");

	return (n || !after || after == 0xff) ? -EIO : 0;
}

/*
 * Bring channel up and prove a byte moves, using internal loopback so the
 * result does not depend on the CLK feeding the bit-rate generators -- which
 * is not known: the board has an unmeasured crystal, and the datasheet's own
 * 19200 example does not fit its nominal 35 MHz.  In loopback both directions
 * derive from the same clock and divisor, so the byte round-trips regardless.
 */
static int nm32a_stage(struct nm32a *p, unsigned chip, unsigned chan, int stage)
{
	u8 __iomem *tx = p->dma;
	u8 __iomem *rx = p->dma + 0x1000;
	phys_addr_t txp = p->dma_phys, rxp = p->dma_phys + 0x1000;
	static const char msg[8] = "DMA-TEST";
	int i, n;
	u8 st;

	for (i = 0; i < 8; i++)
		__raw_writeb(msg[i], tx + i);
	for (i = 0; i < 16; i++)
		__raw_writeb(0, rx + i);

	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	cwr(p, chip, RCOR, 0x00);
	cwr(p, chip, RBPR, 0x81);
	cwr(p, chip, TCOR, 0x02);	/* bit 1 = local loopback */
	cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR,  0xC2);	/* RxMode=DMA | TxMode=DMA | async */
	cwr(p, chip, COR1, 0x17);	/* 8 bits, no parity */
	cwr(p, chip, COR2, 0x00);
	cwr(p, chip, COR3, 0x02);	/* 1 stop bit */
	cwr(p, chip, COR4, 0x08);
	cwr(p, chip, COR5, 0x00);
	cwr(p, chip, DMR,  0x00);

	cwr(p, chip, CCR, CCR_INITCH);
	if (ccr_wait(p, chip)) {
		pr_err(DRV ": chip %u chan %u: InitCh never completed\n", chip, chan);
		return -ETIMEDOUT;
	}
	pr_info(DRV ": stage 1 ok: InitCh accepted, CSR=%02x CMR=%02x\n",
		crd(p, chip, CSR), crd(p, chip, CMR));
	if (stage < 2)
		return 0;

	/*
	 * Arm both descriptors BEFORE enabling the channel.  Enabling a
	 * DMA-mode receiver with zeroed descriptor registers makes it master at
	 * a garbage address, which master-aborts and hangs the machine.
	 */
	cwr16(p, chip, ARBADRU, rxp >> 16);
	cwr16(p, chip, ARBADRL, rxp & 0xffff);
	cwr16(p, chip, ARBCNT, 256);
	cwr(p, chip, ARBSTS, OWN | D_INTR);
	pr_info(DRV ": stage 2 ok: rx descriptor armed, ARBSTS=%02x\n",
		crd(p, chip, ARBSTS));
	if (stage < 3)
		return 0;

	cwr16(p, chip, ATBADRU, txp >> 16);
	cwr16(p, chip, ATBADRL, txp & 0xffff);
	cwr16(p, chip, ATBCNT, 8);
	cwr(p, chip, ATBSTS, OWN | D_INTR | D_EOF);
	pr_info(DRV ": stage 3 ok: tx descriptor armed, ATBSTS=%02x\n",
		crd(p, chip, ATBSTS));
	if (stage < 4)
		return 0;

	pr_info(DRV ": stage 4: enabling channel -- card may now bus-master\n");
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	if (ccr_wait(p, chip)) {
		pr_err(DRV ": chip %u chan %u: EnTx|EnRx never completed\n", chip, chan);
		return -ETIMEDOUT;
	}
	cwr(p, chip, IER, 0x09);

	for (n = 0; n < 200000; n++) {
		st = crd(p, chip, ATBSTS);
		if (!(st & OWN))
			break;
		udelay(10);
	}
	pr_info(DRV ": chip %u chan %u: ATBSTS=%02x after %d polls%s\n",
		chip, chan, st, n, (st & 0x80) ? " (BUS ERROR)" : "");
	if (st & OWN) {
		pr_err(DRV ": transmit buffer never taken\n");
		return -EIO;
	}

	for (n = 0; n < 200000; n++) {
		if (__raw_readb(rx))
			break;
		udelay(10);
	}
	pr_info(DRV ": ARBSTS=%02x rx: %02x %02x %02x %02x\n",
		crd(p, chip, ARBSTS), __raw_readb(rx), __raw_readb(rx + 1),
		__raw_readb(rx + 2), __raw_readb(rx + 3));

	for (i = 0; i < 8; i++)
		if (__raw_readb(rx + i) != msg[i])
			return -EIO;

	pr_info(DRV ": *** LOOPBACK OK on chip %u chan %u -- the channel moves data ***\n",
		chip, chan);
	return 0;
}

/*
 * The card hangs the machine outright on a bad DMA setup -- master abort, no
 * oops, no console.  So the self-test does NOT run at probe: it is triggered
 * by writing a stage number to
 *
 *     /sys/module/nm32a/parameters/selftest
 *
 * 0 = dump registers, touching nothing   1 = download microcode
 * 2 = channel init                        3 = + arm receive descriptor
 * 4 = + arm transmit descriptor           5 = + enable channel (card masters)
 *
 * Probe deliberately does NOT download.  Reading the state a *working* driver
 * leaves behind means booting relicnos straight after IOS (BREAK into ROMMON,
 * no power cycle, so the CD2481s keep their state) and dumping with stage 0 --
 * a download at probe would wipe exactly what we came to read.
 *
 * One boot then covers several experiments, and when a stage does wedge the
 * board we power-cycle once and resume at the next one instead of paying a
 * full rebuild per attempt.
 */
static struct nm32a *nm32a_dev;

static int nm32a_stage(struct nm32a *p, unsigned chip, unsigned chan, int stage);
static void nm32a_dump(struct nm32a *p);
static void nm32a_find_iack(struct nm32a *p);
static void nm32a_io_probe(struct nm32a *p, int with_service);
static void nm32a_char_tx(struct nm32a *p, unsigned chan, int loopback);
static void nm32a_hunt_ack(struct nm32a *p, int do_writes);
static void nm32a_acked_tx(struct nm32a *p, unsigned chan, int loopback);
static void nm32a_tx_line(struct nm32a *p, unsigned chip, unsigned chan);
static void nm32a_talk(struct nm32a *p, unsigned chip, unsigned chan);
static void nm32a_baud_sweep(struct nm32a *p, unsigned chip, unsigned chan);
static void nm32a_echo_match(struct nm32a *p, unsigned chip, unsigned chan);
static void nm32a_measure_baud(struct nm32a *p, unsigned chip, unsigned chan);

static int nm32a_set_selftest(const char *val, const struct kernel_param *kp)
{
	int stage, rc;

	rc = kstrtoint(val, 0, &stage);
	if (rc)
		return rc;
	if (!nm32a_dev) {
		pr_err(DRV ": no card\n");
		return -ENODEV;
	}
	pr_info(DRV ": ---- selftest stage %d ----\n", stage);
	if (stage == 0) {
		nm32a_dump(nm32a_dev);
		return 0;
	}
	if (stage == 20) {
		nm32a_measure_baud(nm32a_dev, 4, 0);
		return 0;
	}
	if (stage == 19) {
		nm32a_echo_match(nm32a_dev, 4, 0);
		return 0;
	}
	if (stage == 18) {
		nm32a_baud_sweep(nm32a_dev, 4, 0);
		return 0;
	}
	if (stage == 17) {
		nm32a_talk(nm32a_dev, 4, 0);	/* port 16: the cabled one */
		return 0;
	}
	if (stage == 15 || stage == 16) {
		/* 15 = chip 0 chan 0; 16 = port 16, the cabled one */
		nm32a_tx_line(nm32a_dev, stage == 16 ? 4 : 0, 0);
		return 0;
	}
	if (stage == 13 || stage == 14) {
		nm32a_acked_tx(nm32a_dev, 0, stage == 13);
		return 0;
	}
	if (stage == 11 || stage == 12) {
		nm32a_hunt_ack(nm32a_dev, stage == 12);
		return 0;
	}
	if (stage == 9 || stage == 10) {
		nm32a_char_tx(nm32a_dev, 0, stage == 9);
		return 0;
	}
	if (stage == 7 || stage == 8) {
		nm32a_io_probe(nm32a_dev, stage == 8);
		return 0;
	}
	if (stage == 6) {
		nm32a_find_iack(nm32a_dev);
		return 0;
	}
	if (stage == 1) {
		unsigned c;

		/*
		 * All eight, not just chip 0.  A chip without microcode never
		 * completes InitCh, so anything targeting chips 1-7 -- such as
		 * port 16 on chip 4 -- silently did nothing.
		 */
		for (c = 0; c < NM32A_CHIPS; c++) {
			rc = nm32a_download(nm32a_dev, c);
			if (rc)
				pr_err(DRV ": chip %u download FAILED\n", c);
		}
		pr_info(DRV ": ---- all chips downloaded ----\n");
		return 0;
	}
	rc = nm32a_stage(nm32a_dev, 0, 0, stage - 1);
	pr_info(DRV ": ---- stage %d returned %d ----\n", stage, rc);
	return 0;
}

static const struct kernel_param_ops nm32a_selftest_ops = {
	.set = nm32a_set_selftest,
};
module_param_cb(selftest, &nm32a_selftest_ops, NULL, 0200);
MODULE_PARM_DESC(selftest, "run DMA bring-up stage 1..4 (see source)");

static void nm32a_dump(struct nm32a *p)
{
	unsigned chip, chan;

	pr_info(DRV ": card: +0x800=%02x%02x +0x812=%02x%02x\n",
		__raw_readb(p->bar + NM32A_PORTS),
		__raw_readb(p->bar + NM32A_PORTS + 1),
		__raw_readb(p->bar + NM32A_CTL),
		__raw_readb(p->bar + NM32A_CTL + 1));

	for (chip = 0; chip < NM32A_CHIPS; chip++) {
		pr_info(DRV ": chip %u @+0x%03x GFRCR=%02x\n",
			chip, chip_off[chip], crd(p, chip, GFRCR));
		for (chan = 0; chan < 4; chan++) {
			cwr(p, chip, CAR, chan);
			pr_info(DRV ":   ch%u CMR=%02x CSR=%02x IER=%02x "
				"TBPR=%02x TCOR=%02x RBPR=%02x RCOR=%02x\n",
				chan, crd(p, chip, CMR), crd(p, chip, CSR),
				crd(p, chip, IER), crd(p, chip, TBPR),
				crd(p, chip, TCOR), crd(p, chip, RBPR),
				crd(p, chip, RCOR));
			pr_info(DRV ":   ch%u ARB %02x%02x:%02x%02x cnt %02x%02x sts %02x | "
				"ATB %02x%02x:%02x%02x cnt %02x%02x sts %02x\n", chan,
				crd(p, chip, ARBADRU), crd(p, chip, ARBADRU+1),
				crd(p, chip, ARBADRL), crd(p, chip, ARBADRL+1),
				crd(p, chip, ARBCNT), crd(p, chip, ARBCNT+1),
				crd(p, chip, ARBSTS),
				crd(p, chip, ATBADRU), crd(p, chip, ATBADRU+1),
				crd(p, chip, ATBADRL), crd(p, chip, ATBADRL+1),
				crd(p, chip, ATBCNT), crd(p, chip, ATBCNT+1),
				crd(p, chip, ATBSTS));
		}
	}
}

/*
 * Hunt for the address the FPGA decodes as an interrupt-acknowledge cycle.
 *
 * §103 settled that async is the character path, and §96 showed TDR/RDR only
 * open inside an interrupt service window entered by a hardware IACK on
 * IACKIN*.  During that cycle the CD2481 drives a vector built from LIVR, so
 * LIVR is the detector: set it to something distinctive, make a service
 * pending, and whichever BAR read hands that value back is the IACK port.
 *
 * Safe to run, unlike the DMA stages: CMR = 0x02 is interrupt mode, so
 * enabling the channel makes the chip request service without ever mastering
 * the bus.
 */
#define IACK_LIVR	0x5C

static void nm32a_find_iack(struct nm32a *p)
{
	unsigned chip = 0, a, hits = 0;
	struct pci_dev *pdev = p->pdev;
	int n;
	u8 v;

	/* the chip rejects an IACK whose A[6:0] does not match a PILR */
	cwr(p, chip, RPILR, 0x02);
	cwr(p, chip, TPILR, 0x04);
	cwr(p, chip, MPILR, 0x06);

	cwr(p, chip, CAR, 0);
	cwr(p, chip, LIVR, IACK_LIVR);
	cwr(p, chip, RCOR, 0x00); cwr(p, chip, RBPR, 0x81);
	cwr(p, chip, TCOR, 0x00); cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR,  0x02);		/* async, INTERRUPT mode */
	cwr(p, chip, COR1, 0x17);
	cwr(p, chip, COR2, 0x00);
	cwr(p, chip, COR3, 0x02);
	cwr(p, chip, COR4, 0x08);
	cwr(p, chip, COR5, 0x00);
	cwr(p, chip, CCR, CCR_INITCH);
	ccr_wait(p, chip);
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	ccr_wait(p, chip);
	cwr(p, chip, IER, 0x09);

	for (n = 0; n < 200000; n++)
		if (crd(p, chip, TIR) & 0x80)
			break;
	pr_info(DRV ": TIR=%02x TISR=%02x LIVR=%02x -- scanning BAR for the vector\n",
		crd(p, chip, TIR), crd(p, chip, TISR), crd(p, chip, LIVR));

	/* the whole 1 MB BAR, not just the first 64 KB */
	for (a = 0; a < 0x100000; a++) {
		/* skip the eight chip windows; their own registers are not it */
		if ((a & ~0xff) == 0x400 || (a & ~0xff) == 0x500 ||
		    (a & ~0xff) == 0x600 || (a & ~0xff) == 0x700 ||
		    (a & ~0xff) == 0x900 || (a & ~0xff) == 0xA00 ||
		    (a & ~0xff) == 0xB00 || (a & ~0xff) == 0xC00)
			continue;
		v = __raw_readb(p->bar + a);
		if ((v & 0xFC) == IACK_LIVR) {
			u8 tir = crd(p, chip, TIR), tftc = crd(p, chip, TFTC);

			pr_info(DRV ": +0x%05x -> %02x  (TIR=%02x TFTC=%02x)\n",
				a, v, tir, tftc);
			if (++hits >= 12) {
				pr_info(DRV ": (stopping after 12)\n");
				break;
			}
		}
	}
	if (!hits)
		pr_info(DRV ": no byte read in 1 MB returned the LIVR vector\n");

	/* the vector may come back as a halfword rather than a byte */
	for (a = 0; a < 0x100000 && hits < 12; a += 2) {
		u16 h;

		if ((a & ~0xff) >= 0x400 && (a & ~0xff) <= 0xC00)
			continue;
		h = __raw_readw(p->bar + a);
		if ((h & 0xFC) == IACK_LIVR || ((h >> 8) & 0xFC) == IACK_LIVR) {
			pr_info(DRV ": halfword +0x%05x -> %04x (TIR=%02x)\n",
				a, h, crd(p, chip, TIR));
			hits++;
		}
	}
	pr_info(DRV ": scan complete, %u candidate(s) in memory space\n", hits);

	/*
	 * The I/O BAR.  This is the part of the card we could never reach until
	 * the host bridge got an I/O window: 16 bytes, exactly the shape of a
	 * vector/acknowledge port, and the acknowledge cycle is not in memory
	 * space (§104).
	 */
	if (pci_resource_len(pdev, 1)) {
		unsigned long io = pci_resource_start(pdev, 1);
		int i;

		pr_info(DRV ": I/O BAR at 0x%lx len %llu -- reading it\n",
			io, (unsigned long long)pci_resource_len(pdev, 1));
		for (i = 0; i < 16; i++) {
			u8 v = inb(io + i);

			pr_info(DRV ":   io+%d = %02x  %s (TIR=%02x TFTC=%02x)\n",
				i, v,
				(v & 0xFC) == IACK_LIVR ? "*** LIVR VECTOR ***" : "",
				crd(p, chip, TIR), crd(p, chip, TFTC));
		}
	} else {
		pr_info(DRV ": no I/O BAR assigned -- host bridge has no I/O window\n");
	}
}

/*
 * Read the card's I/O BAR and find out why it hangs.
 *
 * §106: the first inb() on the newly-assigned BAR wedges the machine, while an
 * unclaimed I/O port reads 0xff harmlessly.  Two explanations fit: an
 * interrupt-acknowledge handshake that never completes (IACKIN* asserted with
 * nothing to drive IACKOUT*), or an I/O cycle that is simply not routed.
 *
 * They are separable.  With every interrupt source disabled there is nothing
 * for the card to acknowledge, so an IACK-shaped hang should not happen; a
 * routing failure will hang regardless.
 *
 *   stage 7 = quiet card, nothing pending   stage 8 = service pending
 *
 * Each step announces itself and drains the console first, because the board
 * dies without an oops and the last line out is the diagnosis.
 */
static void nm32a_io_probe(struct nm32a *p, int with_service)
{
	unsigned long io = pci_resource_start(p->pdev, 1);
	unsigned chip, chan;
	u8 v;

	if (!pci_resource_len(p->pdev, 1)) {
		pr_info(DRV ": no I/O BAR assigned\n");
		return;
	}

	/* silence every channel on every chip: no IER, no enables */
	for (chip = 0; chip < NM32A_CHIPS; chip++)
		for (chan = 0; chan < 4; chan++) {
			cwr(p, chip, CAR, chan);
			cwr(p, chip, IER, 0x00);
		}

	if (with_service) {
		cwr(p, chip = 0, CAR, 0);
		cwr(p, 0, LIVR, IACK_LIVR);
		cwr(p, 0, CMR, 0x02);
		cwr(p, 0, COR1, 0x17);
		cwr(p, 0, COR3, 0x02);
		cwr(p, 0, CCR, CCR_INITCH);
		ccr_wait(p, 0);
		cwr(p, 0, CCR, CCR_ENTX | CCR_ENRX);
		ccr_wait(p, 0);
		cwr(p, 0, IER, 0x09);
		pr_info(DRV ": service armed, TIR=%02x\n", crd(p, 0, TIR));
	} else {
		pr_info(DRV ": all interrupts disabled, TIR=%02x (nothing to ack)\n",
			crd(p, 0, TIR));
	}

	pr_info(DRV ": about to inb(0x%lx) -- if this is the last line, that read "
		"never returned\n", io);
	mdelay(400);

	v = inb(io);

	pr_info(DRV ": *** inb(0x%lx) returned %02x -- the I/O BAR is READABLE ***\n",
		io, v);
	mdelay(200);
	if ((v & 0xFC) == IACK_LIVR)
		pr_info(DRV ": that is the LIVR vector -- this is the IACK port\n");
}

/*
 * The character path, retried under conditions that were never actually met.
 *
 * §96 concluded TDR was unreachable because writes did not move TFTC and TEOIR
 * did not clear Tact.  Both observations predate §103, which showed IER must be
 * set before the chip will enter a service at all -- every earlier TDR write
 * was made with IER = 0.  And TFTC is a per-service allowance, so it need not
 * decrement per write; the meaningful signal is whether TEOIR clears Tact.
 *
 * So: microcode loaded, CMR = 0x02 (async, interrupt), IER = 0x09, wait for a
 * genuine transmit service, then write TDR, write TEOIR, and report everything.
 *
 *   stage 9 = internal loopback (TCOR bit 1)    stage 10 = out the real port
 */
static void nm32a_char_tx(struct nm32a *p, unsigned chan, int loopback)
{
	static const u8 msg[4] = { 'A', 'B', 'C', 'D' };
	unsigned chip = 0;
	int n, i;
	u8 tir0, tir1, tftc0, tftc1;

	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	cwr(p, chip, RCOR, 0x00);
	cwr(p, chip, RBPR, 0x81);		/* 9600 at the ~10 MHz CLK (§103) */
	cwr(p, chip, TCOR, loopback ? TCOR_LLM : 0x00);
	cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR,  0x02);		/* async, interrupt -- never DMA */
	cwr(p, chip, COR1, 0x17);
	cwr(p, chip, COR2, 0x00);
	cwr(p, chip, COR3, 0x02);
	cwr(p, chip, COR4, 0x08);
	cwr(p, chip, COR5, 0x00);

	cwr(p, chip, CCR, CCR_INITCH);
	if (ccr_wait(p, chip)) { pr_err(DRV ": InitCh stuck\n"); return; }
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	if (ccr_wait(p, chip)) { pr_err(DRV ": EnTx|EnRx stuck\n"); return; }

	cwr(p, chip, IER, 0x09);		/* THE bit that was missing before */
	pr_info(DRV ": chan %u loopback=%d CSR=%02x IER=%02x\n",
		chan, loopback, crd(p, chip, CSR), crd(p, chip, IER));

	for (n = 0; n < 500000; n++)
		if (crd(p, chip, TIR) & 0x80)
			break;
	tir0 = crd(p, chip, TIR);
	tftc0 = crd(p, chip, TFTC);
	pr_info(DRV ": tx service: TIR=%02x TFTC=%02x TISR=%02x (%d polls)\n",
		tir0, tftc0, crd(p, chip, TISR), n);
	if (!(tir0 & 0x80)) { pr_err(DRV ": no service ever offered\n"); return; }

	for (i = 0; i < 4; i++)
		cwr(p, chip, TDR, msg[i]);
	tftc1 = crd(p, chip, TFTC);
	cwr(p, chip, TEOIR, 0x00);		/* Notrans clear: data was transferred */
	tir1 = crd(p, chip, TIR);

	pr_info(DRV ": wrote 4 bytes; TFTC %02x->%02x, TIR %02x->%02x  %s\n",
		tftc0, tftc1, tir0, tir1,
		(tir1 & 0x40) ? "Tact STILL SET -- service not accepted"
			      : "*** Tact CLEARED -- the service completed ***");

	/* did anything come back? in loopback it should */
	for (n = 0; n < 500000; n++)
		if ((crd(p, chip, RIR) & 0x80) || crd(p, chip, RFOC))
			break;
	pr_info(DRV ": rx: RIR=%02x RFOC=%02x RISRl=%02x CSR=%02x\n",
		crd(p, chip, RIR), crd(p, chip, RFOC),
		crd(p, chip, RISRl), crd(p, chip, CSR));
	if (crd(p, chip, RFOC)) {
		for (i = 0; i < 4; i++)
			pr_info(DRV ":   RDR -> %02x\n", crd(p, chip, RDR));
		cwr(p, chip, REOIR, 0x00);
	}
}

/*
 * Hunt for the acknowledge cycle by BEHAVIOUR rather than by returned data.
 *
 * The §104 scan looked for the LIVR vector coming back from a read, and found
 * nothing anywhere in 1 MB.  That detector assumes the FPGA hands the vector to
 * the host; it may well consume it itself, in which case the scan could never
 * have worked no matter where the port is.
 *
 * The datasheet gives a detector that does not care: Ten "is cleared during a
 * valid transmit interrupt acknowledge cycle".  So arm a transmit service, walk
 * the BAR, and after each access ask the chip whether it now considers the
 * interrupt acknowledged.  Reads first; stage 12 repeats with writes, since an
 * acknowledge may well be triggered by a write strobe rather than a read.
 */
static void nm32a_hunt_ack(struct nm32a *p, int do_writes)
{
	unsigned chip = 0, a;
	int n;
	u8 tir;

	cwr(p, chip, CAR, 0);
	cwr(p, chip, LIVR, IACK_LIVR);
	cwr(p, chip, RCOR, 0x00); cwr(p, chip, RBPR, 0x81);
	cwr(p, chip, TCOR, 0x00); cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR, 0x02);
	cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
	cwr(p, chip, CCR, CCR_INITCH); ccr_wait(p, chip);
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX); ccr_wait(p, chip);
	cwr(p, chip, IER, 0x09);
	for (n = 0; n < 200000; n++)
		if (crd(p, chip, TIR) & 0x80)
			break;
	tir = crd(p, chip, TIR);
	pr_info(DRV ": armed: TIR=%02x -- hunting for an access that clears Ten "
		"(%s)\n", tir, do_writes ? "writes" : "reads");
	if (!(tir & 0x80)) { pr_err(DRV ": no service to acknowledge\n"); return; }

	for (a = 0; a < 0x100000; a++) {
		if ((a & ~0xff) >= 0x400 && (a & ~0xff) <= 0xC00)
			continue;			/* the chip windows themselves */
		if (do_writes)
			__raw_writeb(0x00, p->bar + a);
		else
			(void)__raw_readb(p->bar + a);

		if (!(crd(p, chip, TIR) & 0x80)) {
			pr_info(DRV ": *** +0x%05x (%s) CLEARED Ten -- TIR=%02x "
				"TFTC=%02x ***\n", a, do_writes ? "write" : "read",
				crd(p, chip, TIR), crd(p, chip, TFTC));
			pr_info(DRV ": this is the acknowledge cycle\n");
			return;
		}
	}
	pr_info(DRV ": no %s in 1 MB cleared Ten\n", do_writes ? "write" : "read");
}

/*
 * Transmit through the character path, acknowledging first.
 *
 * This is what every earlier attempt was missing.  The CD2481 only opens
 * TDR/TEOIR inside an interrupt service, and the service is entered by the
 * acknowledge cycle -- a read of BAR offset 0 -- not by the host noticing Ten
 * in TIR.  Sequence: arm, acknowledge, fill the FIFO, end the interrupt.
 */
static void nm32a_acked_tx(struct nm32a *p, unsigned chan, int loopback)
{
	static const u8 msg[4] = { 'A', 'B', 'C', 'D' };
	unsigned chip = 0;
	int n, i;
	u8 tir_armed, tir_acked, tir_done, tftc_before, tftc_after, ack;

	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	cwr(p, chip, RCOR, 0x00); cwr(p, chip, RBPR, 0x81);
	cwr(p, chip, TCOR, loopback ? TCOR_LLM : 0x00); cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR,  0x02);
	cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
	cwr(p, chip, COR4, 0x08); cwr(p, chip, COR5, 0x00);
	cwr(p, chip, CCR, CCR_INITCH);  if (ccr_wait(p, chip)) return;
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX); if (ccr_wait(p, chip)) return;
	cwr(p, chip, IER, 0x09);

	for (n = 0; n < 500000; n++)
		if (crd(p, chip, TIR) & 0x80)
			break;
	tir_armed = crd(p, chip, TIR);
	if (!(tir_armed & 0x80)) { pr_err(DRV ": no service offered\n"); return; }

	/* THE acknowledge */
	ack = __raw_readb(p->bar + NM32A_ACK);
	tir_acked = crd(p, chip, TIR);
	tftc_before = crd(p, chip, TFTC);
	pr_info(DRV ": armed TIR=%02x -> ack read returned %02x -> TIR=%02x TFTC=%02x\n",
		tir_armed, ack, tir_acked, tftc_before);

	for (i = 0; i < 4; i++)
		cwr(p, chip, TDR, msg[i]);
	tftc_after = crd(p, chip, TFTC);
	cwr(p, chip, TEOIR, 0x00);
	tir_done = crd(p, chip, TIR);

	pr_info(DRV ": wrote ABCD: TFTC %02x->%02x, TIR %02x->%02x  %s\n",
		tftc_before, tftc_after, tir_acked, tir_done,
		(tir_done & 0x40) ? "Tact still set"
				  : "*** Tact CLEARED -- TRANSMIT ACCEPTED ***");

	for (n = 0; n < 1000000; n++)
		if ((crd(p, chip, RIR) & 0x80) || crd(p, chip, RFOC))
			break;
	pr_info(DRV ": rx: RIR=%02x RFOC=%02x RISRl=%02x\n",
		crd(p, chip, RIR), crd(p, chip, RFOC), crd(p, chip, RISRl));
	if (crd(p, chip, RIR) & 0x80) {
		u8 rack = __raw_readb(p->bar + NM32A_ACK);	/* acknowledge the rx */
		u8 cnt = crd(p, chip, RFOC);

		pr_info(DRV ": rx ack returned %02x, RFOC=%02x\n", rack, cnt);
		for (i = 0; i < cnt && i < 8; i++)
			pr_info(DRV ":   RDR -> %02x '%c'\n",
				crd(p, chip, RDR), crd(p, chip, RDR));
		cwr(p, chip, REOIR, 0x00);
	}
}

/*
 * Is anything actually coming out of the transmitter?
 *
 * RCOR bit 7 is TLVal -- "the logical value of the transmit data pin",
 * read-only.  That is a direct view of the wire, independent of the receiver,
 * and it explains IOS's RCOR=0x80 in §103: not configuration, just the line
 * sitting idle-high.
 *
 * Idle is mark (1).  Any character contains a start bit, so a transmitter that
 * is running must pull the line low.  Sampling TLVal across a burst therefore
 * answers "do bytes leave the chip?" without needing RX to work.
 */
static void nm32a_tx_line(struct nm32a *p, unsigned chip, unsigned chan)
{
	unsigned i, zeros = 0, edges = 0, last = 1;
	int n;
	u8 tir;

	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	cwr(p, chip, RCOR, 0x00); cwr(p, chip, RBPR, 0x81);
	cwr(p, chip, TCOR, 0x00); cwr(p, chip, TBPR, 0x81);
	cwr(p, chip, CMR,  0x02);
	cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
	cwr(p, chip, COR4, 0x08); cwr(p, chip, COR5, 0x00);
	cwr(p, chip, CCR, CCR_INITCH);
	if (ccr_wait(p, chip)) {
		pr_err(DRV ": chip %u chan %u: InitCh stuck (GFRCR=%02x -- "
		       "microcode loaded?)\n", chip, chan, crd(p, chip, GFRCR));
		return;
	}
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	if (ccr_wait(p, chip)) {
		pr_err(DRV ": chip %u chan %u: EnTx|EnRx stuck\n", chip, chan);
		return;
	}
	cwr(p, chip, MSVR_RTS, 0x01);
	cwr(p, chip, MSVR_DTR, 0x02);
	cwr(p, chip, IER, 0x09);

	pr_info(DRV ": chip %u chan %u: TLVal idle = %u (1 = mark, as expected)\n",
		chip, chan, !!(crd(p, chip, RCOR) & 0x80));

	/* keep the transmitter fed for a while, sampling the line throughout */
	for (n = 0; n < 40; n++) {
		int k;

		for (k = 0; k < 20000 && !(crd(p, chip, TIR) & 0x80); k++)
			;
		tir = crd(p, chip, TIR);
		if (!(tir & 0x80))
			break;
		__raw_readb(p->bar + NM32A_ACK);	/* acknowledge */
		for (i = 0; i < 8; i++)
			cwr(p, chip, TDR, 0x55);	/* 0x55 = maximum edges */
		cwr(p, chip, TEOIR, 0x00);

		for (i = 0; i < 3000; i++) {
			unsigned v = !!(crd(p, chip, RCOR) & 0x80);

			if (!v)
				zeros++;
			if (v != last)
				edges++;
			last = v;
		}
	}
	pr_info(DRV ": after %d bursts: TLVal low %u times, %u edges, TIR=%02x CSR=%02x\n",
		n, zeros, edges, crd(p, chip, TIR), crd(p, chip, CSR));
	pr_info(DRV ": %s\n", edges ?
		"*** THE TRANSMIT LINE IS MOVING -- data is leaving the chip ***" :
		"line never left idle -- nothing is being transmitted");
}

/*
 * Talk to whatever is on the other end of the cable.
 *
 * Port 16 (chip 4, channel 0) goes to an Arista switch console, which is an
 * active talker rather than a loopback plug: send it a carriage return and it
 * should answer with a prompt.  That exercises transmit and receive together
 * against real equipment, which is a far better test than internal loopback.
 *
 * Receive has the same shape as transmit: wait for Ren in RIR, perform the
 * acknowledge (read BAR+0), read RFOC for the count, read that many bytes from
 * RDR, then write REOIR to end the service.
 */
static void nm32a_talk(struct nm32a *p, unsigned chip, unsigned chan)
{
	u8 buf[256];
	unsigned got = 0;
	int round, n, i;

	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	cwr(p, chip, RCOR, RCOR_CLK(CLKSEL_9600)); cwr(p, chip, RBPR, BPR_9600);
	cwr(p, chip, TCOR, TCOR_CLK(CLKSEL_9600)); cwr(p, chip, TBPR, BPR_9600);
	cwr(p, chip, CMR,  0x02);
	cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
	cwr(p, chip, COR4, 0x01);		/* interrupt on every character */
	cwr(p, chip, COR5, 0x00);
	cwr(p, chip, CCR, CCR_INITCH);
	if (ccr_wait(p, chip)) {
		pr_err(DRV ": InitCh stuck (GFRCR=%02x)\n", crd(p, chip, GFRCR));
		return;
	}
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	if (ccr_wait(p, chip)) { pr_err(DRV ": enable stuck\n"); return; }
	cwr(p, chip, MSVR_RTS, 0x01);
	cwr(p, chip, MSVR_DTR, 0x02);
	cwr(p, chip, IER, 0x09);
	pr_info(DRV ": port16 up: CSR=%02x -- sending CR, listening for a reply\n",
		crd(p, chip, CSR));

	for (round = 0; round < 60 && got < sizeof(buf) - 8; round++) {
		/* every few rounds, prod the far end with a carriage return */
		if ((round % 20) == 0) {
			for (n = 0; n < 100000; n++)
				if (crd(p, chip, TIR) & 0x80)
					break;
			if (crd(p, chip, TIR) & 0x80) {
				__raw_readb(p->bar + NM32A_ACK);
				cwr(p, chip, TDR, '\r');
				cwr(p, chip, TDR, '\n');
				cwr(p, chip, TEOIR, 0x00);
			}
		}

		/* drain anything the far end has sent */
		for (n = 0; n < 300000; n++) {
			if (!(crd(p, chip, RIR) & 0x80))
				continue;
			__raw_readb(p->bar + NM32A_ACK);	/* acknowledge */
			i = crd(p, chip, RFOC);
			if (i > (int)(sizeof(buf) - got))
				i = sizeof(buf) - got;
			while (i-- > 0 && got < sizeof(buf))
				buf[got++] = crd(p, chip, RDR);
			cwr(p, chip, REOIR, 0x00);
			break;
		}
		mdelay(20);
	}

	pr_info(DRV ": received %u byte(s) from the far end\n", got);
	if (got) {
		char line[80];
		unsigned k, o = 0;

		for (k = 0; k < got && o < sizeof(line) - 4; k++) {
			u8 c = buf[k];

			line[o++] = (c >= 0x20 && c < 0x7f) ? c : '.';
		}
		line[o] = 0;
		pr_info(DRV ": *** \"%s\" ***\n", line);
		pr_info(DRV ": first bytes: %02x %02x %02x %02x %02x %02x\n",
			buf[0], got>1?buf[1]:0, got>2?buf[2]:0,
			got>3?buf[3]:0, got>4?buf[4]:0, got>5?buf[5]:0);
	} else {
		pr_info(DRV ": nothing received (RIR=%02x RFOC=%02x RISRl=%02x CSR=%02x)\n",
			crd(p, chip, RIR), crd(p, chip, RFOC),
			crd(p, chip, RISRl), crd(p, chip, CSR));
	}
}

/*
 * Find the real bit-rate settings by measurement.
 *
 * §103 derived "CLK is about 10 MHz" from IOS's TBPR=0x81 together with
 * TCOR=0x00.  That dump was taken after a reset, and TCOR's reset default IS
 * 0x00 -- so a retained value may have been paired with a default one, and the
 * arithmetic is only as good as that pairing.  §109 sends fine but the Arista's
 * echo comes back as c0 3c c0 3c, which is what a wrong divisor looks like.
 *
 * So sweep clock select and divisor, prod the far end, and keep whatever
 * decodes as printable ASCII.  The switch echoes, so a correct setting shows up
 * as legible characters rather than as a particular value.
 */
static void nm32a_baud_sweep(struct nm32a *p, unsigned chip, unsigned chan)
{
	unsigned sel, bpr, best_sel = 0, best_bpr = 0, best_score = 0;
	int n, i;

	pr_info(DRV ": sweeping clock select 0-2 x divisor 1-255 on port 16\n");

	for (sel = 0; sel <= 2; sel++) {
		for (bpr = 1; bpr < 256; bpr++) {
			unsigned printable = 0, got = 0;

			cwr(p, chip, CAR, chan & 3);
			cwr(p, chip, CCR, CCR_INITCH);
			if (ccr_wait(p, chip))
				continue;
			cwr(p, chip, RCOR, sel); cwr(p, chip, RBPR, bpr);
			cwr(p, chip, TCOR, sel); cwr(p, chip, TBPR, bpr);
			cwr(p, chip, CMR, 0x02);
			cwr(p, chip, COR1, 0x17);
			cwr(p, chip, COR2, 0x00);
			cwr(p, chip, COR3, 0x02);
			cwr(p, chip, COR4, 0x01);
			cwr(p, chip, COR5, 0x00);
			cwr(p, chip, CCR, CCR_INITCH);
			if (ccr_wait(p, chip))
				continue;
			cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
			if (ccr_wait(p, chip))
				continue;
			cwr(p, chip, IER, 0x09);

			/* prod */
			for (n = 0; n < 60000; n++)
				if (crd(p, chip, TIR) & 0x80)
					break;
			if (crd(p, chip, TIR) & 0x80) {
				__raw_readb(p->bar + NM32A_ACK);
				cwr(p, chip, TDR, '\r');
				cwr(p, chip, TEOIR, 0x00);
			}
			mdelay(60);

			/* listen */
			for (n = 0; n < 40000 && got < 32; n++) {
				if (!(crd(p, chip, RIR) & 0x80))
					continue;
				__raw_readb(p->bar + NM32A_ACK);
				i = crd(p, chip, RFOC);
				while (i-- > 0 && got < 32) {
					u8 c = crd(p, chip, RDR);

					got++;
					if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n')
						printable++;
				}
				cwr(p, chip, REOIR, 0x00);
			}
			if (got && printable * 4 >= got * 3 && printable > best_score) {
				best_score = printable;
				best_sel = sel; best_bpr = bpr;
				pr_info(DRV ": sel=%u bpr=0x%02x -> %u/%u printable\n",
					sel, bpr, printable, got);
			}
		}
	}
	if (best_score)
		pr_info(DRV ": *** best: ClkSel=%u BPR=0x%02x (%u printable) ***\n",
			best_sel, best_bpr, best_score);
	else
		pr_info(DRV ": nothing decoded as ASCII anywhere in the sweep\n");
}

/*
 * Pin the divisor by echo match rather than by "looks printable".
 *
 * Both live tests returned exactly as many bytes as were sent -- 6 for 6, 1 for
 * 1 -- which is a console echoing character for character.  So the sharp test
 * is whether the echo comes back byte-identical to what went out: at the right
 * divisor it matches exactly, at a wrong one it does not.  That needs no guess
 * about what the far end would say unprompted.
 */
static void nm32a_echo_match(struct nm32a *p, unsigned chip, unsigned chan)
{
	static const u8 probe[6] = { 'U', 'U', 'A', 'A', 'U', 'A' };
	/*
	 * Standard rates at the measured CLK of 35.08 MHz (§110).  clk0 cannot
	 * reach 9600 (divisor 455 > 8 bits), so the low rates live on clk1.
	 */
	static const struct { u8 sel, bpr; unsigned rate; } cand[] = {
		{ 1, 0xe3,   4800 }, { 1, 0x71,   9600 }, { 1, 0x38,  19200 },
		{ 1, 0x1c,  38400 }, { 1, 0x12,  57600 }, { 1, 0x09, 115200 },
		{ 0, 0xe3,  19200 }, { 0, 0x71,  38400 }, { 0, 0x4b,  57600 },
		{ 0, 0x25, 115200 },
	};
	unsigned c;	pr_info(DRV ": echo-match on port 16, probe = 55 55 41 41 55 41\n");

	for (c = 0; c < ARRAY_SIZE(cand); c++) {
		u8 got[16];
		unsigned ngot = 0, match = 0, printable = 0;
		int n, i;

		cwr(p, chip, CAR, chan & 3);
		cwr(p, chip, RCOR, RCOR_CLK(cand[c].sel)); cwr(p, chip, RBPR, cand[c].bpr);
		cwr(p, chip, TCOR, TCOR_CLK(cand[c].sel)); cwr(p, chip, TBPR, cand[c].bpr);
		cwr(p, chip, CMR, 0x02);
		cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
		cwr(p, chip, COR4, 0x01); cwr(p, chip, COR5, 0x00);
		cwr(p, chip, CCR, CCR_INITCH); if (ccr_wait(p, chip)) continue;
		cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX); if (ccr_wait(p, chip)) continue;
		cwr(p, chip, IER, 0x09);

		for (n = 0; n < 60000; n++)
			if (crd(p, chip, TIR) & 0x80)
				break;
		if (!(crd(p, chip, TIR) & 0x80))
			continue;
		__raw_readb(p->bar + NM32A_ACK);
		for (i = 0; i < 6; i++)
			cwr(p, chip, TDR, probe[i]);
		cwr(p, chip, TEOIR, 0x00);

		mdelay(120);
		for (n = 0; n < 80000 && ngot < sizeof(got); n++) {
			if (!(crd(p, chip, RIR) & 0x80))
				continue;
			__raw_readb(p->bar + NM32A_ACK);
			i = crd(p, chip, RFOC);
			while (i-- > 0 && ngot < sizeof(got))
				got[ngot++] = crd(p, chip, RDR);
			cwr(p, chip, REOIR, 0x00);
		}
		for (i = 0; i < (int)ngot && i < 6; i++) {
			if (got[i] == probe[i])
				match++;
			if (got[i] >= 0x20 && got[i] < 0x7f)
				printable++;
		}
		pr_info(DRV ": %6u baud (sel=%u bpr=0x%02x): got %u  %02x %02x %02x "
			"%02x %02x %02x  match %u/6%s\n",
			cand[c].rate, cand[c].sel, cand[c].bpr, ngot,
			got[0], ngot>1?got[1]:0, ngot>2?got[2]:0,
			ngot>3?got[3]:0, ngot>4?got[4]:0, ngot>5?got[5]:0,
			match, match >= 5 ? "   <== MATCH" : "");
	}
	pr_info(DRV ": echo-match sweep done\n");
}

/*
 * Measure the bit-rate clock instead of deriving it.
 *
 * §103 computed "CLK is about 10 MHz" from IOS's TBPR paired with a TCOR that
 * may have been a reset default rather than a retained value, and everything
 * downstream inherited that uncertainty.  The chip will tell us directly: send
 * a known number of characters at a known divisor and time how long the line
 * is busy.  Each 8N1 character is exactly 10 bit times, so
 *
 *     baud = chars * 10 / seconds        CLK = baud * (BPR + 1) * 8
 *
 * with no assumption about anything.  TLVal (RCOR bit 7) shows when the line
 * has gone back to idle.
 */
static void nm32a_measure_baud(struct nm32a *p, unsigned chip, unsigned chan)
{
	const unsigned bprs[] = { 0x81, 0x86, 0x71, 0x40, 0x20 };
	unsigned b;

	/*
	 * Measure ONE character on the wire, not a burst.
	 *
	 * Timing a 200-character burst measured our own polling loops: divisors
	 * 0x81 and 0x20 differ by 3.94x but the "rates" differed by 1.96x, and
	 * the implied CLK came out anywhere from 26 to 142 MHz.  Software
	 * dominated the interval.
	 *
	 * Sending a single 0x00 gives a start bit plus eight zero data bits --
	 * nine consecutive bit times with the line low, bounded by idle-high on
	 * both sides.  Timing that low period measures the line alone:
	 *
	 *     baud = 9 / t_low        CLK = baud * (BPR + 1) * 8
	 */

	for (b = 0; b < ARRAY_SIZE(bprs); b++) {
		unsigned bpr = bprs[b], sent = 0;
		ktime_t t0, t1;
		s64 us;
		int n;

		cwr(p, chip, CAR, chan & 3);
		cwr(p, chip, RCOR, 0x00); cwr(p, chip, RBPR, bpr);
		cwr(p, chip, TCOR, 0x00); cwr(p, chip, TBPR, bpr);
		cwr(p, chip, CMR, 0x02);
		cwr(p, chip, COR1, 0x17); cwr(p, chip, COR2, 0x00); cwr(p, chip, COR3, 0x02);
		cwr(p, chip, COR4, 0x08); cwr(p, chip, COR5, 0x00);
		cwr(p, chip, CCR, CCR_INITCH); if (ccr_wait(p, chip)) continue;
		cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX); if (ccr_wait(p, chip)) continue;
		cwr(p, chip, IER, 0x09);

		us = 0; sent = 0;
		for (n = 0; n < 8; n++) {		/* a few characters, averaged */
			int k;

			for (k = 0; k < 200000 && !(crd(p, chip, TIR) & 0x80); k++)
				;
			if (!(crd(p, chip, TIR) & 0x80))
				break;
			__raw_readb(p->bar + NM32A_ACK);
			cwr(p, chip, TDR, 0x00);
			cwr(p, chip, TEOIR, 0x00);

			/* wait for the start bit: line leaves idle */
			for (k = 0; k < 2000000; k++)
				if (!(crd(p, chip, RCOR) & 0x80))
					break;
			if (k == 2000000)
				break;
			t0 = ktime_get();
			/* ... and for it to return to idle: 9 bit times low */
			for (k = 0; k < 2000000; k++)
				if (crd(p, chip, RCOR) & 0x80)
					break;
			t1 = ktime_get();
			us += ktime_to_us(ktime_sub(t1, t0));
			sent++;
		}
		if (sent && us > 0) {
			/* 64-bit divide needs the helper on 32-bit MIPS */
			/* the low period is 9 bit times (start + eight zeros) */
			u32 baud = div_u64((u64)sent * 9 * 1000000ULL, (u32)us);
			u32 clk  = baud * (bpr + 1) * 8;

			pr_info(DRV ": BPR=0x%02x: %u chars, %u us low -> ~%u baud"
				"  => CLK ~ %u.%02u MHz\n",
				bpr, sent, (u32)us, baud,
				clk / 1000000, (clk / 10000) % 100);
		}
	}
}


/* ------------------------------------------------------------------ *
 *  tty driver
 *
 *  32 ports, /dev/ttyNM0..31, laid out the way IOS numbers them: port n
 *  is chip n/4, channel n%4 -- the mapping in the chip-offset table at
 *  0x46581470 (§92).  Port 16 is the first port of the third octal cable.
 *
 *  No interrupt is wired from the card, so a kthread polls the eight chips.
 *  Each service is entered by the acknowledge cycle -- a read of BAR+0
 *  (§108) -- without which TDR/RDR/EOIR do nothing at all.
 * ------------------------------------------------------------------ */

/*
 * Bit rates.  CLK measured at 35.08 MHz (§110); clk0 = CLK/8, clk1 = CLK/32.
 * ClkSel sits in different bits in TCOR and RCOR (§113), which is what
 * TCOR_CLK()/RCOR_CLK() exist to keep straight.  clk0 cannot express 9600
 * (divisor 455 > 8 bits), so the low rates live on clk1.
 */
static const struct { unsigned rate; u8 sel, bpr; } nm32a_baud[] = {
	{   1200, 1, 0xff }, {   2400, 1, 0xe3 }, {   4800, 1, 0x71 },
	{   9600, 1, 0x71 }, {  19200, 1, 0x38 }, {  38400, 0, 0x71 },
	{  57600, 0, 0x4b }, { 115200, 0, 0x25 },
};

static void nm32a_set_rate(struct nm32a *p, unsigned chip, unsigned rate)
{
	unsigned i, best = 3;			/* default 9600 */

	for (i = 0; i < ARRAY_SIZE(nm32a_baud); i++)
		if (nm32a_baud[i].rate == rate) {
			best = i;
			break;
		}
	cwr(p, chip, TCOR, TCOR_CLK(nm32a_baud[best].sel));
	cwr(p, chip, TBPR, nm32a_baud[best].bpr);
	cwr(p, chip, RCOR, RCOR_CLK(nm32a_baud[best].sel));
	cwr(p, chip, RBPR, nm32a_baud[best].bpr);
}

static void nm32a_chan_init(struct nm32a *p, unsigned chip, unsigned chan,
			    unsigned rate)
{
	cwr(p, chip, CAR, chan & 3);
	cwr(p, chip, LIVR, 0x40);
	nm32a_set_rate(p, chip, rate);
	cwr(p, chip, CMR,  0x02);		/* async, interrupt mode (§103) */
	cwr(p, chip, COR1, 0x17);		/* 8 bits, no parity */
	cwr(p, chip, COR2, 0x00);
	cwr(p, chip, COR3, 0x02);		/* 1 stop bit */
	cwr(p, chip, COR4, 0x01);		/* interrupt per character */
	cwr(p, chip, COR5, 0x00);
	cwr(p, chip, CCR, CCR_INITCH);
	ccr_wait(p, chip);
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	ccr_wait(p, chip);
	cwr(p, chip, MSVR_RTS, 0x01);
	cwr(p, chip, MSVR_DTR, 0x02);
	cwr(p, chip, IER, 0x09);		/* RxD | TxD -- required (§109) */
}

/*
 * Re-arm transmit for any channel that has data queued.
 *
 * This has to happen in the poll thread rather than in write(): CAR is
 * per-CHIP channel select, so setting it from another context while a service
 * is in progress -- where the acknowledge established the channel -- aims those
 * accesses at the wrong channel and loses characters.  Keeping every chip
 * access in this one thread removes the race by construction.
 */
static void nm32a_arm_tx(struct nm32a *p, unsigned chip)
{
	unsigned c;

	for (c = 0; c < 4; c++) {
		struct nm32a_port *np = &p->ports[chip * 4 + c];
		bool pending;

		spin_lock(&np->lock);
		pending = np->open && np->head != np->tail;
		spin_unlock(&np->lock);
		if (!pending)
			continue;
		cwr(p, chip, CAR, c);
		cwr(p, chip, IER, 0x09);		/* RxD | TxD */
	}
}

/*
 * One acknowledge per pass, and let the CHIP say what it granted.
 *
 * The acknowledge is a single card-level cycle and the chip chooses which
 * service it hands over.  An earlier version acknowledged once for transmit and
 * again for receive in the same pass, assuming each got what it asked for.  When
 * the chip granted the other one, TDR was written inside a receive context (and
 * vice versa), corrupting both directions -- characters vanishing mid-command in
 * exactly the way "show version" arrived as "srsion".
 *
 * So: look before, acknowledge once, look again.  Whichever enable bit dropped
 * is the service we are actually in, and only that one is serviced.
 */
static void nm32a_service(struct nm32a *p, unsigned chip)
{
	unsigned long flags;
	struct nm32a_port *np;
	u8 tir0, rir0, tir1, rir1;
	u8 rxbuf[16];
	int i, room, cnt = 0;
	bool did_rx = false;

	spin_lock_irqsave(&p->hw_lock, flags);

	tir0 = crd(p, chip, TIR);
	rir0 = crd(p, chip, RIR);
	if (!((tir0 | rir0) & 0x80)) {		/* nothing pending */
		spin_unlock_irqrestore(&p->hw_lock, flags);
		return;
	}

	__raw_readb(p->bar + NM32A_ACK);	/* the one acknowledge (§108) */

	tir1 = crd(p, chip, TIR);
	rir1 = crd(p, chip, RIR);

	if ((rir0 & 0x80) && !(rir1 & 0x80)) {
		/* receive was granted */
		unsigned chan = rir0 & 3;

		np = &p->ports[chip * 4 + chan];
		cnt = crd(p, chip, RFOC) & 0x1f;
		if (cnt > (int)sizeof(rxbuf))
			cnt = sizeof(rxbuf);
		for (i = 0; i < cnt; i++)
			rxbuf[i] = crd(p, chip, RDR);
		cwr(p, chip, REOIR, 0x00);
		did_rx = true;
	} else if ((tir0 & 0x80) && !(tir1 & 0x80)) {
		/* transmit was granted */
		unsigned chan = tir0 & 3;

		np = &p->ports[chip * 4 + chan];
		spin_lock(&np->lock);
		room = crd(p, chip, TFTC);
		if (room > 16)
			room = 16;
		for (i = 0; i < room && np->head != np->tail; i++) {
			cwr(p, chip, TDR, np->tx[np->tail]);
			np->tail = (np->tail + 1) % NM32A_TXBUF;
		}
		if (!i)
			cwr(p, chip, IER, 0x08);	/* idle: stop asking */
		cwr(p, chip, TEOIR, i ? 0x00 : 0x08);
		spin_unlock(&np->lock);
	} else {
		/* neither cleared: end the service without transferring */
		cwr(p, chip, TEOIR, 0x08);
	}

	spin_unlock_irqrestore(&p->hw_lock, flags);

	if (did_rx && cnt > 0 && np->open) {
		tty_insert_flip_string(&np->port, rxbuf, cnt);
		tty_flip_buffer_push(&np->port);
	}
}

static int nm32a_poll_thread(void *data)
{
	struct nm32a *p = data;

	while (!kthread_should_stop()) {
		unsigned chip, active = 0;

		for (chip = 0; chip < NM32A_CHIPS; chip++) {
			unsigned c, busy = 0;

			for (c = 0; c < 4; c++)
				if (p->ports[chip * 4 + c].open)
					busy = 1;
			if (!busy)
				continue;		/* nothing open on this chip */
			active++;
			nm32a_arm_tx(p, chip);
			nm32a_service(p, chip);
		}
		if (active)
			usleep_range(1000, 2000);
		else
			msleep(50);
	}
	return 0;
}

/*
 * tty_port_open() calls port->ops->activate(), so port->ops must be set:
 * leaving it NULL faults at tty_port_open+0xb0 with BadVA 0x0000000c, which is
 * the offset of ->activate inside a NULL tty_port_operations.
 *
 * The channel bring-up belongs here rather than in ->open: activate() runs once
 * per first open, under port->mutex, which is exactly the lifetime the hardware
 * setup should follow.
 */
static int nm32a_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct nm32a_port *np = container_of(port, struct nm32a_port, port);
	unsigned long flags;

	spin_lock_irqsave(&np->card->hw_lock, flags);
	nm32a_chan_init(np->card, np->chip, np->chan, 9600);
	np->head = np->tail = 0;
	np->open = true;
	spin_unlock_irqrestore(&np->card->hw_lock, flags);
	return 0;
}

static void nm32a_port_shutdown(struct tty_port *port)
{
	struct nm32a_port *np = container_of(port, struct nm32a_port, port);
	unsigned long flags;

	spin_lock_irqsave(&np->card->hw_lock, flags);
	np->open = false;
	cwr(np->card, np->chip, CAR, np->chan);
	cwr(np->card, np->chip, IER, 0x00);	/* stop asking for service */
	spin_unlock_irqrestore(&np->card->hw_lock, flags);
}

static const struct tty_port_operations nm32a_port_ops = {
	.activate = nm32a_port_activate,
	.shutdown = nm32a_port_shutdown,
};

static struct nm32a_port *nm32a_lookup(struct tty_struct *tty)
{
	struct nm32a *p = tty->driver->driver_state;

	return &p->ports[tty->index];
}

static int nm32a_tty_open(struct tty_struct *tty, struct file *f)
{
	struct nm32a_port *np = nm32a_lookup(tty);

	tty->driver_data = np;
	return tty_port_open(&np->port, tty, f);
}

static void nm32a_tty_close(struct tty_struct *tty, struct file *f)
{
	struct nm32a_port *np = tty->driver_data;

	if (np)
		tty_port_close(&np->port, tty, f);
}

static ssize_t nm32a_tty_write(struct tty_struct *tty, const u8 *buf, size_t n)
{
	struct nm32a_port *np = tty->driver_data;
	unsigned long flags;
	size_t i;

	if (!np)
		return -ENODEV;
	spin_lock_irqsave(&np->lock, flags);
	for (i = 0; i < n; i++) {
		unsigned next = (np->head + 1) % NM32A_TXBUF;

		if (next == np->tail)
			break;
		np->tx[np->head] = buf[i];
		np->head = next;
	}
	spin_unlock_irqrestore(&np->lock, flags);
	/*
	 * Deliberately no chip access here.  CAR is per-CHIP channel-select
	 * state, and writing it from the writer while the poll thread is inside
	 * a service -- where the channel context was set by the acknowledge --
	 * steers those accesses at the wrong channel.  That was losing
	 * characters mid-command ("show interfaces status" arriving as "show
	 * interfce taus").  IER now stays enabled from channel init, so the
	 * chip keeps asking for transmit service and the poll thread is the
	 * only thing that touches the hardware.
	 */
	return i;
}

static unsigned int nm32a_tty_write_room(struct tty_struct *tty)
{
	struct nm32a_port *np = tty->driver_data;
	unsigned used;

	if (!np)
		return 0;
	used = (np->head - np->tail + NM32A_TXBUF) % NM32A_TXBUF;
	return NM32A_TXBUF - 1 - used;
}

static void nm32a_tty_set_termios(struct tty_struct *tty,
				  const struct ktermios *old)
{
	struct nm32a_port *np = tty->driver_data;
	unsigned rate = tty_get_baud_rate(tty);
	unsigned long flags;

	if (!np)
		return;
	spin_lock_irqsave(&np->card->hw_lock, flags);
	cwr(np->card, np->chip, CAR, np->chan);
	nm32a_set_rate(np->card, np->chip, rate ? rate : 9600);
	cwr(np->card, np->chip, CCR, CCR_INITCH);
	ccr_wait(np->card, np->chip);
	cwr(np->card, np->chip, CCR, CCR_ENTX | CCR_ENRX);
	ccr_wait(np->card, np->chip);
	cwr(np->card, np->chip, IER, 0x09);
	spin_unlock_irqrestore(&np->card->hw_lock, flags);
}

static const struct tty_operations nm32a_tty_ops = {
	.open		= nm32a_tty_open,
	.close		= nm32a_tty_close,
	.write		= nm32a_tty_write,
	.write_room	= nm32a_tty_write_room,
	.set_termios	= nm32a_tty_set_termios,
};

static int nm32a_tty_setup(struct nm32a *p)
{
	unsigned i;
	int rc;

	p->tty = tty_alloc_driver(NM32A_PORTS_N,
				  TTY_DRIVER_REAL_RAW |
				  TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(p->tty))
		return PTR_ERR(p->tty);

	p->tty->driver_name	= "nm32a";
	p->tty->name		= "ttyNM";
	p->tty->major		= 0;		/* dynamic */
	p->tty->type		= TTY_DRIVER_TYPE_SERIAL;
	p->tty->subtype		= SERIAL_TYPE_NORMAL;
	p->tty->init_termios	= tty_std_termios;
	p->tty->init_termios.c_cflag = B9600 | CS8 | CREAD | CLOCAL | HUPCL;
	p->tty->driver_state	= p;
	tty_set_operations(p->tty, &nm32a_tty_ops);

	for (i = 0; i < NM32A_PORTS_N; i++) {
		struct nm32a_port *np = &p->ports[i];

		tty_port_init(&np->port);
		np->port.ops = &nm32a_port_ops;
		np->card = p;
		np->chip = i / 4;
		np->chan = i % 4;
		spin_lock_init(&np->lock);
	}

	rc = tty_register_driver(p->tty);
	if (rc) {
		tty_driver_kref_put(p->tty);
		return rc;
	}
	for (i = 0; i < NM32A_PORTS_N; i++)
		tty_port_register_device(&p->ports[i].port, p->tty, i,
					 &p->pdev->dev);

	dev_info(&p->pdev->dev,
		 "32 ports registered as /dev/ttyNM0..31 (major %d)\n",
		 p->tty->major);
	return 0;
}

static int nm32a_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nm32a *p;
	int rc;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->pdev = pdev;

	/*
	 * Nothing claimed this device before, so pci_enable_device() was never
	 * called and the card sat with memory decode off -- its whole BAR read
	 * as 0xff and every earlier probe was reading a disabled device
	 * (sec 92).  Enabling it here is what makes the registers appear.
	 */
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);		/* it bus-masters; sec 97 */

	p->bar = pcim_iomap(pdev, 0, 0);
	if (!p->bar)
		return -ENOMEM;

	/* uncached alias of the region plat_mem_setup() held back for us */
	p->dma_phys = C2811_DMA_BASE;
	p->dma = (void __iomem *)CKSEG1ADDR(C2811_DMA_BASE);

	dev_info(&pdev->dev, "BAR0 %pR, DMA scratch at %pa (bus == phys)\n",
		 &pdev->resource[0], &p->dma_phys);

	/*
	 * Nothing else here: probe must leave the card exactly as it was found,
	 * so the state IOS left behind can be read with stage 0.
	 */
	spin_lock_init(&p->hw_lock);

	/* microcode into all eight chips -- without it nothing responds (§95) */
	for (rc = 0; rc < NM32A_CHIPS; rc++)
		if (nm32a_download(p, rc))
			dev_err(&pdev->dev, "chip %d microcode failed\n", rc);

	rc = nm32a_tty_setup(p);
	if (rc)
		dev_err(&pdev->dev, "tty registration failed (%d)\n", rc);
	else {
		p->poller = kthread_run(nm32a_poll_thread, p, "nm32a-poll");
		if (IS_ERR(p->poller)) {
			dev_err(&pdev->dev, "poll thread failed\n");
			p->poller = NULL;
		}
	}

	nm32a_dev = p;
	dev_info(&pdev->dev,
		 "ready; trigger DMA bring-up with "
		 "echo N > /sys/module/nm32a/parameters/selftest\n");

	pci_set_drvdata(pdev, p);
	return 0;
}

static void nm32a_remove(struct pci_dev *pdev)
{
	struct nm32a *p = pci_get_drvdata(pdev);
	unsigned i;

	if (p) {
		if (p->poller)
			kthread_stop(p->poller);
		if (p->tty) {
			for (i = 0; i < NM32A_PORTS_N; i++)
				tty_unregister_device(p->tty, i);
			tty_unregister_driver(p->tty);
			tty_driver_kref_put(p->tty);
		}
	}
	pci_clear_master(pdev);
}

static const struct pci_device_id nm32a_ids[] = {
	{ PCI_DEVICE(0x10ee, 0x4013) },		/* Xilinx XC4013E on the NM-32A */
	{ }
};
MODULE_DEVICE_TABLE(pci, nm32a_ids);

static struct pci_driver nm32a_driver = {
	.name		= DRV,
	.id_table	= nm32a_ids,
	.probe		= nm32a_probe,
	.remove		= nm32a_remove,
};
module_pci_driver(nm32a_driver);

MODULE_DESCRIPTION("Cisco NM-32A 32-port async module (CD2481)");
MODULE_LICENSE("GPL");
