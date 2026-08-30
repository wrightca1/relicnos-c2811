/*
 * poke -- write arbitrary physical memory, 8/16/32-bit.
 *
 * Counterpart to peek, for driver bring-up.  The NM-32A's CD2430s read
 * GFRCR = 0 from a cold boot, meaning they have not finished self-initialising;
 * IOS's 0x4035743c only WAITS for that (3 s timeout), so the release happens
 * elsewhere.  The board register at BAR+0x800 reads 0xffff after IOS has run and
 * 0xfeff after a plain relicnos boot -- bit 8 differs -- which makes it the first
 * thing to try.
 *
 *   poke <phys> <value> [-b|-w|-l]      default 32-bit
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	unsigned long phys, val, page, off;
	int fd, i, width = 4;
	volatile uint8_t *m;

	if (argc < 3) {
		fprintf(stderr, "usage: poke <phys> <value> [-b|-w|-l]\n");
		return 2;
	}
	phys = strtoul(argv[1], NULL, 0);
	val  = strtoul(argv[2], NULL, 0);
	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-b")) width = 1;
		else if (!strcmp(argv[i], "-w")) width = 2;
		else if (!strcmp(argv[i], "-l")) width = 4;
	}
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	page = phys & ~0xfffUL;
	off  = phys & 0xfff;
	m = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }

	if (width == 1) {
		printf("%08lx: %02x -> ", phys, *(volatile uint8_t *)(m + off));
		*(volatile uint8_t *)(m + off) = (uint8_t)val;
		printf("%02x\n", *(volatile uint8_t *)(m + off));
	} else if (width == 2) {
		printf("%08lx: %04x -> ", phys, *(volatile uint16_t *)(m + off));
		*(volatile uint16_t *)(m + off) = (uint16_t)val;
		printf("%04x\n", *(volatile uint16_t *)(m + off));
	} else {
		printf("%08lx: %08x -> ", phys, *(volatile uint32_t *)(m + off));
		*(volatile uint32_t *)(m + off) = (uint32_t)val;
		printf("%08x\n", *(volatile uint32_t *)(m + off));
	}
	return 0;
}
