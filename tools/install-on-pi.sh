#!/usr/bin/env bash
# install-on-pi.sh -- install aa-bridge on a RUNNING Raspberry Pi (Raspberry Pi OS / Debian trixie or bookworm, arm64). The card-image flow is tools/deploy-card.sh.
# Run it on the Pi, as root, from a checkout of this repository (it installs the scripts and units from the checkout, so they always match the binary):
#
#   sudo tools/install-on-pi.sh --binary /path/to/aa-bridge --md-cert-dir DIR [--hu-cert-dir DIR] [--runtime-dir DIR] [--set key=value ...] [--bluez-justworks]
#
#   --binary FILE        the aarch64 aa-bridge executable (see BUILDING.md); the one it replaces is kept as aa-bridge.prev
#   --md-cert-dir DIR    holds md_cert.pem + md_key.pem: the phone-role identity presented to the head unit (checked: the key must match, expiry is printed)
#   --hu-cert-dir DIR    holds hu_cert.pem + hu_key.pem: the head-unit-role identity presented to the phone. Needed on the first install
#   --runtime-dir DIR    a folder with lib/ and gst/ (and gst-plugin-scanner) for a Pi that lacks the GStreamer / aasdk libraries: installed to /opt/aa-bridge and
#                        put on the service's LD_LIBRARY_PATH / GST_PLUGIN_PATH
#   --set key=value      a key for /etc/aa-bridge/aa-bridge.conf (repeatable), e.g. --set camera_enable=0 --set obd_enable=1. A new file gets a random AP password
#   --bluez-justworks    JustWorksRepairing=always in /etc/bluetooth/main.conf: a phone that forgot the bond can pair again without first removing it on the Pi.
#                        Off by default (any device could then re-pair over an existing bond while the Pi is discoverable)
#   --root DIR           install into the tree at DIR instead of / (no apt, no systemctl): for testing and for a mounted card
#   --reboot             reboot at the end
#
# What it sets up, and why each piece exists:
#   * hostapd + dnsmasq (apt), disabled as system services: the bridge starts its own copies for the phone's access point
#   * /usr/local/bin/aa-bridge + the four radio/health scripts, the service units, persistent journal, NetworkManager leaves only the AP interface (ap_iface, wlan0) alone
#   * a bluetoothd drop-in with --noplugin=sap: BlueZ's SIM Access server otherwise holds RFCOMM channel 8, the AA Wireless profile fails to register
#     ("rfcomm_bind: Address already in use") and the phone pairs but never starts wireless Android Auto
#   * dtoverlay=dwc2,dr_mode=peripheral for the Pi 4's USB-C port to act as the "phone" toward the vehicle (a Pi 3 cannot; a Pi 5 needs a different udc_name)
#   * /etc/aa-bridge: certificates (keys 0600) and the config
# Afterwards: reboot. wlan0 then becomes the phone's access point, so keep Ethernet (or a second adapter) for SSH.
set -euo pipefail

BIN=""; MD=""; HU=""; RT=""; ROOT=""; JW=0; REBOOT=0; SETS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --binary) BIN="$2"; shift ;;
    --md-cert-dir) MD="$2"; shift ;;
    --hu-cert-dir) HU="$2"; shift ;;
    --runtime-dir) RT="$2"; shift ;;
    --set) SETS+=("$2"); shift ;;
    --bluez-justworks) JW=1 ;;
    --root) ROOT="${2%/}"; shift ;;
    --reboot) REBOOT=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//' | grep -v '^set -euo'; exit 0 ;;
    *) echo "unknown option $1 (see --help)"; exit 2 ;;
  esac; shift
done

HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$BIN" ] || { echo "--binary FILE is required"; exit 2; }
[ "$(head -c 4 "$BIN" | od -An -c | tr -d ' ')" = '177ELF' ] && [ "$(od -An -tx1 -j18 -N1 "$BIN" | tr -d ' ')" = b7 ] || { echo "refusing: $BIN is not an aarch64 ELF"; exit 2; }   # ELF magic, e_machine 0xB7 = AArch64
[ -d "$MD" ] && [ -f "$MD/md_cert.pem" ] && [ -f "$MD/md_key.pem" ] || { echo "--md-cert-dir needs md_cert.pem and md_key.pem"; exit 2; }
[ "$(openssl x509 -in "$MD/md_cert.pem" -noout -modulus | md5sum)" = "$(openssl rsa -in "$MD/md_key.pem" -noout -modulus 2>/dev/null | md5sum)" ] || { echo "REFUSING: md_key.pem does not match md_cert.pem"; exit 2; }
if [ -n "$HU" ]; then [ -f "$HU/hu_cert.pem" ] && [ -f "$HU/hu_key.pem" ] || { echo "--hu-cert-dir needs hu_cert.pem and hu_key.pem"; exit 2; }
elif [ ! -f "$ROOT/etc/aa-bridge/hu_cert.pem" ]; then echo "first install: --hu-cert-dir (hu_cert.pem + hu_key.pem) is required"; exit 2; fi
[ -z "$RT" ] || { [ -d "$RT/lib" ] && [ -d "$RT/gst" ] || { echo "--runtime-dir needs lib/ and gst/"; exit 2; }; }
for f in scripts/aa-bridge-setup.sh scripts/aa-bridge-snapshot.sh scripts/aa-bridge-health.sh scripts/aa-bridge-apwatch.sh systemd/aa-bridge.service systemd/aa-bridge-btmon.service \
         systemd/journald.conf.d/persist.conf systemd/bluetooth/aa-bridge-nosap.conf; do
  [ -f "$HERE/$f" ] || { echo "missing $HERE/$f (run this from a checkout of the repository)"; exit 2; }
done
sysd() { [ -n "$ROOT" ] || systemctl "$@"; }
step() { echo "== $*"; }
inst() { install -D -m"$3" "$1" "$ROOT$2"; }

step "packages"
if [ -z "$ROOT" ]; then
  DEBIAN_FRONTEND=noninteractive apt-get install -y hostapd dnsmasq iw rfkill
  systemctl disable --now hostapd dnsmasq 2>&1 || true
  systemctl mask hostapd 2>&1 || true
fi

if [ -n "$RT" ]; then
  step "runtime bundle -> /opt/aa-bridge"
  rm -rf "$ROOT/opt/aa-bridge/lib" "$ROOT/opt/aa-bridge/gst"; mkdir -p "$ROOT/opt/aa-bridge"
  cp -a "$RT/lib" "$RT/gst" "$ROOT/opt/aa-bridge/"
  [ -f "$RT/gst-plugin-scanner" ] && inst "$RT/gst-plugin-scanner" /opt/aa-bridge/gst-plugin-scanner 755 || { [ -f "$RT/bin/gst-plugin-scanner" ] && inst "$RT/bin/gst-plugin-scanner" /opt/aa-bridge/gst-plugin-scanner 755; } || true
  mkdir -p "$ROOT/etc/systemd/system/aa-bridge.service.d"
  cat > "$ROOT/etc/systemd/system/aa-bridge.service.d/bundle.conf" <<'EOT'
[Service]
Environment=LD_LIBRARY_PATH=/opt/aa-bridge/lib
Environment=GST_PLUGIN_PATH=/opt/aa-bridge/gst
Environment=GST_PLUGIN_SYSTEM_PATH=/opt/aa-bridge/gst
Environment=GST_PLUGIN_SCANNER=/opt/aa-bridge/gst-plugin-scanner
Environment=GST_REGISTRY=/var/tmp/aa-bridge-gst.bin
Environment=GST_REGISTRY_FORK=no
EOT
fi

step "binary, scripts, units"
[ -f "$ROOT/usr/local/bin/aa-bridge" ] && cp -f "$ROOT/usr/local/bin/aa-bridge" "$ROOT/usr/local/bin/aa-bridge.prev"
inst "$BIN" /usr/local/bin/aa-bridge 755
for f in aa-bridge-setup.sh aa-bridge-snapshot.sh aa-bridge-health.sh aa-bridge-apwatch.sh; do inst "$HERE/scripts/$f" /usr/local/bin/$f 755; done
inst "$HERE/systemd/aa-bridge.service" /etc/systemd/system/aa-bridge.service 644
inst "$HERE/systemd/aa-bridge-btmon.service" /etc/systemd/system/aa-bridge-btmon.service 644
inst "$HERE/systemd/journald.conf.d/persist.conf" /etc/systemd/journald.conf.d/persist.conf 644
inst "$HERE/systemd/bluetooth/aa-bridge-nosap.conf" /etc/systemd/system/bluetooth.service.d/aa-bridge-nosap.conf 644
echo "$(basename "$BIN") $(md5sum "$BIN" | cut -c1-8)" > "$ROOT/etc/aa-bridge-build"

step "certificates and config"
mkdir -p "$ROOT/etc/aa-bridge"
inst "$MD/md_cert.pem" /etc/aa-bridge/md_cert.pem 644; inst "$MD/md_key.pem" /etc/aa-bridge/md_key.pem 600
[ ! -f "$MD/galroot_cert.pem" ] || inst "$MD/galroot_cert.pem" /etc/aa-bridge/galroot_cert.pem 644
if [ -n "$HU" ]; then inst "$HU/hu_cert.pem" /etc/aa-bridge/hu_cert.pem 644; inst "$HU/hu_key.pem" /etc/aa-bridge/hu_key.pem 600; fi
CONF="$ROOT/etc/aa-bridge/aa-bridge.conf"
if [ ! -f "$CONF" ]; then
  PW=$(head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 12)
  printf '# written by tools/install-on-pi.sh; keys and defaults: src/Config.h\nap_ssid=AABridge\nap_password=%s\nap_channel=44\n' "$PW" > "$CONF"; chmod 600 "$CONF"
  echo "AP password: $PW   (in /etc/aa-bridge/aa-bridge.conf)"
fi
for kv in "${SETS[@]:-}"; do
  [ -n "$kv" ] || continue
  k="${kv%%=*}"; grep -q "^$k=" "$CONF" && sed -i "s|^$k=.*|$kv|" "$CONF" || echo "$kv" >> "$CONF"
  echo "conf: $kv"
done
# NetworkManager must leave ONLY the access-point interface alone (hostapd owns it). Any other Wi-Fi adapter, e.g. a USB dongle used to reach the Pi
# while it is in the vehicle, stays under NetworkManager so it can join a normal network.
AP_IFACE=$(sed -n 's/^[[:space:]]*ap_iface[[:space:]]*=[[:space:]]*//p' "$CONF" | head -1 | tr -d '\r'); AP_IFACE=${AP_IFACE:-wlan0}
mkdir -p "$ROOT/etc/NetworkManager/conf.d"
printf '# aa-bridge owns %s (hostapd access point for the phone). NetworkManager must not manage or re-block it; other adapters stay managed.\n[keyfile]\nunmanaged-devices=interface-name:%s\n' "$AP_IFACE" "$AP_IFACE" > "$ROOT/etc/NetworkManager/conf.d/aa-bridge-unmanaged.conf"
echo "networkmanager: leaving only $AP_IFACE unmanaged"
END=$(openssl x509 -in "$MD/md_cert.pem" -noout -enddate | cut -d= -f2); LEFT=$(( ($(date -d "$END" +%s) - $(date +%s)) / 86400 ))
echo "phone-side cert expires $END ($LEFT days left)$([ "$LEFT" -lt 30 ] && echo '   <<< under 30 days: refresh it')"

step "USB gadget overlay"
CT="$ROOT/boot/firmware/config.txt"; [ -f "$CT" ] || CT="$ROOT/boot/config.txt"
if [ -f "$CT" ]; then
  cp -n "$CT" "$CT.pre-aa-bridge"
  grep -q '^dtoverlay=dwc2,dr_mode=peripheral' "$CT" || printf '\n[pi4]\ndtoverlay=dwc2,dr_mode=peripheral\n[all]\n' >> "$CT"
else echo "no config.txt found: add dtoverlay=dwc2,dr_mode=peripheral for the Pi 4 yourself"; fi

if [ "$JW" = 1 ]; then
  MC="$ROOT/etc/bluetooth/main.conf"
  if [ -f "$MC" ]; then
    grep -q '^JustWorksRepairing' "$MC" && sed -i 's/^JustWorksRepairing.*/JustWorksRepairing = always/' "$MC" || sed -i '/^\[General\]/a JustWorksRepairing = always' "$MC"
    echo "bluez: JustWorksRepairing = always"
  else echo "no $MC"; fi
fi

step "services"
sysd daemon-reload
sysd restart systemd-journald || true
if [ -n "$ROOT" ]; then mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"; ln -sf /etc/systemd/system/aa-bridge.service "$ROOT/etc/systemd/system/multi-user.target.wants/aa-bridge.service"
else systemctl enable aa-bridge.service; fi
echo "DONE. Reboot to activate dwc2, the bluetoothd change and the service; wlan0 then becomes the phone's access point (keep Ethernet for SSH)."
echo "After the reboot:  bluetoothctl show | grep -c 4de17a00   -> 1 means the AA Wireless profile registered"
[ "$REBOOT" = 1 ] && [ -z "$ROOT" ] && { echo "rebooting"; systemctl reboot; }
exit 0
