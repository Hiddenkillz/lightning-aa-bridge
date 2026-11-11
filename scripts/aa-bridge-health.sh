#!/bin/sh
# Health recorder: every 2 s append uptime, CPU temp, CPU freq, firmware throttle/under-voltage flags and load to /var/log/aa-bridge-health.log
# and force it (and everything else buffered, incl. aa-bridge.log) to the SD card. If the Pi dies suddenly the last lines say how it was doing
# (power sag shows as under-voltage bits 0x1/0x10000 or a falling core voltage; heat as temp >= 80 C / throttle bits 0x2 0x4 0x8).
# get_throttled bits: 0 under-voltage now, 1 freq capped, 2 throttled, 3 soft temp limit; 16-19 = the same "has occurred since boot".
exec 9>/run/aa-bridge-health.lock; flock -n 9 || exit 0      # one recorder only (service restarts must not start a second)
LOG=/var/log/aa-bridge-health.log
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 2000000 ] && mv -f "$LOG" "$LOG.prev"
echo "=== health recorder start $(date) uptime $(cut -d' ' -f1 /proc/uptime)" >> "$LOG"
thr() {
  if [ -r /sys/devices/platform/soc/soc:firmware/get_throttled ]; then cat /sys/devices/platform/soc/soc:firmware/get_throttled
  elif command -v vcgencmd >/dev/null 2>&1; then vcgencmd get_throttled | cut -d= -f2
  else echo n/a; fi
}
# one-shot boot timing (read-only): where the boot time goes. Written into the same log ~60 s after start so read-card picks it up with no changes.
( sleep 60; { echo "bootanalyze: $(systemd-analyze 2>&1 | head -2 | tr '\n' ' ')"; systemd-analyze blame 2>&1 | head -22 | sed 's/^/bootanalyze: blame /'; systemd-analyze critical-chain 2>&1 | head -18 | sed 's/^/bootanalyze: chain /'; } >> "$LOG" ) </dev/null >/dev/null 2>&1 &
while :; do
  T=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
  F=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
  UV=$(cat /sys/class/hwmon/hwmon*/in0_lcrit_alarm 2>/dev/null | tr '\n' ',')
  U=$(cat /sys/class/udc/*/state 2>/dev/null | tr "\n" ",")
  V=$(command -v vcgencmd >/dev/null 2>&1 && vcgencmd measure_volts core 2>/dev/null | cut -d= -f2)
  echo "up=$(cut -d' ' -f1 /proc/uptime) temp_mC=$T freq_kHz=$F throttled=$(thr) uv_alarm=$UV core=$V udc=$U load=$(cut -d' ' -f1-3 /proc/loadavg) mem_free_kB=$(awk '/MemAvailable/{print $2}' /proc/meminfo)" >> "$LOG"
  sync
  # keep the file bounded during a long run too (the start-of-script check only sees the previous boot)
  if [ $((N % 15)) -eq 0 ]; then   # every ~30 s: new kernel USB/UVC complaints (cable/extender problems show up here: enumeration retries, -71 protocol errors, resets)
    UD=$(dmesg 2>/dev/null | grep -Ei "usb [0-9.-]+:|uvcvideo|xhci|over-current|reset (high|full|low)-speed|error -[0-9]+|cannot enumerate|descriptor read" | grep -v "gadget\|dwc2\|g_ffs\|configfs" | tail -4)
    [ "$UD" != "$LASTUD" ] && [ -n "$UD" ] && { echo "$UD" | sed "s/^/usbdmesg: /" >> "$LOG"; LASTUD="$UD"; }
  fi
  N=$((N+1)); if [ $((N % 450)) -eq 0 ] && [ "$(wc -c < "$LOG")" -gt 2000000 ]; then mv -f "$LOG" "$LOG.prev"; echo "=== rotated at uptime $(cut -d' ' -f1 /proc/uptime)" >> "$LOG"; fi
  sleep 2
done
