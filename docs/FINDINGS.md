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

## Reading a live console end to end

The card now reads an attached 115200 console cleanly and repeatably -- a full
U-Boot sequence, then a login prompt, with no dropped or duplicated characters.
Getting there needed four more fixes, three of them in the interrupt protocol
and one nowhere near the card.

**The EOIR must match the interrupt type acknowledged.** The service routine
ended every unrecognised service with `TEOIR`.  A *receive* service taking that
path left the receive context open for ever; the next acknowledge nested onto a
context that never ended, the chip stopped answering, and the CPU stalled inside
the read with interrupts off.  No oops, no console output, no softlockup report
-- just a board that stops.

**The context must be confirmed closed.** IOS writes the EOIR, then reads `STK`
(the interrupt nesting stack, bits 7/0 = the active level) and re-issues the
write until the level pops, up to 100 times.  The driver now does the same.

**A receive TIMEOUT is not an error.** The datasheet (5.3.5) is explicit that the
host is expected to collect the characters still in the FIFO.  Discarding it
stranded the tail of every burst shorter than the FIFO threshold, so a
23-character prompt arrived as exactly 16 bytes.

**COR6/COR7/STCR must be written, not inherited.** IOS loads a full channel
image; this driver set none of the three, so a channel kept whatever the previous
owner left.  In async mode COR6 is the UNIX-tty helper -- `IgnCR`/`ICRNL`/`INLCF`
translate or drop CR and NL, and `ParMrk` prefixes an errored character with
`FF 00` -- and COR7 strips the eighth bit.  Any of those turns clean console text
into convincing garbage that reads like a baud error.

## The dropped characters were not the card

A prompt still arrived truncated, and the cause was in the kernel configuration:

    CONFIG_HZ=250, CONFIG_HZ_PERIODIC=y, no CONFIG_HIGH_RES_TIMERS

Without high-resolution timers, `usleep_range(500, 1000)` cannot sleep less than
one jiffy -- **4 ms**, whatever range is requested.  At 115200 that is roughly 46
characters against a 16-byte FIFO, so a burst overran before the poll thread ever
looked at it.  The "500-1000 us" in that call was never what the machine did.

Replacing it with `udelay()` + `cond_resched()` made the dependency measurable.
Swept against the driver's own overrun counter:

| poll interval | bytes of a 23-byte prompt | overruns |
|---------------|---------------------------|----------|
| 200 us        | 18                        | 1        |
| 100 us        | 18                        | 1        |
| **50 us**     | **23**                    | **0**    |
| 20 us         | 23                        | 0        |

Six consecutive trials at 50 us: 6/6 complete, zero overruns.

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
* **A dead link and a broken driver look identical from one end.**  Much of this
  work chased a fault that was not in the code: the attached console stopped
  being driven partway through, and every measurement afterwards pointed at the
  driver.  Two things settled it -- the receive divisor became *irrelevant*
  across an 18x sweep (no timing error is invariant across 18x), and stock
  vendor firmware failed identically on the same port it had read an hour
  earlier.  When the reference implementation fails the same way, the fault is
  not in your reimplementation.
* **Instrumentation that costs time changes the answer.**  `printk` on a 9600
  console is about 1 ms a line, which slows a poll loop enough to *cause* the
  overruns it is meant to observe.  That produced two false results here: a
  timing bug that vanished under tracing, and an overrun that only existed while
  tracing.  The fix was to count in the driver and read the counters afterwards.
  Where the measurement must live inside the thing being measured, make
  recording free and reading expensive, not the other way round.
* **Check the platform's assumptions, not just the device's.**  Four register
  theories were built on top of a sleep that was eight times longer than the
  code requested.

## Hanging up, and what the far end does with a BREAK

The driver raised DTR/RTS at channel init and never lowered them, so closing a
console session signalled nothing to the attached device.  A getty resets and
reprints its login prompt when DTR drops; ours never dropped, so a stale session
on the far end looked like the console needing a power cycle to recover.
`->dtr_rts` is implemented now and the last close lowers both lines.

That cannot help a device on a three-wire console cable (TX/RX/ground, no modem
leads) -- the vendor firmware reports `noCTS noDSR` on such a port and DTR never
reaches the far end.  There, a newline is the recovery.

Worth knowing before debugging a live console: **a getty commonly cycles through
a list of speeds on BREAK.**  An attached device was found running at 57600 while
its own `/proc/cmdline` said `console=ttyS0,115200`; nothing had been
reconfigured.  Every mis-framed burst from this driver, while its receive path
was still wrong, looked like a BREAK, and the getty stepped down its list.
Emitting breaks has that cost.

## The second Ethernet port

`eth1`'s interrupt bit was a reasoned guess -- bit 32 was measured for port 0,
and port 1 predicted as the next bit.  With the port finally cabled it
negotiates 100 Mb/s full duplex and carries IPv4 and IPv6 in both directions, so
the prediction holds.

One trap: with the interface up and the far end quiet, `rx_packets` sat at 0 and
`rx_errors` at 0, which is indistinguishable from a wrong receive interrupt.  A
switch port will hold carrier while forwarding nothing at all.  Pinging the
gateway settled it in one command -- an idle counter is not a broken one.

## Sending a break

A break is how you reach a Cisco supervisor's `rommon>` prompt, and how you
unstick a getty that has stopped listening.  Two pieces were missing:

The CD2481 has **no assert-break register bit**.  A break is an escape sequence
embedded in the transmit stream (datasheet 7.5.4) and only works when **ETC**
(COR2 bit 5) is enabled:

    00 81   send BREAK        00 82 xx   lengthen it (xx * TPR tick)
    00 83   stop BREAK        00 00      a literal NUL

Enabling ETC has a cost: a real `0x00` in the outbound stream must now be sent
as `00 00`, or it is read as the start of a command.  The write path escapes it.

The console server was also discarding the request.  `nmconsole` swallowed
`IAC BRK` in its generic two-byte-command case, so a telnet client's *Send
Break* never reached the line -- useless on a console server.  It now calls
`tcsendbreak()`, which reaches the driver through `->break_ctl`.

**Status: implemented per the datasheet, not yet proven on the wire.**  A local
self-test that watched the transmit pin (RCOR bit 7, TLVal) never caught the
line low -- but that test is weak evidence: its first form wrote `TDR` from
outside a transmit interrupt context, where the datasheet says writes go
nowhere, and its second may be sampling the wrong channel, since the poll thread
moves `CAR` continuously.  Treat the first real break as a test of the feature.

## Remote access without the serial console

`telnetd` (handing off to `/bin/login`) starts from `init`, so the board is
reachable with no console cable attached.  Two things had to be fixed to make it
work, each of which looked like something else:

* **Sessions died the instant they connected.**  `/dev/ptmx` existed and
  `CONFIG_UNIX98_PTYS` was set, but nothing mounted **devpts**, so no pty could
  be allocated.
* **`/etc/shadow` was owned by the build user**, because cpio preserves host
  ownership; `/bin/login` will not trust it.  The image is now built with
  `cpio -R 0:0`.

Telnet is unencrypted.  The password in `build_initramfs.sh` is a speed bump
against a passer-by, not protection against anyone watching the wire -- change
it before the board goes anywhere that matters.

## A port that went deaf was a service the driver ended as the wrong type

Symptom: one console goes silent in both directions and stays that way. A client
connects, gets the telnet negotiation, and never receives a byte. Nothing is
typed back, and the attached device's own output never arrives. Other ports on
the card keep working, there is no mute message and no oops, and restarting
nmconsole does not help. Only a cold power cycle of the router clears it. It
happened under sustained two-way traffic: steady input into a device that echoed
a screen redraw for every byte.

Cause: `nm32a_service()` decided receive-or-transmit from the TIR/RIR read taken
before the interrupt acknowledge. With the PILRs equal, one acknowledge serves
every type and the chip grants receive first. So a byte arriving between that
read and the acknowledge turned a transmit the driver expected into a receive
the chip granted. The driver then wrote TDR into a receive context and ended it
with TEOIR. The receive context never ended, the chip stopped asking for service
on that channel, and InitCh on the next open does not touch an open interrupt
context.

Fix: take the branch, channel and vector from the post-acknowledge read, where
the granted context is the one reading Ren=0/Ract=1 (0x40). A context left
stranded by the old code reads the same way, so the fix also ends it on the next
pass. Two counters make the race visible: `st_misgrant` and `st_nogrant`.
