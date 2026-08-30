# Findings

Condensed from a much longer working log.  These are the things that were not
obvious and cost real time.

## ROMMON

* Loads an arbitrary ELF and jumps to it -- there is no secure boot on this
  generation.
* Validates `e_machine`: must be `0x0087`, not `EM_MIPS`.
* Passes **no** kernel command line, so `CONFIG_CMDLINE_BOOL` +
  `CMDLINE_OVERRIDE` are required or the kernel finds no `console=`, selects a
  dummy VGA console, and goes silent mid-boot while running perfectly.
* `boot` with no argument is ambiguous once flash holds two bootable images --
  always name the file.
* A serial **BREAK** reliably enters ROMMON within ~60 s of power-on.  A
  running IOS ignores BREAK (config-register 0x2102), but a warm `reload`
  passes back through the window.

## System controller

* It is an **MV96340** (`11ab:9634`).  Close to the MV64340 but the
  mainline glue for that family was removed from Linux long ago.
* `base_addr_enable` (`0x278`) is **inverted**: a set bit *disables* a window.
* Ethernet interrupt is cause bit 32, found by watching the register, not from
  a header.  An earlier guess of 42 produced a port that could transmit but
  never receive.

## PCI

* Nothing claims `10ee:4013`, so without a driver `pci_enable_device()` is
  never called and the card sits with memory decode off -- **the entire BAR
  reads `0xff`**.  Several early "the card is dead" conclusions were really
  this.
* MIPS defines `IO_SPACE_LIMIT` as `0xffff`.  An I/O resource larger than that
  cannot be requested, `register_pci_controller()` gives up, and you get
  `Skipping PCI bus scan due to resource conflict` -- no devices at all, not
  even the bridge's own functions.
* Declaring an I/O resource without programming the MV96340's I/O window is
  worse than declaring neither: `io_map_base` then points at an address the CPU
  does not decode and enumeration bus-errors.
* Only the **first 8 MB of DRAM** is reachable from PCI (one chip-select window
  enabled).  A bus-master DMA outside it takes a master abort and hangs the
  board with no oops and no console.

## The CD2481

* No protocol code in ROM.  Inert until the host downloads 8192 18-bit
  microcode words.  An undriven part reads exactly like RAM.
* `GFRCR` is documented **read/write**, so writing it and reading the value
  back proves nothing -- a "test" that made a live part look dead for a long
  time.  `0xE0` is silicon revision D, stated as such in the datasheet.
* Register addresses come in **Intel and Motorola** columns; this board is
  Motorola.  IOS's own download routine is what settles it.
* `RCOR` bit 7 is `TLVal`, a **read-only** view of the transmit pin -- not
  configuration.  It is also an excellent probe: it shows whether anything is
  leaving the transmitter without needing the receiver to work.
* ClkSel is in **different bits** in `TCOR` (7:5) and `RCOR` (2:0).  Writing
  the same value to both gives transmit and receive different rates.
* Data registers are **Virtual registers**, reachable only inside an interrupt
  context entered by the acknowledge cycle.  On this card the acknowledge is
  `read BAR+0`.
* **One acknowledge grants one service and the chip chooses which** -- receive,
  then transmit, then modem.  Acknowledging twice in a pass corrupts both
  directions.
* `TDR` and `RDR` share address `0xF8`.  A "flush" read-back after writing a
  register therefore reads the *receive* register after every transmitted
  character, injecting bytes.  PCI writes to one device are already ordered;
  the read-back was never needed.

## Method notes

Four separate wrong turns had the same shape: a plausible value at a plausible
offset, accepted without checking a primary source for what it meant.  The
datasheet contained the answer each time.  In particular:

* Reading one register's layout and assuming the neighbouring register matches
  it accounts for three of the bugs above.
* A detector that assumes what the hardware *returns* can be blind to the
  answer: a sweep for a vector value read the correct address and discarded it,
  while a sweep for a *behaviour* (`Ten` clearing) found it immediately.
* A measurement that disagrees with itself is measuring the wrong thing.
  Timing a 200-character burst gave five different clock frequencies; timing
  one character gave the right one.
* Values captured after a reset may be reset defaults rather than the running
  configuration.  Thirty-two identical register lines on a card with one
  configured port should have been read as "these are defaults", not as
  corroboration.
