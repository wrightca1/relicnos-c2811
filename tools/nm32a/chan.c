/*
 * chan -- bring up one CD2481 channel and try a local-loopback byte.
 *
 * Run `ucode` first: the parts are dead until the microcode is downloaded
 * (§95), and `ucode` leaves them freshly Reset-All'd.
 *
 * All addresses here are the datasheet's MOTOROLA column -- IOS's download
 * routine uses that column, so the card is wired big-endian (§91).
 *
 * Local loopback is TCOR bit 1: "This register controls the transmit baud
 * rate generator and Local Loopback mode."  That matters because the CLK
 * feeding the bit-rate generators is not known -- the board has a Y1 crystal
 * and an IDT 49FCT805 buffer we have never measured, and the datasheet's own
 * 19200 example does not fit its nominal 35 MHz.  In loopback both directions
 * derive from the same clock with the same divisor, so a byte round-trips
 * whatever the crystal actually is.
 *
 *   chan [--base A] [--chip N] [--chan N] [--div D] [--byte X] [--noloop]
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* global */
#define GFRCR 0x81
#define CAR   0xEE
#define TPR   0xDA
/* per-channel */
#define CMR   0x1B
#define COR1  0x10
#define COR2  0x17
#define COR3  0x16
#define COR4  0x15
#define COR5  0x14
#define CCR   0x13
#define CSR   0x1A
#define LIVR  0x09
#define IER   0x11
#define TCOR  0xC0
#define TBPR  0xC3
#define RCOR  0xC8
#define RBPR  0xCB
#define MSVR_RTS 0xDE
#define MSVR_DTR 0xDF
/* data + service */
#define TDR   0xF8
#define RDR   0xF8
#define TFTC  0x80
#define RFOC  0x30
#define RIR   0xED
#define TIR   0xEC
#define RISRl 0x89
#define TISR  0x8A
#define REOIR 0x84
#define TEOIR 0x85

/* CCR mode 1: bit6 ClrCh, bit5 InitCh, bit4 RstAll, b3 EnTx, b2 DisTx, b1 EnRx, b0 DisRx */
#define CCR_INITCH 0x20
#define CCR_ENTX   0x08
#define CCR_ENRX   0x02

static volatile uint8_t *win;
static unsigned cbase;

static void wr(unsigned r, uint8_t v) { win[cbase + r] = v; (void)win[cbase + r]; }
static uint8_t rd(unsigned r)         { return win[cbase + r]; }

static int ccr_wait(void)
{
	int n;
	for (n = 0; n < 500000; n++)
		if (rd(CCR) == 0)
			return n;
	return -1;
}

int main(int argc, char **argv)
{
	static const unsigned chip_off[8] = {0x400,0x500,0x600,0x700,
					     0x900,0xA00,0xB00,0xC00};
	unsigned long base = 0x18000000;
	int fd, i, chip = 0, chan = 0, div = 0x81, loop = 1;
	uint8_t byte = 0x5A, got;
	int n;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--base") && i+1 < argc) base = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--chip") && i+1 < argc) chip = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--chan") && i+1 < argc) chan = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--div")  && i+1 < argc) div  = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--byte") && i+1 < argc) byte = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--noloop")) loop = 0;
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	win = mmap(NULL, 0x2000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, base);
	if (win == MAP_FAILED) { perror("mmap"); return 1; }
	cbase = chip_off[chip & 7];

	printf("chip %d @ +0x%03x, channel %d, divisor 0x%02x, loopback %s\n",
	       chip, cbase, chan, div, loop ? "ON" : "off");
	printf("  GFRCR = %02x %s\n", rd(GFRCR),
	       rd(GFRCR) == 0xE0 ? "(boot ROM -- microcode NOT loaded, run ucode first)"
				 : "(microcode revision)");

	wr(TPR, 0x40);				/* global timer prescale */
	wr(CAR, chan & 3);			/* select the channel */

	wr(LIVR, 0x40);
	wr(RCOR, 0x00);				/* clk0 = CLK/8 */
	wr(RBPR, div);
	wr(TCOR, loop ? 0x02 : 0x00);		/* bit 1 = LLM local loopback */
	wr(TBPR, div);

	wr(CMR,  0x02);				/* async, interrupt (not DMA) */
	wr(COR1, 0x17);				/* 8 bits, no parity, ignore parity */
	wr(COR2, 0x00);				/* no in-band flow control */
	wr(COR3, 0x02);				/* 1 stop bit */
	wr(COR4, 0x08);				/* FIFO threshold */
	wr(COR5, 0x00);

	wr(CCR, CCR_INITCH | CCR_ENTX | CCR_ENRX);
	n = ccr_wait();
	printf("  CCR InitCh|EnTx|EnRx: %s (%d polls)\n",
	       n >= 0 ? "accepted" : "NEVER CLEARED", n);
	if (n < 0) return 1;

	/*
	 * IER last, exactly as the datasheet's example does it.  This is not
	 * optional even though no interrupt line is wired: IER is what makes
	 * the chip assert Ten/Ren at all, and TDR/RDR are only reachable
	 * inside that service window.  With IER=0 the part sits silent and
	 * looks broken -- which is what the first run of this tool showed.
	 */
	wr(IER, 0x09);				/* bit3 RxD | bit0 TxD */

	printf("  CSR = %02x   TFTC = %02x   RFOC = %02x   IER = %02x\n",
	       rd(CSR), rd(TFTC), rd(RFOC), rd(IER));

	/* assert the modem outputs; some designs gate the datapath on them */
	wr(MSVR_RTS, 0x01);
	wr(MSVR_DTR, 0x02);

	/* transmit: wait for the chip to ask for data, then fill the FIFO */
	printf("  transmitting 0x%02x\n", byte);
	for (n = 0; n < 500000; n++) {
		uint8_t tir = rd(TIR);
		if (tir & 0x80) {
			uint8_t f0 = rd(TFTC), f1, f2;
			int k;
			printf("  tx service after %d polls: TIR=%02x TFTC=%02x "
			       "TISR=%02x\n", n, tir, f0, rd(TISR));
			/*
			 * Does the byte actually enter the FIFO?  TFTC is the
			 * free-space count, so a landed write must decrement
			 * it.  If it does not move, the data register is not
			 * reachable this way and the FPGA is gating it.
			 */
			wr(TDR, byte);
			f1 = rd(TFTC);
			for (k = 0; k < 7; k++)		/* a few more, to be sure */
				wr(TDR, byte);
			f2 = rd(TFTC);
			printf("  TFTC %02x -> %02x (after 1) -> %02x (after 8)  %s\n",
			       f0, f1, f2,
			       (f1 != f0 || f2 != f0) ? "data register ACCEPTS writes"
						      : "!! TFTC never moved");
			wr(TEOIR, 0x00);		/* Notrans=0: data was transferred */
			break;
		}
	}
	if (n == 500000)
		printf("  !! no transmit service (TIR=%02x TISR=%02x CSR=%02x)\n",
		       rd(TIR), rd(TISR), rd(CSR));

	for (n = 0; n < 500000; n++) {
		uint8_t rir = rd(RIR), rfoc = rd(RFOC);
		if ((rir & 0x80) || rfoc) {
			printf("  service after %d polls: RIR=%02x RFOC=%02x "
			       "TIR=%02x RISRl=%02x\n", n, rir, rfoc,
			       rd(TIR), rd(RISRl));
			got = rd(RDR);
			printf("  RDR = %02x  ->  %s\n", got,
			       got == byte ? "*** LOOPBACK OK -- the channel "
					     "transmits and receives ***"
					   : "byte differs");
			wr(REOIR, 0x00);
			return got == byte ? 0 : 1;
		}
	}
	printf("  no receive service seen (RIR=%02x RFOC=%02x TIR=%02x "
	       "CSR=%02x TFTC=%02x)\n", rd(RIR), rd(RFOC), rd(TIR),
	       rd(CSR), rd(TFTC));
	return 1;
}
