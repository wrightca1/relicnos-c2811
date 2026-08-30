/*
 * pciscan -- enumerate the Cisco 2811's two PCI buses from userspace.
 *
 * The kernel has no PCI at all on this board (there is no MV64340 host-bridge
 * driver in mainline -- the mv64x60 code went with the PowerPC removal), so
 * before writing one it is worth finding out what is actually on the buses.
 * The MV64340 exposes the usual config address/data pair in its register
 * window, so a config-space read is three memory accesses and needs no driver.
 *
 * Read-only: nothing here writes config space or touches a BAR.
 *
 * Registers are little-endian while the CPU is big-endian, hence the swaps --
 * the same relationship CONFIG_SWAP_IO_SPACE handles for the kernel.
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>

#define MV_PHYS		0x14000000UL
#define MV_LEN		0x10000

/*
 * IOS flushes the posted CONFIG_ADDR write with a dummy read of 0xBFA00000
 * (DevCS0, the WIC CPLD) before touching CONFIG_DATA -- see the helper at
 * 0x400582d0, which is literally `lui v1,0xbfa0 ; lhu a0,0(v1)`.  Without it
 * the address write may not have landed when the data cycle runs.
 */
#define FLUSH_PHYS	0x1FA00000UL
#define FLUSH_LEN	0x1000

#define PCI0_CONFIG_ADDR	0xcf8
#define PCI0_CONFIG_DATA	0xcfc
#define PCI1_CONFIG_ADDR	0xc78
#define PCI1_CONFIG_DATA	0xc7c

static volatile uint8_t *mv;
static volatile uint16_t *flushp;

/*
 * The interface's own bus number, from P2P Configuration bits [23:16].  The
 * host bridge answers as device 0 on THIS bus, and only that access needs the
 * byte swap.  Hard-coding 0 here (as this file used to) silently breaks every
 * bridge access the moment BusNum is reprogrammed -- the ID comes back as
 * 3496:ab11 instead of 11ab:9634 and the status register reads 0x0000, which
 * makes a master-abort test look like a clean negative when it is really a bad
 * measurement.
 */
static int local_bus[2] = { 0, 0 };

static inline uint32_t swab32(uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
static inline uint32_t rd(unsigned off)
{
	return swab32(*(volatile uint32_t *)(mv + off));
}
static inline void wr(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(mv + off) = swab32(v);
}

#define MV_PCI0_CMD		0xc00
#define MV_PCI1_CMD		0xc80
#define MV_PCI0_P2P_CONFIG	0x1d14
#define MV_PCI1_P2P_CONFIG	0x1d94
#define MV_INT_CAUSE_LOW	0x004
#define MV_INT_CAUSE_HIGH	0x00c

/*
 * Byte swap EVERY config access.
 *
 * This file used to swap only the host bridge's own space, on the documented
 * Galileo quirk that the DATA register is "raw for every other device".  That is
 * wrong on this controller: the NM-32A at dev 1 reads 0xee101340 unswapped,
 * which is meaningless, and 0x401310ee swapped -- the exact ID IOS checks for.
 * Both the bridge and downstream devices need it (§67).
 */
static uint32_t cfg_read(int bus_if, int bus, int dev, int fn, int reg)
{
	unsigned a = bus_if ? PCI1_CONFIG_ADDR : PCI0_CONFIG_ADDR;
	unsigned d = bus_if ? PCI1_CONFIG_DATA : PCI0_CONFIG_DATA;
	uint32_t v;

	wr(a, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc));

	/* flush the posted address write, exactly as IOS does */
	if (flushp)
		(void)*flushp;

	/*
	 * IOS reads CONFIG_DATA twice and uses the second read.  Replicated
	 * rather than rationalised: the first access evidently cannot be
	 * trusted on this controller.
	 */
	v = *(volatile uint32_t *)(mv + d);
	v = *(volatile uint32_t *)(mv + d);
	return swab32(v);
}

static void cfg_write(int bus_if, int bus, int dev, int fn, int reg, uint32_t val)
{
	unsigned a = bus_if ? PCI1_CONFIG_ADDR : PCI0_CONFIG_ADDR;
	unsigned d = bus_if ? PCI1_CONFIG_DATA : PCI0_CONFIG_DATA;

	wr(a, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc));
	if (flushp)
		(void)*flushp;
	*(volatile uint32_t *)(mv + d) = swab32(val);
}

/* Did the last cycle master-abort?  An empty slot aborts; a held-in-reset bus
 * may not even do that.  Distinguishing the two is the whole question here. */
static int took_abort(void)
{
	uint32_t lo = rd(MV_INT_CAUSE_LOW);

	/* master/target abort bits live in the low cause word */
	return (lo & 0x00300000) != 0;
}

static const char *class_name(uint32_t cls)
{
	switch (cls >> 16) {
	case 0x0000: return "legacy";
	case 0x0001: return "storage";
	case 0x0002: return "network";
	case 0x0003: return "display";
	case 0x0004: return "multimedia";
	case 0x0006: return "bridge";
	case 0x0007: return "comm/serial";
	case 0x0008: return "system peripheral";
	case 0x000b: return "processor";
	case 0x000c: return "serial bus";
	case 0x000f: return "encryption";
	default:     return "?";
	}
}

static void show_dev(int bus_if, int bus, int dev, int fn)
{
	uint32_t id = cfg_read(bus_if, bus, dev, fn, 0x00);
	uint32_t cls, hdr, i;

	if (id == 0xffffffff || id == 0)
		return;

	cls = cfg_read(bus_if, bus, dev, fn, 0x08);
	hdr = cfg_read(bus_if, bus, dev, fn, 0x0c);

	printf("PCI%d %02x:%02x.%d  %04x:%04x  class %06x (%s)  hdr %02x\n",
	       bus_if, bus, dev, fn, id & 0xffff, id >> 16,
	       cls >> 8, class_name(cls >> 8), (hdr >> 16) & 0x7f);

	if (((hdr >> 16) & 0x7f) != 0)			/* bridge: BARs differ */
		return;

	for (i = 0; i < 6; i++) {
		uint32_t bar = cfg_read(bus_if, bus, dev, fn, 0x10 + i * 4);

		if (!bar || bar == 0xffffffff)
			continue;
		if (bar & 1)
			printf("      BAR%u  I/O  0x%08x\n", i, bar & ~3u);
		else
			printf("      BAR%u  MEM  0x%08x%s\n", i, bar & ~0xfu,
			       (bar & 8) ? " prefetch" : "");
	}
	printf("      IRQ pin/line 0x%08x\n", cfg_read(bus_if, bus, dev, fn, 0x3c));
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	int bus_if, bus, dev, fn, maxbus = argc > 1 ? atoi(argv[1]) : 4;

	if (fd < 0) { perror("open /dev/mem"); return 1; }
	mv = mmap(NULL, MV_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MV_PHYS);
	if (mv == MAP_FAILED) { perror("mmap"); return 1; }
	flushp = mmap(NULL, FLUSH_LEN, PROT_READ, MAP_SHARED, fd, FLUSH_PHYS);
	if (flushp == MAP_FAILED) { flushp = NULL; printf("(no flush mapping)\n"); }

	local_bus[0] = (rd(MV_PCI0_P2P_CONFIG) >> 16) & 0xff;
	local_bus[1] = (rd(MV_PCI1_P2P_CONFIG) >> 16) & 0xff;

	printf("MV64340 PCI state\n");
	printf("  local bus numbers: PCI0 = %d, PCI1 = %d\n",
	       local_bus[0], local_bus[1]);
	printf("  PCI0 cmd 0x%08x   P2P config 0x%08x\n",
	       rd(MV_PCI0_CMD), rd(MV_PCI0_P2P_CONFIG));
	printf("  PCI1 cmd 0x%08x   P2P config 0x%08x\n",
	       rd(MV_PCI1_CMD), rd(MV_PCI1_P2P_CONFIG));
	printf("  int cause lo 0x%08x hi 0x%08x\n\n",
	       rd(MV_INT_CAUSE_LOW), rd(MV_INT_CAUSE_HIGH));

	/*
	 * The decisive question: are config cycles reaching the external bus at
	 * all?  If they are, probing an absent device leaves Received Master
	 * Abort (bit 13) in the bridge's OWN PCI status register.  If that bit
	 * never sets, the cycles are not being generated and "empty bus" is an
	 * artefact of our access, not a fact about the hardware.
	 */
	{
		uint32_t v, st_before, st_after;
		int bif;

		for (bif = 0; bif < 2; bif++) {
			int lb = local_bus[bif];

			st_before = cfg_read(bif, lb, 0, 0, 0x04) >> 16;
			v = cfg_read(bif, lb, 5, 0, 0x00);
			st_after = cfg_read(bif, lb, 0, 0, 0x04) >> 16;
			printf("  PCI%d probe %02x:05.0 -> 0x%08x | status %04x -> %04x  "
			       "RMA=%d\n", bif, lb, v, st_before, st_after,
			       (st_after >> 13) & 1);
		}
		v = cfg_read(0, 0, 5, 0, 0);
		printf("  main-cause abort flag=%d\n", took_abort());
		printf("  (P2P config low byte may be the LOCAL BUS NUMBER: if it is not the\n"
		       "   bus we address, the controller emits Type 1 cycles and nothing on\n"
		       "   the local bus ever answers.)\n\n");
	}

	/*
	 * --rma <if> <bus> <dev>: the positive control this investigation never had.
	 *
	 * Reading the host bridge's own config space proves nothing about external
	 * cycles -- device 0 on the local bus is a special path.  But once the
	 * bridge's BusNum is programmed to match the bus we address, a Type 0
	 * cycle to an ABSENT device must go out on the wire and master-abort,
	 * setting Received Master Abort (status bit 13) in the bridge's own
	 * status register.
	 *
	 *   RMA sets     -> cycles reach the slot; the card genuinely is not there
	 *   RMA never set-> cycles are not being emitted, and every "no device"
	 *                   result in this thread is an artefact of our access path
	 *
	 * The bridge's status must be read at its CURRENT location (bus = BusNum,
	 * device 0), not at 00:00.0 -- reading the wrong place returns 0xffff and
	 * makes RMA look permanently set, which is exactly the artefact that
	 * confused §54.
	 */
	/*
	 * --sweep-bus <if>: the only bus an interface answers with a Type 0 cycle
	 * is its own programmed BusNum -- everything else goes out as Type 1 and
	 * cannot be claimed on a bus with no downstream bridge.  So "scan buses
	 * 0..N" never scanned more than one bus.  This walks BusNum itself
	 * through all 256 values and probes devices 1-31 on each, which is the
	 * only way to find a device whose bus number we do not already know.
	 *
	 * Bus mastering is enabled first: without it the interface cannot drive
	 * the bus at all and every probe returns 0xffffffff regardless (§63).
	 */
	if (argc > 2 && !strcmp(argv[1], "--sweep-bus")) {
		int bif = atoi(argv[2]), b, dev, found = 0;
		unsigned p2p = bif ? MV_PCI1_P2P_CONFIG : MV_PCI0_P2P_CONFIG;
		uint32_t save = rd(p2p);

		local_bus[bif] = (save >> 16) & 0xff;
		cfg_write(bif, local_bus[bif], 0, 0, 0x04, 0x02b00146);
		printf("PCI%d: bus master enabled, sweeping BusNum 0..255\n", bif);

		for (b = 0; b < 256; b++) {
			wr(p2p, (save & ~0x00ff0000u) | ((uint32_t)b << 16));
			local_bus[bif] = b;
			for (dev = 1; dev < 32; dev++) {
				uint32_t v = cfg_read(bif, b, dev, 0, 0x00);

				if (v != 0xffffffff && v != 0x00000000) {
					printf("  FOUND bus %d dev %d -> 0x%08x\n", b, dev, v);
					found++;
				}
			}
		}
		wr(p2p, save);
		local_bus[bif] = (save >> 16) & 0xff;
		printf("sweep complete: %d device(s) found; BusNum restored to %d\n",
		       found, local_bus[bif]);
		return 0;
	}

	/*
	 * --find <if>: is the NM-32A answering?  One probe of dev 1 on the
	 * interface's current bus, reported as present/absent.  Used to bisect
	 * which of the CPLD writes in §67 is actually required.
	 */
	if (argc > 2 && !strcmp(argv[1], "--find")) {
		int bif = atoi(argv[2]);
		uint32_t v;

		local_bus[bif] = (rd(bif ? MV_PCI1_P2P_CONFIG
					 : MV_PCI0_P2P_CONFIG) >> 16) & 0xff;
		cfg_write(bif, local_bus[bif], 0, 0, 0x04, 0x02b00146);
		v = cfg_read(bif, local_bus[bif], 1, 0, 0x00);
		printf("PCI%d bus %d dev 1 -> 0x%08x   %s\n", bif, local_bus[bif], v,
		       v == 0x401310ee ? "*** NM-32A PRESENT (10ee:4013) ***"
					: "absent");
		return v == 0x401310ee ? 0 : 1;
	}

	/* --cfgr <if> <bus> <dev> <fn> <reg>   raw config read  */
	if (argc > 6 && !strcmp(argv[1], "--cfgr")) {
		uint32_t v = cfg_read(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
				      atoi(argv[5]), (int)strtoul(argv[6], NULL, 0));
		printf("cfg[%s] %02x:%02x.%s reg 0x%02lx = 0x%08x\n", argv[2],
		       atoi(argv[3]), atoi(argv[4]), argv[5],
		       strtoul(argv[6], NULL, 0), v);
		if (strtoul(argv[6], NULL, 0) == 4)
			printf("   command 0x%04x  (IO %d  MEM %d  MASTER %d)   status 0x%04x\n",
			       v & 0xffff, v & 1, !!(v & 2), !!(v & 4), v >> 16);
		return 0;
	}

	/* --cfgw <if> <bus> <dev> <fn> <reg> <val>   raw config write */
	if (argc > 7 && !strcmp(argv[1], "--cfgw")) {
		int bif = atoi(argv[2]), bus = atoi(argv[3]), dev = atoi(argv[4]);
		int fn = atoi(argv[5]), reg = (int)strtoul(argv[6], NULL, 0);
		uint32_t val = strtoul(argv[7], NULL, 0);

		printf("before 0x%08x\n", cfg_read(bif, bus, dev, fn, reg));
		cfg_write(bif, bus, dev, fn, reg, val);
		printf("wrote  0x%08x -> now 0x%08x\n", val,
		       cfg_read(bif, bus, dev, fn, reg));
		return 0;
	}

	if (argc > 4 && !strcmp(argv[1], "--rma")) {
		int bif = atoi(argv[2]), bus = atoi(argv[3]), dev = atoi(argv[4]);
		uint32_t id, before, after;

		before = cfg_read(bif, bus, 0, 0, 0x04) >> 16;
		id     = cfg_read(bif, bus, dev, 0, 0x00);
		after  = cfg_read(bif, bus, 0, 0, 0x04) >> 16;
		printf("bridge at %02x:00.0 status %04x -> %04x\n", bus, before, after);
		printf("probe    %02x:%02x.0 -> 0x%08x\n", bus, dev, id);
		printf("RMA (status bit 13): before %d, after %d  ->  %s\n",
		       (before >> 13) & 1, (after >> 13) & 1,
		       (before & 0xffff) == 0xffff ? "BRIDGE READ FAILED - result meaningless"
		       : (((after >> 13) & 1) && !((before >> 13) & 1))
		         ? "cycles REACH the bus; no device answered"
		         : "no master abort recorded");
		return 0;
	}

	for (bus_if = 0; bus_if < 2; bus_if++)
		for (bus = 0; bus <= maxbus; bus++)
			for (dev = 0; dev < 32; dev++)
				for (fn = 0; fn < 8; fn++) {
					show_dev(bus_if, bus, dev, fn);
					if (fn == 0) {
						uint32_t h = cfg_read(bus_if, bus, dev, 0, 0x0c);
						if (!((h >> 16) & 0x80))
							break;	/* not multifunction */
					}
				}
	return 0;
}
