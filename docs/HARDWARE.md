# The Cisco 2811, as seen from Linux

Everything here was established by measurement on the board or read out of
ROMMON/IOS, not from Cisco documentation, which does not exist publicly for
this platform.

## The machine

| | |
|---|---|
| CPU | PMC-Sierra RM5261A-350H, MIPS IV "Nevada", `PRId 0x2831`, big-endian |
| System controller | Marvell **MV96340** (`11ab:9634`) -- not the MV64340 |
| DRAM | 256 MB at physical 0 |
| Console | 16550 at `0x1FA80000` (a second at `0x1FA80040`) |
| Flash | CompactFlash, ATA on device chip-select DevCS3, `0x1FB00000` |
| Boot | stock ROMMON 12.4(13r)T, loads an arbitrary ELF and jumps to it |

ROMMON validates `e_machine`: it must be `0x0087`, not `EM_MIPS` (8), or you
get `loadprog: error - Invalid image for platform`.  `tools/set_e_machine.py`
stamps a built kernel.

## Physical memory map

```
  0x00000000  +---------------------------------------------+
              |  DRAM, 256 MB                               |
              |                                             |
              |   0x00600000  2 MB reserved for card DMA    |  <-- memblock_reserve
              |               (only the low 8 MB is         |      in setup.c
              |                reachable from PCI)          |
  0x10000000  +---------------------------------------------+
              |  PCI0 memory window                         |
  0x14000000  +---------------------------------------------+
              |  MV96340 internal registers (64 KB)         |
  0x18000000  +---------------------------------------------+
              |  PCI1 memory window, 64 MB                  |
              |   0x18000000  NM-32A BAR0 (1 MB)            |
  0x1C000000  +---------------------------------------------+
              |  PCI1 I/O window, 1 MB (see note)           |
  0x1FA80000  +---------------------------------------------+
              |  console UART / second UART at +0x40        |
  0x1FB00000  +---------------------------------------------+
              |  CompactFlash task file (DevCS3)            |
              +---------------------------------------------+
```

The PCI1 I/O window is programmed but the I/O *resource* is left empty --
see `docs/FINDINGS.md`, the NM-32A's I/O BAR turned out to be vestigial and
declaring the resource without a decoding window aborts the whole PCI scan.

## Interrupts

The MV96340 aggregates everything into two 64-bit cause registers.  Bit
numbering is not in any header we have; `CONFIG_CISCO2811_ETH_IRQ_BIT` was
found by watching the cause register while the port was pinged.  Bit 32 is
FastEthernet0/0.

The NM-32A's interrupt is **not** wired up: the driver polls.  See
`docs/NM32A.md`.

Both onboard Ethernet ports are registered, `eth0` and `eth1`, on PHY addresses
1 and 2.  Only `eth0`'s interrupt bit is confirmed by measurement; `eth1` uses
the next bit by analogy with the old PowerPC mv64x60 numbering and has never
been cabled here.  A wrong bit shows up as a port that transmits but never
receives -- which is exactly what an earlier wrong guess did to `eth0`.

## The chassis, and where the console lives

```
        +-------------------------------------------------------+
        |  Cisco 2811                                           |
        |                                                       |
        |   CON  o------------------------------------------- RJ45 console,
        |        (16550 at 0x1FA80000, 9600 8N1)                 9600 8N1
        |                                                       |
        |   Fa0/0 o----------------------------------------- 10/100  eth0
        |   Fa0/1 o----------------------------------------- 10/100  eth1
        |        (MV96340 MACs, mv643xx_eth; PHY 1 and 2)         |
        |                                                       |
        |   +-----------------------------------------------+   |
        |   |  NM slot 1                                    |   |
        |   |    NM-32A, 32 async ports  (see NM32A.md)     |   |
        |   +-----------------------------------------------+   |
        |                                                       |
        |   CompactFlash (front)  --  IOS image + our kernel    |
        +-------------------------------------------------------+
```
