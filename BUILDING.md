# Building and installing

Read the first section of `README.md` first: without an Android Auto **device** certificate the truck-facing half will not start. This document assumes you understand that and still want to build, read, or port the code.

## What you need
- Raspberry Pi 4 running Raspberry Pi OS (Bookworm, 64-bit). Kernel 6.6 is what the author runs.
- Packages: `cmake g++ pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libssl-dev libprotobuf-dev protobuf-compiler libboost-all-dev libbluetooth-dev libglib2.0-dev`
- aasdk built and installed on the Pi (or in your cross sysroot); `CMakeLists.txt` finds it with `find_package(aasdk)`. The source uses the `aap_protobuf` layout, so use the [OpenCarDev fork](https://github.com/opencardev/aasdk) (the original [f1xpl](https://github.com/f1xpl/aasdk) has `aasdk_proto` and will not compile this tree). `tools/Dockerfile.build` is a working recipe on Debian Bookworm.
- A USB-C cable to the vehicle's Android Auto port (it powers the Pi), a UVC camera, two USB Bluetooth adapters, optionally a vLinker FS for OBD-II.

## Build
```
mkdir build && cd build
cmake .. && make -j4
```
`tools/Dockerfile.build` has the commands to build and run the cert-free suites in a container on any arm64 host (usage in its comments).
The author cross-builds on an x86 host with a copied Pi sysroot and runs the test suite under `qemu-aarch64-static`; `tools/regress.sh` is that suite. Every test that touches the AA protocol takes certificate paths as arguments, which is why you cannot run the full suite without the certificates.

## Install on a card
`tools/deploy-card.sh <binary> --scripts --md-cert-dir <dir with md_cert.pem + md_key.pem> [--set key=value ...]` copies the binary, scripts, systemd units and config onto an already-imaged card mounted from this host. Read the script; it is short and every option is commented. Configuration keys and defaults are in `src/Config.h`.

## Install on a running Pi
`sudo tools/install-on-pi.sh --binary <aa-bridge> --md-cert-dir <dir> --hu-cert-dir <dir> [--runtime-dir <dir>] [--set key=value ...]`, run on the Pi from a checkout of this repository, then reboot. `--help` lists every option; the header of the script explains each piece it sets up. Re-running it upgrades in place (the old binary is kept as `aa-bridge.prev`, the config and AP password are kept).
Things it does that are easy to get wrong by hand: it masks the system `hostapd`, adds `dtoverlay=dwc2,dr_mode=peripheral` for the Pi 4's USB-C port, and installs a `bluetoothd` drop-in with `--noplugin=sap`. Without that drop-in BlueZ's SIM Access server holds RFCOMM channel 8, the AA Wireless profile fails to register (`bluetoothd: RFCOMM server failed for AA Wireless: Address already in use`) and the phone pairs but never starts wireless Android Auto. Check with `bluetoothctl show | grep -c 4de17a00` (1 = registered). After the reboot wlan0 is the phone's access point, so keep Ethernet for SSH.
**Reaching the Pi while it is in the vehicle.** wlan0 is the phone's access point, so a USB Wi-Fi adapter (wlan1) is the Pi's own network connection there. The installer leaves only the AP interface (`ap_iface`) unmanaged, so NetworkManager manages the dongle: `sudo nmcli device wifi connect <ssid> password <pw> ifname wlan1` (saved, autoconnects). A 2.4 GHz-only adapter (e.g. MT7601U, in-kernel `mt7601u`) needs a network with a 2.4 GHz band. If an adapter shows no networks at all, try another USB port before suspecting the adapter (a marginal port gave an empty scan once).
If a phone says "incorrect PIN" when pairing (no PIN is involved), the Pi still holds a bond the phone has forgotten: `bluetoothctl remove <phone address>` on the Pi, forget the Pi on the phone, pair again (or install with `--bluez-justworks` to let the Pi accept a re-pair).

## Layout
- `src/headunit/` the head-unit role (phone side): Wi-Fi AP, Bluetooth bootstrap, AA session with the phone
- `src/device/` the phone role (vehicle side): USB gadget, AA session with the head unit, sensor/audio relays
- `src/compositor/` GStreamer pipeline: decode, camera, overlay, encode
- `src/ui/` the on-screen UI: layout, menu, rasteriser, touch routing
- `src/btcall/` the hands-free call bridge between the two Bluetooth radios
- `src/obd/` OBD-II poller and the Lightning request table
- `tests/` one program per subsystem, run by `tools/regress.sh`
