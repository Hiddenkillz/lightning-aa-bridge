#!/bin/sh
# Wi-Fi access-point watchdog. The phone can only join if hostapd is alive; if it dies (or the radio is reset) nothing else in the system notices and wireless AA
# would stay broken until the next boot. Every INTERVAL s: is the hostapd from the setup script still running and is the interface in AP mode? After 3 failed checks in a
# row the setup script (which rebuilds hostapd + dnsmasq from scratch) is run again, with a back-off, at most MAXTRIES times per boot.
# Overridable for tests: SETUP PIDFILE IFACE LOGF LOCKF GRACE INTERVAL MAXTRIES BACKOFF CHECK_IW
SETUP=${SETUP:-/usr/local/bin/aa-bridge-setup.sh}; PIDFILE=${PIDFILE:-/tmp/hostapd.pid}; IFACE=${IFACE:-wlan0}; LOGF=${LOGF:-/var/log/aa-bridge-apwatch.log}
GRACE=${GRACE:-120}; INTERVAL=${INTERVAL:-5}; MAXTRIES=${MAXTRIES:-5}; BACKOFF=${BACKOFF:-60}
exec 8>"${LOCKF:-/run/aa-bridge-apwatch.lock}"; flock -n 8 || exit 0
say() { echo "$(date '+%F %T') $*" >> "$LOGF"; }
say "apwatch start (grace ${GRACE}s, every ${INTERVAL}s)"
sleep "$GRACE"
bad=0; tries=0
while :; do
  ok=1
  if [ ! -r "$PIDFILE" ] || ! kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then ok=0; why="hostapd not running"
  elif [ "${CHECK_IW:-1}" = 1 ] && command -v iw >/dev/null 2>&1 && ! iw dev "$IFACE" info 2>/dev/null | grep -q 'type AP'; then ok=0; why="$IFACE is not in AP mode"; fi
  if [ "$ok" = 1 ]; then bad=0
  else
    bad=$((bad+1))
    if [ "$bad" -ge 3 ]; then
      if [ "$tries" -ge "$MAXTRIES" ]; then say "access point still down ($why) and $MAXTRIES restarts already tried: giving up until reboot"; exit 0; fi
      tries=$((tries+1)); say "access point down ($why) for $bad checks -> rebuilding it (try $tries/$MAXTRIES)"
      "$SETUP" >> "$LOGF" 2>&1; bad=0; sleep "$BACKOFF"
    fi
  fi
  sleep "$INTERVAL"
done
