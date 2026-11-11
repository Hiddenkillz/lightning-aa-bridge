#!/bin/sh
# Post-mortem snapshot, 20 s after service start: kernel log, USB gadget state, and the radio facts
# we still need to learn about this Pi (can the BCM43455 run a 5 GHz AP AND a 2.4 GHz AP at once?).
# Keeps the last 5 snapshots. Output: /var/log/aa-bridge-boot-<epoch>.txt
sleep 20
{
  date; echo
  echo "== dmesg tail"; dmesg | tail -150; echo
  echo "== UDC state"; for u in /sys/class/udc/*; do echo "$u: $(cat $u/state 2>/dev/null)"; done
  ls /sys/kernel/config/usb_gadget/*/ 2>&1; cat /sys/kernel/config/usb_gadget/*/UDC 2>&1; echo
  echo "== rfkill"; rfkill list 2>&1; echo
  echo "== iw dev"; iw dev 2>&1; echo
  echo "== iw list (interface combinations + bands)"; iw list 2>&1 | sed -n '/valid interface combinations/,/Device supports/p;/Band [0-9]/p;/Supported interface modes/,/Band/p' | head -60; echo
  echo "== hostapd/dnsmasq"; pgrep -a hostapd; pgrep -a dnsmasq; echo
  echo "== bluetooth"; hciconfig -a 2>&1 | head -20; bluetoothctl show 2>&1 | head -25; echo
  echo "== ip addr"; ip -br addr 2>&1
} > /var/log/aa-bridge-boot-$(date +%s).txt 2>&1
ls -t /var/log/aa-bridge-boot-*.txt | tail -n +6 | xargs -r rm
