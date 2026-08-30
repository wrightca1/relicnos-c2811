// SPDX-License-Identifier: MIT
/*
 * peek -- read arbitrary physical memory, 8/16/32-bit.
 *
 * Needed to reach the NM-32A's register file once its BAR is assigned.  IOS
 * reads the card with 16-bit accesses (`lhu a3,2048(s4)` at 0x4035dc28 decides
 * 16 vs 32 ports), so halfword access matters, and busybox here has no devmem.
 *
 *   peek <phys> [count] [-b|-w|-l]     default: 16 x 32-bit
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
	unsigned long phys, page, off;
	int fd, i, n = 16, width = 4;
	volatile uint8_t *m;

	if (argc < 2) { fprintf(stderr, "usage: peek <phys> [count] [-b|-w|-l]\n"); return 2; }
	phys = strtoul(argv[1], NULL, 0);
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-b")) width = 1;
		else if (!strcmp(argv[i], "-w")) width = 2;
		else if (!strcmp(argv[i], "-l")) width = 4;
		else n = atoi(argv[i]);
	}
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	page = phys & ~0xfffUL;
	off  = phys & 0xfff;
	m = mmap(NULL, 0x2000, PROT_READ, MAP_SHARED, fd, page);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }

	for (i = 0; i < n; i++) {
		unsigned long a = off + (unsigned long)i * width;

		if (i % (width == 4 ? 4 : 8) == 0)
			printf("\n%08lx: ", phys + (unsigned long)i * width);
		if (width == 1)      printf("%02x ", *(volatile uint8_t  *)(m + a));
		else if (width == 2) printf("%04x ", *(volatile uint16_t *)(m + a));
		else                 printf("%08x ", *(volatile uint32_t *)(m + a));
	}
	putchar('\n');
	return 0;
}
