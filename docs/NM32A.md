# The NM-32A, in detail

32 asynchronous serial ports on a network module.  Getting it working was the
bulk of this project; this file is what a driver author needs to know.

## What is on the card

```
   +-----------------------------------------------------------------+
   |  NM-32A base board            "cisco Systems (c) 199x"          |
   |                                                                 |
   |   U18  U19  U20  U21     four SCD2481 quad UARTs = 16 channels   |
   |   [==] [==] [==] [==]                                            |
   |                                                                 |
   |   XILINX XC4013E PQ208     PCI interface, configures itself      |
   |   [================]       from the PLCC-20 PROM at U12          |
   |        U12 [PROM]                                                |
   |                                                                 |
   |   ====== expansion header ======                                 |
   |            |                                                     |
   +------------|----------------------------------------------------+
                |
   +------------|----------------------------------------------------+
   |  expansion board:  four more SCD2481 = channels 16..31           |
   +-----------------------------------------------------------------+

   Front panel: four 68-pin connectors, each fanning out to 8 RS-232 ports
                via an octal cable.
```

The FPGA configures itself from its own PROM at power-up, which is why the
card answers PCI (`10ee:4013`) on a cold boot before the host has sent it
anything.  IOS carries an FPGA image (`nm_as16_fw`) but only loads it into
cards of type 913 -- a later `NM-8/16AM-V2` revision whose bus interface is a
Spartan-II.  On this board that image is neither needed nor loadable.

## Port numbering

```
   port n  ->  chip n/4, channel n%4          (from IOS's own offset table)

   octal cable 1 : ports  0..7      chips 0,1
   octal cable 2 : ports  8..15     chips 2,3
   octal cable 3 : ports 16..23     chips 4,5
   octal cable 4 : ports 24..31     chips 6,7

   /dev/ttyNM<n>   and   TCP 2000+n

   e.g. first port of the third octal cable = port 16
        = chip 4, channel 0 = /dev/ttyNM16 = telnet host 2016
```

IOS numbers the same ports `line 1/0 .. 1/31`, absolute lines 66..97, reverse
telnet 2066..2097.

## Register access

Each chip has a 256-byte window in BAR0, at offsets taken from a table inside
IOS rather than guessed:

```
   chip   0      1      2      3      4      5      6      7
   off  0x400  0x500  0x600  0x700  0x900  0xA00  0xB00  0xC00
                                    ^ note the gap: 0x800 is a card register
```

Registers use the datasheet's **Motorola** column, not the Intel one.  The two
differ, and mixing them is silent:

```
   AIRl  Intel F0 / Motorola F2      MTCR  Intel F3 / Motorola F1
   AIRm  Intel F1 / Motorola F3      BTCR  Intel F4 / Motorola F6
   AIRh  Intel F2 / Motorola F0      GFRCR Intel 82 / Motorola 81
```

## Bring-up sequence

```
  1. pci_enable_device()          without it the whole BAR reads 0xff
  2. download microcode           8192 x 18-bit words, per chip
  3. CCR Reset All, wait GFRCR    GFRCR: 0xE0 (silicon rev D) -> 0x19 (firmware)
  4. per channel:
        CAR   = channel
        TCOR/TBPR, RCOR/RBPR      bit rate, see below
        CMR   = 0x02              async, interrupt (NOT DMA)
        COR1  = 0x17              8 bits, no parity
        COR3  = 0x02              1 stop bit
        COR4  = 0x01              interrupt per character
        CCR   = InitCh            wait for CCR to self-clear
        CCR   = EnTx|EnRx         wait again
        IER   = 0x09              RxD|TxD -- without this the chip never asks
```

Nothing responds until the microcode is loaded: the CD2481 has no protocol
code in ROM, only a boot ROM that clears the RAM-based register file.  An
undriven part therefore looks exactly like a block of RAM, which is a very
convincing wrong answer.

## Moving data: the interrupt-acknowledge context

This is the part that is easy to get wrong.  Data registers are not directly
addressable; from the datasheet:

> The interrupt context begins with the interrupt acknowledge bus cycle, and
> ends with a write access to the appropriate End of Interrupt register.
> During the interrupt context, only the per-channel registers for the channel
> being serviced are available; the CAR has no effect. ... the FIFO registers
> ... are Virtual registers, and must be accessed only during an interrupt
> context.

On this card the acknowledge cycle is:

```
   read  BAR + 0x00000
```

The FPGA turns that access into IACKIN* for whichever chip is requesting.
`TIR` goes `0xC8 -> 0x48` (Ten clears) when it lands.

**One acknowledge grants one service, and the chip chooses which**, receive
before transmit before modem.  So the loop must be:

```
   read TIR, RIR                 what is pending?
   read BAR+0                    ONE acknowledge
   read TIR, RIR again           which enable bit dropped?
     receive granted  -> RFOC, then RDR x count, then REOIR
     transmit granted -> TFTC, then TDR x n,     then TEOIR
```

Acknowledging twice in one pass -- once "for transmit", once "for receive" --
corrupts both directions, because the chip may grant receive when you assumed
transmit and you then write TDR inside a receive context.

## Bit rates

The bit-rate clock is **35.08 MHz**, measured by timing one character on the
wire (a `0x00` puts start + eight zero bits = nine bit times low, and RCOR bit
7 `TLVal` reflects the transmit pin).

```
   clk0 = CLK/8    clk1 = CLK/32     baud = clk / (BPR + 1)
```

ClkSel lives in **different bits** in the two clock registers:

```
   TCOR:  bit7 ClkSel2 | bit6 ClkSel1 | bit5 ClkSel0 | bit3 Ext-1X | bit1 LLM
   RCOR:  bit7 TLVal (read-only!) | bit5 DpllEn | bit4:3 Dpllmd | bit2:0 ClkSel
```

Writing the same value to both gives you two different rates.

```
    rate    ClkSel  BPR    actual   error
    9600      1     0x71    9616    +0.2%     (clk0 cannot reach 9600:
   19200      1     0x38   19232    +0.2%      divisor 455 > 8 bits)
   38400      0     0x71   38465    +0.2%
  115200      0     0x25  115395    +0.2%
```

## Signal path, end to end

```
  telnet client
      |  TCP 2000+n
      v
  nmconsole (userspace, one select loop, speaks telnet)
      |  read/write /dev/ttyNM<n>
      v
  tty layer
      |  tty_port ops / flip buffers
      v
  nm32a driver  --  poll thread, 50 us while a port is open
      |  BAR0 register access + acknowledge cycle at BAR+0
      v
  XC4013E FPGA  (PCI target, local bus master to the UARTs)
      |
      v
  CD2481 channel  --  microcode rev 0x19, 8N1
      |
      v
  RS-232 on the octal cable  ---->  the attached console
```
