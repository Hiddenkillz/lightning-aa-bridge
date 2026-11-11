#!/usr/bin/env bash
# Deploy an aa-bridge binary (and optionally config/prefs) onto the SD card mounted via the the build host card reader.
# Run on the build host.   usage: deploy-card.sh <binary> [--keep-logs] [--scripts] [--set key=value ...] [--prefs <file>]
# Safe by design: refuses an empty/0B device, backs up the binary it replaces, verifies md5 after copy,
# clears logs unless --keep-logs, always unmounts. Never edits anything else on the card.
set -euo pipefail
BIN="${1:?usage: deploy-card.sh <binary> [--keep-logs] [--set key=value ...] [--prefs file]}"; shift
KEEP=0; SETS=(); PREFS=""; SCRIPTS=0; MDCERT=""; SEEDSDREQ=""; FWDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-logs) KEEP=1 ;;
    --scripts) SCRIPTS=1 ;;      # also install setup/snapshot scripts, service unit, journald conf (backs up what it replaces)
    --set) SETS+=("$2"); shift ;;
    --prefs) PREFS="$2"; shift ;;
    --seed-sdreq) SEEDSDREQ="$2"; shift ;;   # a recorded phone service-discovery request, installed ONLY if the card has none yet (the real one is recorded when the phone connects)
    --firmware-dir) FWDIR="$2"; shift ;;   # dir of Bluetooth patch files (brcm/*.hcd): installed into /lib/firmware (the BCM20702B0 dongle needs BCM20702B0-19ff-0239.hcd or its SCO audio toward the vehicle is silent)
    --md-cert-dir) MDCERT="$2"; shift ;;
    --no-probe) NOPROBE=1 ;;
    --onboard-bt) ONBOARD_BT="$2"; shift ;;      # off|on: dtoverlay=disable-bt in the boot partition's config.txt (two USB dongles carry both Bluetooth roles; Wi-Fi unaffected)
    --bluez-justworks) BLUEZ_JW=1 ;;             # JustWorksRepairing=always in /etc/bluetooth/main.conf (ATS2851 dongles: no User Confirmation Negative Reply)   # dir with md_cert.pem + md_key.pem (+ optional galroot_cert.pem): installed on the card, the replaced files are backed up as *.prev
    *) echo "unknown option $1"; exit 2 ;;
  esac; shift
done
[ -f "$BIN" ] || { echo "no such binary: $BIN"; exit 2; }
file "$BIN" | grep -q 'ARM aarch64' || { echo "refusing: $BIN is not an aarch64 ELF"; exit 2; }

DEV=$(lsblk -dno NAME,SIZE,TRAN | awk '$3=="usb" && $2!="0B"{print $1}' | head -1)
[ -n "$DEV" ] || { echo "NO CARD FOUND (reader slot empty?)"; exit 3; }
PART="/dev/${DEV}2"; MNT=/root/aa-bridge-sdcard/root
[ -b "$PART" ] || { echo "no root partition $PART"; exit 3; }
umount "$MNT" 2>/dev/null || true
mount "$PART" "$MNT"
trap 'sync; umount "$MNT" 2>/dev/null || true' EXIT
[ -x "$MNT/usr/local/bin/aa-bridge" ] || [ -d "$MNT/etc/aa-bridge" ] || { echo "this does not look like an aa-bridge card"; exit 4; }

OLD=$(md5sum "$MNT/usr/local/bin/aa-bridge" 2>/dev/null | cut -c1-8 || echo none)
[ "$OLD" != none ] && cp -f "$MNT/usr/local/bin/aa-bridge" "$MNT/usr/local/bin/aa-bridge.prev-$OLD"
install -m755 "$BIN" "$MNT/usr/local/bin/aa-bridge"
WANT=$(md5sum "$BIN" | cut -d' ' -f1); GOT=$(md5sum "$MNT/usr/local/bin/aa-bridge" | cut -d' ' -f1)
[ "$WANT" = "$GOT" ] || { echo "MD5 MISMATCH after copy"; exit 5; }

CONF="$MNT/etc/aa-bridge/aa-bridge.conf"
for kv in "${SETS[@]:-}"; do
  [ -n "$kv" ] || continue
  k="${kv%%=*}"
  if grep -q "^$k=" "$CONF"; then sed -i "s|^$k=.*|$kv|" "$CONF"; else echo "$kv" >> "$CONF"; fi
  echo "conf: $kv"
done
# radio setup scripts / service unit / journald conf (the binary and these must match: the daemon and the setup script share aa-bridge.conf)
if [ "$SCRIPTS" = 1 ]; then
  SDIR="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)/scripts"; [ -d "$SDIR" ] || SDIR="$(dirname "$0")/scripts"
  YDIR="$(dirname "$SDIR")/systemd"
  inst() { src="$1"; dst="$2"; mode="$3"; [ -f "$src" ] || { echo "missing $src"; exit 6; }
           [ -f "$dst" ] && ! cmp -s "$src" "$dst" && cp -f "$dst" "$dst.prev"
           mkdir -p "$(dirname "$dst")"; install -m"$mode" "$src" "$dst"; echo "installed $dst"; }
  inst "$SDIR/aa-bridge-setup.sh"    "$MNT/usr/local/bin/aa-bridge-setup.sh"    755
  inst "$SDIR/aa-bridge-snapshot.sh" "$MNT/usr/local/bin/aa-bridge-snapshot.sh" 755
  inst "$SDIR/aa-bridge-health.sh"   "$MNT/usr/local/bin/aa-bridge-health.sh"   755
  inst "$SDIR/aa-bridge-apwatch.sh"  "$MNT/usr/local/bin/aa-bridge-apwatch.sh"  755
  inst "$YDIR/aa-bridge.service"     "$MNT/etc/systemd/system/aa-bridge.service" 644
  inst "$YDIR/aa-bridge-btmon.service" "$MNT/etc/systemd/system/aa-bridge-btmon.service" 644
  inst "$YDIR/journald.conf.d/persist.conf" "$MNT/etc/systemd/journald.conf.d/persist.conf" 644
  inst "$YDIR/NetworkManager/aa-bridge-unmanaged.conf" "$MNT/etc/NetworkManager/conf.d/aa-bridge-unmanaged.conf" 644
  # NetworkManager's saved state keeps the radio OFF (WirelessEnabled=false) and re-blocks it; the system hostapd/dnsmasq units
  # are enabled on this image, hold port 53 and have no config. Fix the saved state and disable both units.
  NMS="$MNT/var/lib/NetworkManager/NetworkManager.state"
  if [ -f "$NMS" ] && grep -q '^WirelessEnabled=false' "$NMS"; then sed -i 's/^WirelessEnabled=false/WirelessEnabled=true/' "$NMS"; echo "NetworkManager WirelessEnabled -> true"; fi
  for u in hostapd dnsmasq; do rm -f "$MNT/etc/systemd/system/multi-user.target.wants/$u.service" && echo "disabled system $u.service"; done
fi
# head-unit TLS identity for the phone-facing side (install if missing; public well-known AA HU pair)
HUDIR="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)/assets/hu"
[ -d "$HUDIR" ] || HUDIR="$(dirname "$0")/hu"
if [ -f "$HUDIR/hu_cert.pem" ] && [ ! -f "$MNT/etc/aa-bridge/hu_cert.pem" ]; then
  install -m644 "$HUDIR/hu_cert.pem" "$MNT/etc/aa-bridge/hu_cert.pem"; install -m600 "$HUDIR/hu_key.pem" "$MNT/etc/aa-bridge/hu_key.pem"; echo "hu cert/key installed"
fi
echo "$(basename "$BIN") $(md5sum "$BIN" | cut -c1-8)" > "$MNT/etc/aa-bridge-build" && echo "build tag: $(cat "$MNT/etc/aa-bridge-build") -> /etc/aa-bridge-build (shown in the UI footers)"
if [ -n "$FWDIR" ]; then
  [ -d "$FWDIR/brcm" ] || { echo "--firmware-dir needs a brcm/ subdir in $FWDIR"; exit 2; }
  mkdir -p "$MNT/lib/firmware/brcm"
  for f in "$FWDIR"/brcm/*.hcd; do install -m644 "$f" "$MNT/lib/firmware/brcm/$(basename "$f")" && echo "firmware: installed $(basename "$f") ($(stat -c%s "$f") bytes)"; done
fi
if [ -n "$SEEDSDREQ" ]; then
  [ -f "$SEEDSDREQ" ] || { echo "no such file: $SEEDSDREQ"; exit 2; }
  if [ -s "$MNT/etc/aa-bridge/phone_sdreq.bin" ]; then echo "phone_sdreq.bin already on the card ($(stat -c%s "$MNT/etc/aa-bridge/phone_sdreq.bin") bytes): kept"; else install -m644 "$SEEDSDREQ" "$MNT/etc/aa-bridge/phone_sdreq.bin"; echo "seeded phone_sdreq.bin from $SEEDSDREQ"; fi
fi
if [ -n "$MDCERT" ]; then
  [ -f "$MDCERT/md_cert.pem" ] && [ -f "$MDCERT/md_key.pem" ] || { echo "--md-cert-dir needs md_cert.pem and md_key.pem in $MDCERT"; exit 2; }
  [ "$(openssl x509 -in "$MDCERT/md_cert.pem" -noout -modulus | md5sum)" = "$(openssl rsa -in "$MDCERT/md_key.pem" -noout -modulus 2>/dev/null | md5sum)" ] || { echo "REFUSING: md_key.pem does not match md_cert.pem"; exit 2; }
  for f in md_cert.pem md_key.pem galroot_cert.pem; do
    [ -f "$MDCERT/$f" ] || continue
    [ -f "$MNT/etc/aa-bridge/$f" ] && cp -p "$MNT/etc/aa-bridge/$f" "$MNT/etc/aa-bridge/$f.prev"
    install -m $([ "$f" = md_key.pem ] && echo 600 || echo 644) "$MDCERT/$f" "$MNT/etc/aa-bridge/$f"
  done
  echo "phone-side cert installed from $MDCERT (previous kept as *.prev)"
fi
[ -n "${NOPROBE:-}" ] && rm -rf "$MNT/etc/aa-bridge/probe" 2>/dev/null; true   # --no-probe: kept for old invocations (the probe experiment is gone)
# always report which phone-side cert the card now holds and how long it has left (the truck rejects an expired one: AUTH_COMPLETE -24)
if [ -f "$MNT/etc/aa-bridge/md_cert.pem" ]; then
  END=$(openssl x509 -in "$MNT/etc/aa-bridge/md_cert.pem" -noout -enddate | cut -d= -f2); LEFT=$(( ($(date -d "$END" +%s) - $(date +%s)) / 86400 ))
  echo "md cert on card: expires $END ($LEFT days left)$([ "$LEFT" -lt 30 ] && echo '   <<< WARNING: under 30 days, refresh it (--md-cert-dir)')"
fi
if [ -n "${ONBOARD_BT:-}" ]; then
  BOOTP="/dev/${DEV}1"; BOOTM=/root/aa-bridge-sdcard/boot; mkdir -p "$BOOTM"; umount "$BOOTM" 2>/dev/null || true; mount "$BOOTP" "$BOOTM"
  CT="$BOOTM/config.txt"; [ -f "$CT" ] || { echo "no config.txt on the boot partition"; umount "$BOOTM"; exit 2; }
  sed -i '/^dtoverlay=disable-bt$/d' "$CT"
  if [ "$ONBOARD_BT" = off ]; then echo "dtoverlay=disable-bt" >> "$CT"; rm -f "$MNT/etc/systemd/system/multi-user.target.wants/hciuart.service"; echo "onboard Bluetooth DISABLED (dtoverlay=disable-bt, hciuart off)";
  else echo "onboard Bluetooth enabled"; fi
  sync; umount "$BOOTM"
fi
if [ -n "${BLUEZ_JW:-}" ]; then
  MC="$MNT/etc/bluetooth/main.conf"; [ -f "$MC" ] || { echo "no $MC"; exit 2; }
  grep -q '^JustWorksRepairing' "$MC" && sed -i 's/^JustWorksRepairing.*/JustWorksRepairing = always/' "$MC" || sed -i '/^\[General\]/a JustWorksRepairing = always' "$MC"
  echo "bluez: JustWorksRepairing = always"
fi
[ -n "$PREFS" ] && install -m644 "$PREFS" "$MNT/etc/aa-bridge/prefs.conf" && echo "prefs installed"
if [ "$KEEP" = 0 ]; then : > "$MNT/var/log/aa-bridge.log"; : > "$MNT/var/log/aa-bridge-health.log"; : > "$MNT/var/log/aa-bridge-hostapd.log" 2>/dev/null || true; : > "$MNT/var/log/aa-bridge-dnsmasq.log" 2>/dev/null || true; rm -f "$MNT"/var/log/aa-bridge-boot-*.txt "$MNT/var/log/aa-bridge-setup.log"; fi
echo "DEPLOYED $(echo "$WANT" | cut -c1-8) (previous: $OLD) on /dev/$DEV  logs-cleared=$((1-KEEP))"
echo "CARD-READY"
