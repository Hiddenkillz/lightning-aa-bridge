#!/bin/bash
# aa-bridge-setup.sh -- radio setup (Wi-Fi AP + Bluetooth). BEST-EFFORT ONLY.
#
# The USB gadget / AA session toward the truck must NEVER depend on this
# script: a failure here once crash-looped the whole service (rfkill
# soft-block + `set -e` in an ExecStartPre) and the truck just saw a silent
# powered device. Every step logs and continues; the script always exits 0.
# It is also run from ExecStartPost/background, not ExecStartPre.

LOG=/var/log/aa-bridge-setup.log
exec >>"$LOG" 2>&1
echo "=== $(date) aa-bridge-setup start ==="

# Single source of truth = /etc/aa-bridge/aa-bridge.conf (the daemon sends the SAME ssid/password to the phone
# over Bluetooth). Environment variables still override for ad-hoc testing.
CONF=/etc/aa-bridge/aa-bridge.conf
conf() { [ -f "$CONF" ] && sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" | head -1 | tr -d '\r'; }
IFACE=${AP_IFACE:-$(conf ap_iface)}; IFACE=${IFACE:-wlan0}
AP_CHANNEL=${AP_CHANNEL:-$(conf ap_channel)}; AP_CHANNEL=${AP_CHANNEL:-44}
SSID=${AP_SSID:-$(conf ap_ssid)};    SSID=${SSID:-AABridge}
PASS=${AP_PASSWORD:-$(conf ap_password)}; PASS=${PASS:-aabridgepass}
BT_NAME=${BT_NAME:-$(conf bt_name)}; BT_NAME=${BT_NAME:-AA-Bridge}
echo "[setup] iface=$IFACE ssid=$SSID bt_name=$BT_NAME (password from conf, not logged)"

step() { echo "[setup] $*"; "$@" || echo "[setup] (non-fatal) failed: $*"; }

# The Pi image keeps the Wi-Fi radio soft-blocked: NetworkManager's saved state is WirelessEnabled=false and it re-blocks the radio
# after a plain `rfkill unblock` (found on the bench: hostapd "WLAN soft blocked", RTNETLINK RF-kill). Switch it on through NM, take wlan0
# away from NM, stop the system hostapd/dnsmasq units (they hold port 53 / have no config), then verify the block is really gone.
if systemctl is-active --quiet NetworkManager 2>/dev/null; then
  step nmcli radio wifi on
  step nmcli device set "${IFACE}" managed no
fi
step systemctl stop hostapd dnsmasq
step rfkill unblock all
step iw reg set US
wifi_unblocked() { ! rfkill list wifi 2>/dev/null | grep -q 'Soft blocked: yes'; }
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  wifi_unblocked && break
  echo "[setup] WLAN still soft-blocked (try $i); unblocking again"; rfkill unblock wifi; rfkill unblock all; sleep 1
done
if wifi_unblocked; then echo "[setup] WLAN unblocked"; else echo "[setup] WARNING: WLAN STILL BLOCKED - access point will not start"; rfkill list 2>&1; fi
sleep 1

# ---- Wi-Fi AP ----
# Basic 11a config is the proven fallback; try 11n/11ac (HT40) first for throughput/latency. Channel = the HOME network's channel (44): the start-up
# (the AP and a station on the same radio must share a channel: brcmfmac limitation)
cat > /tmp/aa-bridge-hostapd-base.conf << EOT
interface=${IFACE}
driver=nl80211
ctrl_interface=/var/run/hostapd
ctrl_interface_group=0
country_code=US
ieee80211d=1
ssid=${SSID}
hw_mode=a
channel=${AP_CHANNEL}
wmm_enabled=1
auth_algs=1
wpa=2
wpa_passphrase=${PASS}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOT
cp /tmp/aa-bridge-hostapd-base.conf /tmp/aa-bridge-hostapd.conf
cat >> /tmp/aa-bridge-hostapd.conf << EOT
ieee80211n=1
noscan=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]
ieee80211ac=1
vht_capab=[SHORT-GI-80]
vht_oper_chwidth=0
EOT
cat > /tmp/aa-bridge-dnsmasq.conf << EOT
interface=${IFACE}
port=0
bind-interfaces
dhcp-range=192.168.50.10,192.168.50.50,255.255.255.0,24h
dhcp-option=3,192.168.50.1
dhcp-option=6,192.168.50.1
EOT
# Experiment (config ap_dns=1, default off): answer DNS ourselves (every name -> the Pi) so the phone's connectivity probes resolve and fail FAST instead of timing out
# (hypothesis for the phone leaving the AP ~1.3 s after joining). Off unless asked: unproven.
if [ "$(conf ap_dns)" = "1" ]; then
  sed -i '/^port=0$/d' /tmp/aa-bridge-dnsmasq.conf
  printf 'no-resolv\naddress=/#/192.168.50.1\n' >> /tmp/aa-bridge-dnsmasq.conf
  echo "[setup] ap_dns=1: dnsmasq answers DNS (all names -> 192.168.50.1)"
fi

step ip addr flush dev "${IFACE}"
step ip addr add 192.168.50.1/24 dev "${IFACE}"
step ip link set "${IFACE}" up
pkill hostapd 2>/dev/null; pkill dnsmasq 2>/dev/null; sleep 1
if ! hostapd -t -f /var/log/aa-bridge-hostapd.log /tmp/aa-bridge-hostapd.conf -B -P /tmp/hostapd.pid; then
  echo "[setup] hostapd with 11n/ac failed; falling back to basic 11a"
  step hostapd -t -f /var/log/aa-bridge-hostapd.log /tmp/aa-bridge-hostapd-base.conf -B -P /tmp/hostapd.pid
fi
step dnsmasq -C /tmp/aa-bridge-dnsmasq.conf --pid-file=/tmp/dnsmasq.pid --log-dhcp --log-facility=/var/log/aa-bridge-dnsmasq.log
# Wi-Fi power save makes the AP answer late (truck 33 dmesg: "power save enabled"); a car-bound AP has no battery to save. Applied AFTER hostapd is up: switching the
# interface into AP mode can reset it. Done twice (now and again after a short pause) because the driver may re-enable it when the first station associates.
step iw dev "${IFACE}" set power_save off
( sleep 20; iw dev "${IFACE}" set power_save off >/dev/null 2>&1; iw dev "${IFACE}" get power_save 2>&1 | sed 's/^/[setup] /' ) &

# ---- Bluetooth ----
# Pairing agent, profiles (AA Wireless + HSP), discoverable/pairable are handled inside aa-bridge (BtBootstrap, via BlueZ D-Bus).
step rfkill unblock bluetooth
step systemctl start bluetooth

echo "[setup] done (best-effort)"
exit 0
