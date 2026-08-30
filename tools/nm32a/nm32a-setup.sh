#!/bin/sh
# SPDX-License-Identifier: MIT
# NM-32A bring-up and console server, for relicnos on the Cisco 2811.
#
# Everything the card needs is done by the kernel driver at probe: microcode
# into all eight CD2481s, tty registration, and the poll thread.  This script
# is the operator's front end -- check it came up, set line speeds, and start
# the reverse-telnet server.
#
#   nm32a-setup.sh status              what the card is doing
#   nm32a-setup.sh speed <port> <baud> change one line's speed
#   nm32a-setup.sh start [baud]        start the console server
#   nm32a-setup.sh stop
#
# Port numbering matches IOS: port n is chip n/4 channel n%4, and the octal
# cables are ports 0-7, 8-15, 16-23, 24-31.  Port 16 is the first port of the
# third cable.  TCP is 2000 + port, so port 16 is telnet <host> 2016.

BASE=2000

usage() { sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 1; }

status() {
    echo "== driver =="
    dmesg | grep -a nm32a | tail -12
    echo
    echo "== ports =="
    n=0
    while [ $n -lt 32 ]; do
        if [ -c /dev/ttyNM$n ]; then
            printf "  /dev/ttyNM%-2d  cable %d port %d  ->  TCP %d\n" \
                   "$n" $((n / 8 + 1)) $((n % 8)) $((BASE + n))
        fi
        n=$((n + 1))
    done
    echo
    echo "== console server =="
    if pidof nmconsole >/dev/null 2>&1; then
        echo "  running (pid $(pidof nmconsole))"
    else
        echo "  not running -- start it with: $0 start"
    fi
}

case "$1" in
status|"")
    status
    ;;
speed)
    [ -n "$2" ] && [ -n "$3" ] || usage
    # the driver maps the rate onto ClkSel/BPR; see nm32a.c.  9600 and below
    # live on clk1 because clk0 cannot express them in an 8-bit divisor.
    stty -F /dev/ttyNM"$2" "$3" raw -echo
    echo "port $2 set to $3 baud"
    stty -F /dev/ttyNM"$2" | head -2
    ;;
start)
    if pidof nmconsole >/dev/null 2>&1; then
        echo "already running (pid $(pidof nmconsole))"; exit 0
    fi
    [ -c /dev/ttyNM0 ] || { echo "no /dev/ttyNM* -- did the driver probe?"; exit 1; }
    nmconsole --base $BASE --ports 32 --speed "${2:-9600}" &
    sleep 1
    pidof nmconsole >/dev/null 2>&1 && echo "started; port 16 is telnet <host> $((BASE+16))"
    ;;
stop)
    killall nmconsole 2>/dev/null && echo stopped || echo "not running"
    ;;
*)
    usage
    ;;
esac
