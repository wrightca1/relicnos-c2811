# Installing and running

## Getting the kernel onto the box

The CompactFlash is a normal removable card with an MBR and one FAT16
partition.  The simplest install is to pull it and write the file directly:

```
cp vmlinuz /mnt/cf/vmlinuz-c2811
```

**Image the card first.**  It holds the only copy of the IOS image, and you
will want to go back to IOS.

### From ROMMON

Interrupt the boot -- a serial BREAK works reliably within about 60 s of
power-on, repeated Ctrl-C much less so -- then:

```
rommon 1 > boot flash:vmlinuz-c2811
```

To make it the default:

```
rommon 1 > BOOT=flash:vmlinuz-c2811
rommon 1 > sync
```

Keep IOS bootable: `boot flash:<ios-image>.bin` still works, and a warm
`reload` from IOS passes back through ROMMON where BREAK is accepted again.

### Replacing the kernel from a running relicnos

Once relicnos boots, it can install its own successor: Ethernet works, BusyBox
has a TFTP client, and the CF is writable.

```
ip addr add <addr>/<len> dev eth0 && ip link set eth0 up
tftp -g -r vmlinuz-c2811 -l /vmlinuz.new <tftp-server>
md5sum /vmlinuz.new
mount -o remount,rw /mnt/flash
cp /vmlinuz.new /mnt/flash/vmlinuz-c2811 && sync
mount -o remount,ro /mnt/flash
```

`/mnt/flash` is mounted read-only at boot, so **remount** it -- a fresh
`mount` stacks and the copy then fails on a read-only filesystem.

A caution learned the hard way: if the driver hangs during probe, the box never
reaches a shell and this fast path is gone.  Recovery is then BREAK into
ROMMON, boot IOS, and `copy tftp: flash:` -- about fifteen minutes.  Anything
experimental belongs behind a runtime trigger, not in probe.

## Serial console

9600 8N1 on the RJ45 console port.

## Bringing up the 32 async ports

The driver does the hardware at probe: microcode into all eight CD2481s, 32
ttys, and the poll thread.  Check it came up:

```
# nm32a-setup.sh status
== driver ==
nm32a 0000:00:01.0: enabling device (0000 -> 0003)
nm32a: chip 0 @ +0x400: GFRCR e0 -> 19, CCR self-cleared
...
nm32a 0000:00:01.0: 32 ports registered as /dev/ttyNM0..31 (major 252)

== ports ==
  /dev/ttyNM16  cable 3 port 0  ->  TCP 2016
  ...
```

Start the console server:

```
# nm32a-setup.sh start 9600
started; port 16 is telnet <host> 2016
```

and from anywhere on the network:

```
$ telnet <router-ip> 2016
sw7150-lab login:
```

Per-line speed:

```
# nm32a-setup.sh speed 16 115200
```

`init` starts the server automatically when `/dev/ttyNM0` exists, so a booted
box simply answers.  Edit `initramfs/build_initramfs.sh` to set the address it
configures on `eth0`.

## Going back to IOS

`bootios` renames the kernel so ROMMON falls through to the IOS image, then
power-cycle.  IOS does **not** survive a warm restart from relicnos -- it dies
in init with a TLB exception -- so a cold power cycle is part of the procedure.
