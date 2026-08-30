/*
 * ucode -- download the CD2481 microcode on the NM-32A.
 *
 * The CD2481 ships with no protocol code.  Its on-chip ROM only initialises
 * hardware and clears the RAM-based register file, which is exactly the
 * behaviour we measured for months: GFRCR stays writable, CCR never
 * self-clears, the register window behaves as plain RAM.  The chips were not
 * broken and not mis-addressed -- they were empty.
 *
 * The datasheet's chapter 6 gives the download procedure, and IOS's
 * 0x4035f5ac is the same procedure compiled for this board.  Its register
 * table has two columns, INT and MOT, for the two byte orders:
 *
 *      register                          INT   MOT
 *      AIRl  aux instruction low         F0    F2
 *      AIRm  aux instruction middle      F1    F3
 *      AIRh  aux instruction high        F2    F0
 *      MTCR  MPU test control            F3    F1
 *      BTCR  BIL test control            F4    F6
 *
 * IOS writes F2/F3/F0/F1/F6, so this board is wired for the MOT mapping.
 *
 * Instructions are 18 bits.  The blob is a continuous little-endian bit
 * stream -- IOS unpacks four of them per nine bytes (`addiu s0,s0,9` at
 * 0x4035f740), and tops the 8K-word store up to 8192 with 0x1E000, which is
 * also what the blob's own tail is padded with.
 *
 *   ucode <file> [--base 0x18000000] [--chip N] [--ds] [--verify]
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define AIRL 0xF2
#define AIRM 0xF3
#define AIRH 0xF0
#define MTCR 0xF1
#define BTCR 0xF6
#define GFRCR 0x81
#define CCR   0x13		/* Motorola address; Intel would be 0x10 */
#define CCR_RSTALL 0x10		/* bit 4 */

#define NCHIP 8
static const unsigned chip_off[NCHIP] = {
	0x400, 0x500, 0x600, 0x700, 0x900, 0xA00, 0xB00, 0xC00
};

static volatile uint8_t *win;		/* mapped BAR */
static unsigned cbase;			/* current chip's offset */
static unsigned lane;			/* byte-lane XOR, see --xor */
static unsigned RA, RM, RH, RMT, RBT;	/* register numbers, INT or MOT */

/*
 * The CPU is big-endian and PCI is little-endian, so a byte offset on this
 * bus does not necessarily reach the byte lane the CD2481 calls that
 * register.  --xor 3 is the usual 32-bit swap, --xor 1 the 16-bit one.  This
 * is the same ambiguity that made section 72's 0xE0 unreadable.
 */
static void wr(unsigned reg, uint8_t v)
{
	win[cbase + (reg ^ lane)] = v;
	(void)win[cbase + (reg ^ lane)];	/* push it out */
}

static uint8_t rd(unsigned reg)
{
	return win[cbase + (reg ^ lane)];
}

/* one 18-bit instruction into the store: load AIR, then single-step */
static void put(uint32_t insn)
{
	wr(RA, insn & 0xff);
	wr(RM, (insn >> 8) & 0xff);
	wr(RH, (insn >> 16) & 0x03);
	wr(RBT, 0x03);			/* ClkDis | SnglStp -- store to RAM */
}

/* jump 0x3fff, the instruction both setup and start park the MPU on */
static void jump_3fff(void)
{
	wr(RA, 0xff);
	wr(RM, 0x3f);
	wr(RH, 0x03);
}

static void dl_setup(int datasheet)
{
	if (datasheet) {
		wr(RBT, 0x01);		/* stop the clock */
		jump_3fff();
		wr(RMT, 0x01);		/* select auxiliary instruction */
		wr(RBT, 0x03);		/* single step twice */
		wr(RBT, 0x03);
	} else {			/* what IOS actually does */
		jump_3fff();
		wr(RMT, 0x01);
		wr(RBT, 0x01);
	}
	wr(RMT, 0x02);			/* data write mode -- download */
}

static void dl_start(void)
{
	jump_3fff();
	wr(RMT, 0x01);			/* select auxiliary instruction */
	wr(RBT, 0x03);			/* single step twice */
	wr(RBT, 0x03);
	wr(RMT, 0x03);			/* execute from RAM store */
	wr(RBT, 0x00);			/* clock on -- the chip is now alive */
}

/* the i'th 18-bit instruction of a little-endian bit stream */
static uint32_t insn_at(const uint8_t *b, size_t len, unsigned i)
{
	unsigned bit = i * 18, byte = bit >> 3, k;
	uint32_t v = 0;

	for (k = 0; k < 3; k++)
		if (byte + k < len)
			v |= (uint32_t)b[byte + k] << (8 * k);
	return (v >> (bit & 7)) & 0x3FFFF;
}

int main(int argc, char **argv)
{
	unsigned long base = 0x18000000;
	const char *path = NULL;
	int fd, i, only = -1, datasheet = 0, verify = 0, use_int = 0;
	uint8_t *blob;
	long flen;
	unsigned ninsn;
	FILE *f;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--base") && i + 1 < argc)
			base = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--chip") && i + 1 < argc)
			only = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ds")) datasheet = 1;
		else if (!strcmp(argv[i], "--xor") && i + 1 < argc)
			lane = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--int")) use_int = 1;
		else if (!strcmp(argv[i], "--verify")) verify = 1;
		else path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: ucode <file> [--base A] [--chip N] [--ds]"
				" [--verify] [--xor N] [--int]\n");
		return 2;
	}

	/* INT and MOT are the datasheet's two byte-order register mappings */
	if (use_int) { RA = 0xF0; RM = 0xF1; RH = 0xF2; RMT = 0xF3; RBT = 0xF4; }
	else         { RA = AIRL; RM = AIRM; RH = AIRH; RMT = MTCR; RBT = BTCR; }

	f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	fseek(f, 0, SEEK_END); flen = ftell(f); rewind(f);
	blob = malloc(flen);
	if (!blob || fread(blob, 1, flen, f) != (size_t)flen) {
		fprintf(stderr, "short read\n"); return 1;
	}
	fclose(f);
	ninsn = (unsigned)(flen * 8 / 18);
	if (ninsn > 8192) ninsn = 8192;
	printf("microcode: %ld bytes, %u instructions (first %05X)"
	       "  [%s map, lane xor %u]\n", flen, ninsn, insn_at(blob, flen, 0),
	       use_int ? "INT" : "MOT", lane);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	win = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (win == MAP_FAILED) { perror("mmap"); return 1; }

	for (i = 0; i < NCHIP; i++) {
		uint8_t before, after, ccr;
		unsigned n;

		if (only >= 0 && i != only) continue;
		cbase = chip_off[i];
		before = rd(GFRCR);

		dl_setup(datasheet);
		for (n = 0; n < ninsn; n++)
			put(insn_at(blob, flen, n));
		for (; n < 8192; n++)		/* IOS pads the rest of the store */
			put(0x1E000);
		dl_start();

		/*
		 * Datasheet 8.1: after the download, issue Reset All through
		 * CCR and wait for GFRCR to go non-zero again.  The CD2481
		 * zeroes CCR once it has acted on a command, so CCR clearing
		 * is the real proof that the MPU is executing -- a dead part
		 * leaves whatever the host wrote sitting there.
		 */
		wr(CCR, CCR_RSTALL);
		for (n = 0, ccr = 0xff; n < 200000; n++)
			if ((ccr = rd(CCR)) == 0)
				break;
		for (n = 0; n < 200000; n++) {
			after = rd(GFRCR);
			if (after && after != 0xff)
				break;
		}
		printf("chip %d @ +0x%03x: GFRCR %02x -> %02x   CCR after RstAll"
		       " %02x %s\n", i, chip_off[i], before, after, ccr,
		       ccr == 0 ? "(self-cleared -- MPU EXECUTING)"
				: "(never cleared -- MPU dead)");

		if (verify) {
			/*
			 * NOT a write test.  GFRCR is documented "Access: Byte
			 * Read/Write", so writing it and reading the value back
			 * proves nothing at all -- that bogus check is what made
			 * §72 and every run before §95 read a live part as dead.
			 *
			 * The real evidence is above: E0 is the silicon revision
			 * (the datasheet says "Revision D is E0 hex"), the value
			 * that replaces it is the downloaded firmware's own
			 * revision, and CCR only self-clears once the MPU runs.
			 */
			printf("           %s\n",
			       before == 0xE0 ? "silicon: CD2481 revision D"
					      : "silicon: unexpected boot revision");
		}
	}
	return 0;
}
