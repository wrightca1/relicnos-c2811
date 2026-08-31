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
#include <linux/mutex.h>
#include <linux/sched.h>
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
#define RTPRh	0x24
#define RTPRl	0x25
/* per channel */
#define CMR	0x1B
#define COR1	0x10
#define COR2	0x17
#define COR3	0x16
#define COR4	0x15
#define COR5	0x14
#define COR6	0x18
#define COR7	0x07
#define STCR	0x12
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
#define RPILR	0xE1		/* Motorola column; Intel would be E3 */
#define TPILR	0xE0		/* was 0xE3 -- that address is MPILR */
#define MPILR	0xE3		/* was 0xE4 -- not a PILR at all */
#define STK	0xE2
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
/*
 * Interrupt acknowledge addresses.
 *
 * There is not ONE acknowledge address -- there are three.  Datasheet 5.2.2:
 * RPILR/TPILR/MPILR "contain the value that will be present on the address bus
 * during the interrupt acknowledge bus cycle for each type of interrupt", and
 * the chip compares their bits 0-6 against A[0-6] to decide whether the
 * acknowledge level is correct.  Present the wrong one and the chip simply does
 * not answer: the read never retires and the machine stops dead.
 *
 * That is exactly what we were doing.  TPILR's address was wrong (0xE3 is
 * MPILR in the Motorola column), so TPILR was never written and kept its
 * default of 0x00 -- which is why acknowledging everything at BAR+0x000
 * serviced TRANSMIT perfectly and hung the board the moment a RECEIVE request
 * arrived.  Not a channel problem, as sec 119 supposed: a type problem.
 */
/*
 * Acknowledge levels, taken from IOS.
 *
 * IOS's own card init at 0x40357930 programs the three PILRs to distinct
 * values -- MPILR=3, TPILR=1, RPILR=2 -- and the level appears on A[6:0]
 * during the acknowledge, so each type is acknowledged at its own address.
 * Only the low two address bits appear to reach the chip on this card, which
 * is why every IOS level fits in 0..3, and why our own experiment with
 * TPILR=4 acknowledged at BAR+0x004 read back ff: 4 aliases to 0 and matched
 * nothing.  BAR+0x000 only ever worked because TPILR's default is 0.
 */
/*
 * Acknowledge level is a module parameter because the two candidate schemes
 * were never compared with data actually flowing:
 *
 *   pilr=0  all three PILRs 0, one acknowledge at BAR+0x000.  This is the
 *           address sec 108 proved, and the datasheet sanctions equal values
 *           (the chip then prioritises internally, receive first).
 *   pilr=1  IOS's levels -- RPILR=2, TPILR=1, MPILR=3 -- each acknowledged at
 *           its own address.
 *
 * pilr=1 is stable but every received byte is garbage at every divisor, which
 * looks more like a mis-established interrupt context than a timing error.
 */
static int pilr;
module_param(pilr, int, 0644);
MODULE_PARM_DESC(pilr, "0 = single acknowledge at +000, 1 = IOS per-type levels");

#define NM32A_ACK_TX	(pilr ? 0x001 : 0x000)
#define NM32A_ACK_RX	(pilr ? 0x002 : 0x000)
#define NM32A_ACK_MD	(pilr ? 0x003 : 0x000)
#define NM32A_ACK	NM32A_ACK_RX	/* legacy: the self-test paths */

/*
 * All three interrupt types share ONE acknowledge level.
 *
 * The datasheet's note under RPILR: "When each of the three Priority Interrupt
 * Level registers is programmed with the same value, they are internally
 * prioritized, with receive as the highest priority, followed by transmit and
 * modem."  That is the model this driver already assumed -- one acknowledge,
 * and the chip decides what to grant.
 *
 * It never worked for receive because RPILR was programmed 0x02 while the
 * acknowledge was issued at BAR+0x000.  The chip compares the PILR against
 * A[0-6] (5.2.2) and simply does not answer a level it does not recognise, so
 * every receive request stalled the read and took the machine down, while
 * transmit sailed through on TPILR's default of 0x00.  Eight power cycles and
 * a channel theory came out of that one mismatched byte.
 *
 * Program all three to 0x00 to match the acknowledge address.
 */


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
	unsigned long	err_win;		/* start of the error window */
	unsigned	err_cnt;		/* receive exceptions in it */
	bool		rx_off;			/* receiver muted after a storm */
};

struct nm32a {
	void __iomem	*bar;
	void __iomem	*dma;		/* uncached alias of the reserved region */
	phys_addr_t	dma_phys;
	struct pci_dev	*pdev;

	struct tty_driver	*tty;
	struct nm32a_port	ports[NM32A_PORTS_N];
	struct task_struct	*poller;
	struct mutex		hw_lock;	/* serialises CAR + service */
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

/*
 * Wait for the chip to accept a channel command.
 *
 * Bounded deliberately, and every caller checks the result.  A CD2481 that has
 * lost its microcode never clears CCR, and an unbounded wait there turns one
 * sick chip into a hung machine -- which is the wrong failure for a box whose
 * whole job is serving 32 independent consoles.  One port degrading is
 * acceptable; the other 31 going down with it is not.
 */
#define CCR_WAIT_MS	250

static int ccr_wait(struct nm32a *p, unsigned chip)
{
	unsigned long end = jiffies + msecs_to_jiffies(CCR_WAIT_MS);

	/*
	 * Bound this in TIME, not in iterations.  A count only bounds the work
	 * if every read costs what you assumed; these are PCI reads to a card
	 * that may be exactly the thing that is sick, and a stalled read makes
	 * a 200000-iteration "bound" unbounded in wall clock.  hw_lock is a
	 * mutex and every caller is process context, so yield while waiting --
	 * a chip that never answers then costs one port, not the machine.
	 */
	do {
		if (crd(p, chip, CCR) == 0)
			return 0;
		cond_resched();
	} while (time_before(jiffies, end));

	pr_warn_ratelimited(DRV ": chip %u: CCR stuck at %02x (GFRCR=%02x) -- "
			    "channel command not accepted\n",
			    chip, crd(p, chip, CCR), crd(p, chip, GFRCR));
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
	/*
	 * COR6/COR7/STCR must be written, not inherited.
	 *
	 * IOS loads a full channel image (0x00-0x1d at 0x4035f9a0) and sets all
	 * three; we set neither, so a channel kept whatever the last owner left
	 * -- and on this board the last owner is usually IOS.  In async mode
	 * COR6 is the UNIX-tty helper: IgnCR/ICRNL/INLCF silently translate or
	 * drop CR and NL, and ParMrk delivers an errored character prefixed with
	 * FF 00.  COR7 strips the eighth bit and enables LNext processing.  Any
	 * of those turns clean console text into convincing garbage, which is
	 * indistinguishable from a baud error unless you know to look.
	 *
	 * Zero means: no translation, no marking, errors reported normally.
	 */
	cwr(p, chip, COR6, 0x00);
	cwr(p, chip, COR7, 0x00);
	cwr(p, chip, STCR, 0x00);		/* no special transmit command */
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
	cwr(p, chip, RPILR, NM32A_ACK_RX);
	cwr(p, chip, TPILR, NM32A_ACK_TX);
	cwr(p, chip, MPILR, NM32A_ACK_MD);

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
 * Bit rates, taken from IOS's own table (image vaddr 0x465814B0, 8-byte
 * entries: u32 rate, u8 ClkSel, u8 divisor).
 *
 * ⚠ This corrects §110.  That section derived CLK = 35.08 MHz by timing a
 * character, and every entry here contradicts it: IOS drives 4800 through
 * 128000 from ClkSel 0 with divisor = 1152000/rate - 1, so clk0 is
 * 1.152 MHz and CLK is 9.216 MHz -- a factor of 3.8 out.
 *
 * The old numbers were not obviously wrong because 9600 half-worked: sel 1
 * with divisor 114 against a 1.152 MHz clk0 is 10105 baud, 5% fast, which a
 * console tolerates in short bursts.  It fell apart at 115200, where the same
 * error made the receiver ~3.8x too slow and collapsed a whole reply into a
 * single byte -- a convincing impression of a protocol bug.
 *
 * ClkSel sits in different bits in TCOR and RCOR (§113), which is what
 * TCOR_CLK()/RCOR_CLK() exist to keep straight.
 */
static const struct { unsigned rate; u8 sel, bpr; } nm32a_baud[] = {
	{   1200, 2, 0xe3 }, {   2400, 2, 0x71 }, {   4800, 1, 0xe3 },
	{   9600, 1, 0x71 }, {  19200, 1, 0x38 }, {  38400, 0, 0x71 },
	{  57600, 0, 0x4b }, { 115200, 0, 0x25 },
};

/*
 * Runtime divisor override, for finding a rate empirically.
 *
 * Every rate this driver has ever PROVEN on the wire (9600 against the Arista,
 * 19200) uses clk1.  clk0 -- which the table selects for 38400 and above -- has
 * never once been confirmed to work on this card.  Rather than rebuild per
 * guess, allow ClkSel and the divisor to be set directly and swept.
 */
/*
 * Local loopback (TCOR bit 1, LLM).  Loops the transmitter back into the
 * receiver inside the chip, so a rate can be verified with nothing plugged in:
 * write a known pattern, read it back.  Every rate this driver has proven used
 * clk1; clk0 -- which 38400 and above select -- has never been confirmed, and
 * a receive that returns the same byte for every character looks exactly like a
 * clock that is not what we think it is.
 */
/*
 * RECEIVE-ONLY divisor override.
 *
 * fsel/fbpr move both directions at once, which confounds every sweep: get the
 * transmit rate wrong and the far end stops replying, so "no bytes" says
 * nothing about the receive setting.  These pin the receiver alone.
 */
static int frsel = -1;
module_param(frsel, int, 0644);
MODULE_PARM_DESC(frsel, "override RECEIVE ClkSel only, -1 = follow the rest");
static int frbpr = -1;
module_param(frbpr, int, 0644);
MODULE_PARM_DESC(frbpr, "override RECEIVE divisor only, -1 = follow the rest");

static int llm;
module_param(llm, int, 0644);
MODULE_PARM_DESC(llm, "1 = local loopback, for verifying a rate with no cable");

static int fsel = -1;
module_param(fsel, int, 0644);
MODULE_PARM_DESC(fsel, "override ClkSel (0=CLK/8 1=/32 2=/128), -1 = use table");
static int fbpr = -1;
module_param(fbpr, int, 0644);
MODULE_PARM_DESC(fbpr, "override bit-rate divisor, -1 = use table");

static void nm32a_set_rate(struct nm32a *p, unsigned chip, unsigned rate)
{
	unsigned i, best = 3;			/* default 9600 */
	u8 sel, bpr;

	for (i = 0; i < ARRAY_SIZE(nm32a_baud); i++)
		if (nm32a_baud[i].rate == rate) {
			best = i;
			break;
		}
	sel = (fsel >= 0) ? fsel : nm32a_baud[best].sel;
	bpr = (fbpr >= 0) ? fbpr : nm32a_baud[best].bpr;
	cwr(p, chip, TCOR, TCOR_CLK(sel) | (llm ? 0x02 : 0x00));
	cwr(p, chip, TBPR, bpr);
	cwr(p, chip, RCOR, RCOR_CLK(frsel >= 0 ? frsel : sel));
	cwr(p, chip, RBPR, frbpr >= 0 ? frbpr : bpr);
}

/*
 * Counters, not traces.  printk at 9600 costs ~1ms a line, which slows the poll
 * loop enough to CAUSE the overruns it is meant to observe -- that has now
 * misled this investigation twice.  Counting is free; read them afterwards.
 */
static unsigned long st_good, st_bytes, st_overrun, st_break, st_timeout, st_svc;
module_param(st_good, ulong, 0644);
module_param(st_bytes, ulong, 0644);
module_param(st_overrun, ulong, 0644);
module_param(st_break, ulong, 0644);
module_param(st_timeout, ulong, 0644);
module_param(st_svc, ulong, 0644);

static int polldelay = 50;		/* 50us: measured overrun-free at 115200 */
module_param(polldelay, int, 0644);
MODULE_PARM_DESC(polldelay, "microseconds between sweeps while a port is open");

static int rxthresh = 1;
module_param(rxthresh, int, 0644);
MODULE_PARM_DESC(rxthresh, "receive FIFO threshold, COR4[3:0]");

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
	/*
	 * Receive FIFO threshold, not one interrupt per character.
	 *
	 * COR4[3:0] is the threshold.  At 0x01 the chip demands a service for
	 * every byte, and each service costs a mutex, an acknowledge, several
	 * PCI register reads and an EOIR handshake -- so at 115200 the overhead
	 * alone eats the 1.4ms a 16-byte FIFO gives us, and the tail of every
	 * burst is lost to RISRl=08 (overrun).  A threshold of 8 cuts the
	 * service count eightfold and leaves half the FIFO as headroom.
	 *
	 * A threshold on its own would strand the last few characters of a
	 * burst, so pair it with the receive timeout: RTPR non-zero plus RET
	 * (IER bit 5) raises a timeout interrupt when data stops with a
	 * partially full FIFO, which is exactly the end of a console line.
	 */
	/*
	 * Threshold 1, not 8.
	 *
	 * A threshold only pays off if the timeout reliably flushes what is
	 * left below it, and the receive timer here does not fire -- so the
	 * tail of every burst shorter than the threshold was stranded, which
	 * is why a 23-character prompt arrived as exactly 16 bytes and
	 * "login: " never came.  Interrupting per character costs more
	 * services, but the poll loop now drains a chip until it is empty and
	 * does not sleep after doing work, which is what made the old
	 * per-character threshold overrun.
	 */
	cwr(p, chip, COR4, rxthresh & 0x0f);
	cwr(p, chip, RTPRh, 0x00);
	cwr(p, chip, RTPRl, 0x20);		/* flush a partial FIFO when idle */
	cwr(p, chip, COR5, 0x00);
	/*
	 * These three must be written on every channel init, not inherited.
	 * IOS loads a full channel image (0x4035f9a0); we set none of them, so
	 * a channel kept whatever the previous owner left -- and on this board
	 * that is usually IOS.  COR6 is the UNIX-tty helper (IgnCR/ICRNL/INLCF
	 * translate or drop CR and NL; ParMrk prefixes an errored character
	 * with FF 00) and COR7 strips the eighth bit -- any of which turns
	 * clean console text into convincing garbage.
	 */
	cwr(p, chip, COR6, 0x00);		/* no CR/NL translation or marking */
	cwr(p, chip, COR7, 0x00);		/* no 8th-bit strip, no LNext */
	cwr(p, chip, STCR, 0x00);		/* no special transmit command */
	/*
	 * TPR clocks the receive timeout we enabled above (RTPR + RET).  It is
	 * CLK/2048 and the datasheet requires at least 0x10 for the timer to
	 * keep accuracy -- about a 1 ms tick.
	 */
	cwr(p, chip, TPR, 0x40);
	cwr(p, chip, CCR, CCR_INITCH);
	ccr_wait(p, chip);
	cwr(p, chip, CCR, CCR_ENTX | CCR_ENRX);
	ccr_wait(p, chip);
	cwr(p, chip, MSVR_RTS, 0x01);
	cwr(p, chip, MSVR_DTR, 0x02);
	cwr(p, chip, IER, 0x29);		/* RET | RxD | TxD (§109) */
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
/*
 * Re-arm transmit interest for any channel with queued bytes.
 *
 * This MUST hold hw_lock.  CAR is per-CHIP channel-select state, and this
 * function sets it -- so without the lock it races the tty paths, which set
 * CAR and then issue channel commands against it.  A CCR command that lands
 * on the wrong channel (or on a channel mid-service) is exactly how a CD2481
 * gets wedged, and a wedged chip stops answering PCI reads: the CPU then
 * stalls inside the load instruction, where no software watchdog can reach it.
 * That is a machine you can only recover at the rack, so the lock is not
 * optional here.
 */
static void nm32a_arm_tx(struct nm32a *p, unsigned chip)
{
	unsigned c;

	mutex_lock(&p->hw_lock);
	for (c = 0; c < 4; c++) {
		struct nm32a_port *np = &p->ports[chip * 4 + c];
		bool pending;

		spin_lock(&np->lock);
		pending = np->open && np->head != np->tail;
		spin_unlock(&np->lock);
		if (!pending)
			continue;
		cwr(p, chip, CAR, c);
		cwr(p, chip, IER, np->rx_off ? 0x01 : 0x29);	/* RET | RxD | TxD */
	}
	mutex_unlock(&p->hw_lock);
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
/*
 * A port with nothing plugged into it generates receive exceptions forever.
 * Servicing them costs the poll thread real time, and this box exists to serve
 * 31 OTHER consoles -- so after a sustained storm, mute this channel's receiver
 * and say so.  Transmit is left alone, and the next open clears the mute.
 */


static int trace;
module_param(trace, int, 0644);
MODULE_PARM_DESC(trace, "trace this many service passes to the console");

#define NM32A_ERR_WINDOW	HZ		/* accounting window */
#define NM32A_ERR_LIMIT		200		/* exceptions/window before muting */

static void nm32a_rx_exception(struct nm32a *p, unsigned chip, unsigned chan,
			       u8 risr)
{
	struct nm32a_port *np = &p->ports[chip * 4 + chan];

	if (time_after(jiffies, np->err_win + NM32A_ERR_WINDOW)) {
		np->err_win = jiffies;
		np->err_cnt = 0;
	}
	if (++np->err_cnt != NM32A_ERR_LIMIT || np->rx_off)
		return;

	np->rx_off = true;
	cwr(p, chip, CAR, chan);
	cwr(p, chip, IER, 0x01);		/* TxD only: 0x08 is RxD */
	pr_warn(DRV ": ttyNM%u: %u receive exceptions in one second "
		"(RISRl=%02x); muting the receiver -- is anything cabled to "
		"this port?\n", chip * 4 + chan, np->err_cnt, risr);
}

/*
 * End an interrupt service, and CONFIRM the context actually popped.
 *
 * STK (0xE2) is a 4-deep nesting stack whose bits 7 and 0 are CLvl[1:0], the
 * currently active interrupt level; it is pushed by the acknowledge and popped
 * by the EOIR write.  The write does not always take on the first attempt --
 * IOS re-issues it, polling STK, up to 100 times (0x403628a4..0x403628dc), and
 * so do we.  Leaving a context open is not a small matter here: the next
 * acknowledge nests onto it, the chip never answers, and the read stalls with
 * the machine still in it.
 */
/*
 * Re-issuing the EOIR is how IOS confirms the context popped, but a second
 * write may also re-present FIFO data -- received text repeats its tail
 * ("switch login: 610 login:").  Switchable so the two can be compared.
 */
static int eoiretry = 1;
module_param(eoiretry, int, 0644);
MODULE_PARM_DESC(eoiretry, "1 = re-issue EOIR until STK pops (IOS behaviour)");

static void nm32a_eoi(struct nm32a *p, unsigned chip, unsigned reg, u8 val)
{
	int i;

	cwr(p, chip, reg, val);
	if (!eoiretry)
		return;
	for (i = 0; i < 100; i++) {
		if (!(crd(p, chip, STK) & 0x81))
			return;			/* no level active: popped */
		cwr(p, chip, reg, val);
	}
	pr_warn_ratelimited(DRV ": chip %u: interrupt context will not pop "
			    "(STK=%02x)\n", chip, crd(p, chip, STK));
}

/*
 * Services taken per chip per sweep.  Back-to-back services may re-enter the
 * interrupt context before the chip has finished the previous EOIR, which shows
 * up as received text repeating its tail; one per sweep spaces them out at the
 * cost of throughput.  Switchable so the two can be compared on hardware.
 */
/*
 * Spacing between consecutive RDR reads.
 *
 * Received text repeats its tail at the 16-byte service boundary -- a prompt
 * arriving as "switch login: 4610 login:" -- which is the FIFO read pointer
 * failing to keep up with back-to-back PCI reads, so the last bytes are
 * presented again on the next service.  A microsecond between reads costs
 * 16us per full FIFO and nothing that matters at console rates.
 */
static int rdrus = 1;
module_param(rdrus, int, 0644);
MODULE_PARM_DESC(rdrus, "microseconds between consecutive RDR reads");

static int burst = 32;
module_param(burst, int, 0644);
MODULE_PARM_DESC(burst, "services per chip per sweep");

/* returns true if a service was performed -- the caller keeps draining */
static bool nm32a_service(struct nm32a *p, unsigned chip)
{
	unsigned long flags;
	struct nm32a_port *np;
	u8 tir0, rir0, tir1, rir1, erisr, ackv, tirc, rirc;
	unsigned ackaddr;
	bool tr;
	/*
	 * RFOC is five bits, so the FIFO can present up to 31 characters; a
	 * 16-byte buffer clamps the count and the rest is dropped when the
	 * service ends.  That is why a 20-character prompt arrived as exactly
	 * 16 bytes of otherwise perfect text.
	 */
	u8 rxbuf[32];
	int i, room, cnt = 0;
	bool did_rx = false;

	mutex_lock(&p->hw_lock);

	tir0 = crd(p, chip, TIR);
	rir0 = crd(p, chip, RIR);

	/*
	 * Trace only PENDING passes.  Tracing every idle sweep printed at 9600
	 * baud and slowed the loop by ~70x, which was enough to hide the bug
	 * entirely -- two "successful" runs proved nothing but that a slow loop
	 * survives.  An idle pass is not interesting; a request is.
	 */
	tr = trace > 0 && ((tir0 | rir0) & 0x80);
	if (tr) {
		trace--;
		pr_info(DRV ": svc c%u: tir0=%02x rir0=%02x\n", chip, tir0, rir0);
	}
	if (!((tir0 | rir0) & 0x80)) {		/* nothing pending */
		mutex_unlock(&p->hw_lock);
		return false;
	}

	/*
	 * CONFIRM the request before acknowledging it.
	 *
	 * The acknowledge is an IACK bus cycle, and it is only legal while the
	 * chip actually has a service to grant.  Acknowledge a request that has
	 * already gone away and nothing answers the cycle: the read never
	 * retires and the CPU stalls inside the load, interrupts off.  That is
	 * the failure this driver kept dying of -- a silent box, no oops, no
	 * console, no softlockup detector, only a power cycle.
	 *
	 * It reproduced in seconds at a 1 ms poll interval and not at all when
	 * tracing slowed the loop to ~70 ms, which is what a narrow timing
	 * window looks like.  So require the request to be present in two
	 * consecutive reads, and treat the second as authoritative.
	 */
	tirc = crd(p, chip, TIR);
	rirc = crd(p, chip, RIR);
	if (!((tir0 | rir0) & (tirc | rirc) & 0x80)) {
		if (tr) pr_info(DRV ": svc c%u: request vanished, not acking\n", chip);
		mutex_unlock(&p->hw_lock);
		return false;
	}
	tir0 = tirc;
	rir0 = rirc;

	/*
	 * Receive outranks transmit (datasheet 5.2.4.1), and the address we
	 * present decides which one we are acknowledging.
	 */
	ackaddr = (rir0 & 0x80) ? NM32A_ACK_RX : NM32A_ACK_TX;
	if (tr) pr_info(DRV ": svc c%u: ACK read at +%03x...\n", chip, ackaddr);
	ackv = __raw_readb(p->bar + ackaddr);

	if (tr) pr_info(DRV ": svc c%u: ACK returned %02x\n", chip, ackv);

	tir1 = crd(p, chip, TIR);
	rir1 = crd(p, chip, RIR);
	if (tr) pr_info(DRV ": svc c%u: tir1=%02x rir1=%02x\n", chip, tir1, rir1);

	/*
	 * Which service did we just get?  With all three PILRs equal the chip
	 * arbitrates receive first (5.2.4.1), so a pending receive request IS
	 * the granted service -- no need to infer it from which bit cleared.
	 *
	 * Getting this right matters beyond tidiness: the EOIR write is what
	 * returns the chip to non-interrupt context, and it must match the type
	 * acknowledged.  The old fallback ended EVERY unrecognised service with
	 * TEOIR, so a receive service that did not clear as expected left the
	 * receive context open for ever; the next acknowledge then nested onto
	 * a context that never ended and stalled the bus.  Harmless while
	 * receive interrupts were never successfully acknowledged -- which was
	 * the state of this driver until the PILR fix.
	 */
	if (rir0 & 0x80) {
		/* receive was granted */
		unsigned chan = rir0 & 3;	/* Rcn[1:0] */

		np = &p->ports[chip * 4 + chan];

		/*
		 * RIR bits 3:2 are Rvct[1:0] and say WHICH receive service this
		 * is (datasheet 9.5.2.x): 11 is good data, 00 is an exception.
		 *
		 * Worth stating plainly because this driver had it inverted, and
		 * the inversion is expensive in both directions: treating good
		 * data as an exception silences a working console, and treating
		 * an exception as good data drains RFOC bytes out of an RDR that
		 * has nothing to give, which stalls the bus cycle and takes the
		 * machine down with no oops and no console output.
		 *
		 * On an exception, RISR carries the cause and the offending
		 * character is discarded by ending the service with NoTrans
		 * (REOIR bit 3) rather than by reading it out.
		 */
		if (tr) pr_info(DRV ": svc c%u: rx chan %u vct=%x\n",
				chip, chan, (rir0 >> 2) & 3);

		if ((rir0 & 0x0c) != 0x0c) {		/* exception, not data */
			/*
			 * Do NOT end this with NoTrans.
			 *
			 * The trace shows one good character per reply, then an
			 * exception, then silence for the rest of the burst --
			 * which is what discarding the FIFO would look like.
			 * RISRl bit 7 is a timeout, and the datasheet is explicit
			 * that a timeout has no character associated with it; the
			 * error cases (overrun/parity/framing/break) DO have one
			 * sitting in the FIFO.  So drain the offending character
			 * when there is one, drop it in software, and end the
			 * service normally -- leaving whatever else has arrived
			 * alone.
			 */
			erisr = crd(p, chip, RISRl);
			/*
			 * A TIMEOUT is not an error -- it is the chip saying
			 * "data stopped, come and collect what is left".  The
			 * datasheet is explicit (5.3.5): the host is expected to
			 * retrieve the characters still in the FIFO.  Discarding
			 * it strands the tail of every burst shorter than the
			 * threshold, which is why a 23-character prompt arrived
			 * as exactly 16 bytes and "login: " never appeared.
			 */
			if (erisr & 0x01) st_break++;
			if (erisr & 0x08) st_overrun++;
			if (erisr & 0x80) {
				st_timeout++;
				cnt = crd(p, chip, RFOC) & 0x1f;
				if (cnt > (int)sizeof(rxbuf))
					cnt = sizeof(rxbuf);
				for (i = 0; i < cnt; i++)
					rxbuf[i] = crd(p, chip, RDR);
				cnt = i;
				nm32a_eoi(p, chip, REOIR, 0x00);
				if (cnt > 0 && np->open) {
					tty_insert_flip_string(&np->port, rxbuf, cnt);
					tty_flip_buffer_push(&np->port);
				}
				mutex_unlock(&p->hw_lock);
				return true;
			}
			if (tr)
				pr_info(DRV ": svc c%u: RISRl=%02x [%s%s%s%s%s] RFOC=%d\n",
					chip, erisr,
					(erisr & 0x80) ? "timeout " : "",
					(erisr & 0x08) ? "overrun " : "",
					(erisr & 0x04) ? "parity " : "",
					(erisr & 0x02) ? "framing " : "",
					(erisr & 0x01) ? "break " : "",
					crd(p, chip, RFOC) & 0x1f);
			if (!(erisr & 0x80) && (crd(p, chip, RFOC) & 0x1f))
				(void)crd(p, chip, RDR);	/* the bad char */
			nm32a_eoi(p, chip, REOIR, 0x00);
			nm32a_rx_exception(p, chip, chan, erisr);
			mutex_unlock(&p->hw_lock);
			return true;
		}

		cnt = crd(p, chip, RFOC) & 0x1f;	/* RxCt4..0, so <= 31 */
		if (cnt > (int)sizeof(rxbuf))
			cnt = sizeof(rxbuf);
		if (tr) pr_info(DRV ": svc c%u: RFOC=%d, draining\n", chip, cnt);
		/*
		 * Latch RFOC once and read exactly that many, as IOS does
		 * (0x40362f18: one lbu of RFOC, then RDR in a loop).
		 *
		 * Re-checking RFOC between reads seems safer and is not: the
		 * count lags the reads, so the guard permits an extra read past
		 * the end and stale bytes come back as duplicated text -- a
		 * prompt arriving as "switch login: 610 login:".  The FIFO is 16
		 * deep and RFOC is five bits, so the latched count is already
		 * bounded by rxbuf.
		 */
		for (i = 0; i < cnt; i++) {
			rxbuf[i] = crd(p, chip, RDR);
			if (rdrus)
				udelay(rdrus);
		}
		cnt = i;
		st_good++; st_bytes += cnt;
		if (tr && cnt > 0)
			pr_info(DRV ": svc c%u: got %02x %02x %02x ('%c')\n", chip,
				rxbuf[0], cnt > 1 ? rxbuf[1] : 0,
				cnt > 2 ? rxbuf[2] : 0,
				(rxbuf[0] >= 0x20 && rxbuf[0] < 0x7f) ? rxbuf[0] : '.');
		nm32a_eoi(p, chip, REOIR, 0x00);
		did_rx = true;
	} else {
		/* transmit was granted */
		unsigned chan = tir0 & 3;

		np = &p->ports[chip * 4 + chan];
		spin_lock_irqsave(&np->lock, flags);
		room = crd(p, chip, TFTC);
		if (room > 16)
			room = 16;
		for (i = 0; i < room && np->head != np->tail; i++) {
			cwr(p, chip, TDR, np->tx[np->tail]);
			np->tail = (np->tail + 1) % NM32A_TXBUF;
		}
		if (!i)
			cwr(p, chip, IER, 0x28);	/* idle: stop asking to send */
		nm32a_eoi(p, chip, TEOIR, i ? 0x00 : 0x08);
		spin_unlock_irqrestore(&np->lock, flags);
	}

	mutex_unlock(&p->hw_lock);

	if (did_rx && cnt > 0 && np->open) {
		tty_insert_flip_string(&np->port, rxbuf, cnt);
		tty_flip_buffer_push(&np->port);
	}
	return true;
}

static int nm32a_poll_thread(void *data)
{
	struct nm32a *p = data;

	while (!kthread_should_stop()) {
		unsigned chip, active = 0, svc;
		bool worked = false;

		for (chip = 0; chip < NM32A_CHIPS; chip++) {
			unsigned c, busy = 0;

			for (c = 0; c < 4; c++)
				if (p->ports[chip * 4 + c].open)
					busy = 1;
			if (!busy)
				continue;		/* nothing open on this chip */
			active++;
			nm32a_arm_tx(p, chip);
			/*
			 * Drain the chip, do not take one service per sweep.
			 *
			 * COR4 asks for a service per character, so at 115200 a
			 * character lands every 87us while the sweep runs every
			 * ~0.5ms -- one service per pass cannot keep up and the
			 * 16-byte FIFO overruns.  It shows up as text with holes
			 * punched through it ("Enablin  Enabling M: 2 GiB"),
			 * which reads like a line fault rather than a driver too
			 * slow to collect.  Keep servicing while the chip still
			 * has something pending, bounded so a stuck request
			 * cannot spin here for ever.
			 */
			for (svc = 0; svc < (unsigned)burst; svc++) {
				if (!nm32a_service(p, chip))
					break;
				worked = true;
			}
		}
		/*
		 * Only sleep once the card has gone quiet.
		 *
		 * 16-byte FIFO: at 115200 a character lands every 87us, so it
		 * overflows in ~1.4ms.  Sleeping 500-1000us after a sweep that
		 * just moved data is ~11 characters of headroom, and scheduling
		 * jitter eats it -- which punches holes through the middle of
		 * words and reads like a line fault.  If we just did work, go
		 * straight round again and only yield the CPU.
		 */
		if (worked) {
			cond_resched();
		} else if (active) {
			/*
			 * udelay, not usleep_range.
			 *
			 * This kernel has CONFIG_HZ=250 and no HIGH_RES_TIMERS,
			 * so usleep_range() cannot sleep less than a jiffy -- 4ms,
			 * whatever range you ask for.  At 115200 that is ~46
			 * characters against a 16-byte FIFO, so a burst overran
			 * before the poll thread ever looked, and a 23-character
			 * prompt arrived as exactly 16 bytes.  The "500-1000us"
			 * in the old call was never what the machine did.
			 *
			 * A busy wait is honest here: 200us keeps us well inside
			 * the 1.4ms the FIFO gives us, and cond_resched() keeps
			 * the box responsive under PREEMPT_NONE.
			 */
			udelay(polldelay);
			cond_resched();
		} else {
			msleep(50);
		}
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
/*
 * Raise or lower DTR/RTS.
 *
 * MSVR bit 1 is DTR and bit 0 is RTS, and writing the MSVR-DTR / MSVR-RTS
 * registers drives the outputs (datasheet 9.4.4).
 *
 * This matters more than it looks.  A serial login prompt comes from a getty on
 * the far end, and a getty resets and reprints when it sees a HANGUP -- DTR
 * dropping.  This driver used to raise DTR/RTS at channel init and never lower
 * them, so closing a console session signalled nothing: the far end went on
 * believing the session was still up, and the login prompt never came back
 * without power-cycling the attached device.  Wiring this into ->dtr_rts lets
 * the tty layer hang the line up on the last close, as every other serial
 * driver does.
 */
static void nm32a_set_mctrl(struct nm32a *p, unsigned chip, unsigned chan,
			    bool active)
{
	cwr(p, chip, CAR, chan);
	cwr(p, chip, MSVR_DTR, active ? 0x02 : 0x00);
	cwr(p, chip, MSVR_RTS, active ? 0x01 : 0x00);
}

static void nm32a_dtr_rts(struct tty_port *port, bool active)
{
	struct nm32a_port *np = container_of(port, struct nm32a_port, port);

	mutex_lock(&np->card->hw_lock);
	nm32a_set_mctrl(np->card, np->chip, np->chan, active);
	mutex_unlock(&np->card->hw_lock);
}

static int nm32a_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct nm32a_port *np = container_of(port, struct nm32a_port, port);
	unsigned rate;

	mutex_lock(&np->card->hw_lock);
	/*
	 * Refuse the open if the chip is not running its microcode: GFRCR still
	 * reading the silicon revision means the download never took, and the
	 * channel commands below would simply time out.  Failing here gives the
	 * caller an error instead of a port that accepts writes and drops them.
	 */
	if (crd(np->card, np->chip, GFRCR) == 0xE0) {
		mutex_unlock(&np->card->hw_lock);
		pr_warn(DRV ": chip %u has no microcode; refusing open of ttyNM%u\n",
			np->chip, np->chip * 4 + np->chan);
		return -ENODEV;
	}
	if (trace > 0)
		pr_info(DRV ": ttyNM%u: activate: chan_init...\n",
			np->chip * 4 + np->chan);
	/*
	 * Program the rate the TTY LAYER holds, not a hardcoded one.
	 *
	 * ->activate runs on every open, so hardcoding 9600 here quietly undid
	 * every stty: the chip went back to 9600 while the tty layer still
	 * believed the speed it had been given.  Reading a 115200 console then
	 * produces a steady stream of plausible-looking garbage, which is easy
	 * to mistake for a protocol bug -- and was.
	 */
	rate = tty ? tty_get_baud_rate(tty) : 0;
	nm32a_chan_init(np->card, np->chip, np->chan, rate ? rate : 9600);
	if (trace > 0)
		pr_info(DRV ": ttyNM%u: activate: chan_init done\n",
			np->chip * 4 + np->chan);
	np->head = np->tail = 0;
	np->err_cnt = 0;
	np->err_win = jiffies;
	np->rx_off = false;
	np->open = true;
	mutex_unlock(&np->card->hw_lock);
	return 0;
}

static void nm32a_port_shutdown(struct tty_port *port)
{
	struct nm32a_port *np = container_of(port, struct nm32a_port, port);

	mutex_lock(&np->card->hw_lock);
	np->open = false;
	cwr(np->card, np->chip, CAR, np->chan);
	cwr(np->card, np->chip, IER, 0x00);	/* stop asking for service */
	/*
	 * Hang the line up.  Without this the far end never learns the session
	 * ended, so its getty sits there and no fresh login prompt appears --
	 * which looked like "the console needs a reboot to come back".
	 */
	nm32a_set_mctrl(np->card, np->chip, np->chan, false);
	mutex_unlock(&np->card->hw_lock);
}

static const struct tty_port_operations nm32a_port_ops = {
	.activate = nm32a_port_activate,
	.dtr_rts  = nm32a_dtr_rts,
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

	if (!np)
		return;
	mutex_lock(&np->card->hw_lock);
	cwr(np->card, np->chip, CAR, np->chan);
	nm32a_set_rate(np->card, np->chip, rate ? rate : 9600);
	cwr(np->card, np->chip, CCR, CCR_INITCH);
	ccr_wait(np->card, np->chip);
	cwr(np->card, np->chip, CCR, CCR_ENTX | CCR_ENRX);
	ccr_wait(np->card, np->chip);
	cwr(np->card, np->chip, IER, 0x09);
	mutex_unlock(&np->card->hw_lock);
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
	mutex_init(&p->hw_lock);

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
