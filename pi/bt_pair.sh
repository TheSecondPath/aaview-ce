#!/bin/sh
# Scan for Bluetooth input devices, and pair, trust and connect those whose name matches $1 (by default 8BitDo,
# ProtoArc and common keyboard and gamepad names). Everything goes to /tmp/bt_pair.log, including any passkey to
# type on a keyboard ("Passkey: NNNNNN").
PAT=${1:-8BitDo|ProtoArc|Keyboard|KB|Mouse|Pad|Gamepad|Controller|Pro 2}
LOG=/tmp/bt_pair.log
ESC=$(printf '\033')
clean() { sed "s/${ESC}\[[0-9;]*m//g" "$LOG"; }   # bluetoothctl output without colour codes
: > $LOG
sudo rfkill unblock bluetooth 2>/dev/null
sudo bluetoothctl power on >> $LOG 2>&1

# Phase 1: scan 30 s.
( echo "scan on"; sleep 30; echo "scan off"; echo "devices"; sleep 1; echo "quit" ) | sudo bluetoothctl >> $LOG 2>&1
# Match by name, or by the input-device icon bluetoothctl reports (input-keyboard, input-gaming, input-mouse).
MACS=$( (clean | grep -aE "Device ([0-9A-F]{2}:){5}[0-9A-F]{2} " | grep -iE "$PAT" | grep -aoE "([0-9A-F]{2}:){5}[0-9A-F]{2}"
        for d in $(clean | grep -aoE "Device ([0-9A-F]{2}:){5}[0-9A-F]{2}" | awk '{print $2}' | sort -u); do
            sudo bluetoothctl info $d 2>/dev/null | grep -qE "Icon: input-(keyboard|gaming|mouse)" && echo $d
        done) | sort -u)
echo "=== matching devices: ${MACS:-none}" >> $LOG

# Phase 2: pair, trust and connect each one. The agent shows any passkey, and there are 40 s to type it.
for m in $MACS; do
    ( echo "agent KeyboardDisplay"; echo "default-agent"; echo "scan on"; sleep 4
      echo "pair $m"; sleep 40; echo "trust $m"; sleep 2; echo "connect $m"; sleep 8
      echo "info $m"; sleep 1; echo "scan off"; echo "quit" ) | sudo bluetoothctl >> $LOG 2>&1
done
echo "=== input devices now:" >> $LOG
grep -H . /sys/class/input/event*/device/name >> $LOG 2>&1
echo "=== done" >> $LOG
