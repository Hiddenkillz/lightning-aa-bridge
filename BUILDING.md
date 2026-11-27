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
**Hardware H.264 encode (Pi 4).** `hw_encode=1` in the config encodes the output with the Pi 4's VideoCore (`v4l2h264enc`) instead of software x264: about half the CPU and less heat at 1080p (measured: 73-115% of 400% against 165-217% for the same load). It needs the GStreamer v4l2 plugin, `libgstvideo4linux2.so` (package `gstreamer1.0-plugins-good`), plus `libgstallocators-1.0.so.0` if you use the bundled runtime; put both in the `--runtime-dir` before installing. The bridge encodes a few test frames at start-up and falls back to x264 (with a log line) if the plugin is missing or the codec will not start. The encoder must be given an explicit H.264 level in the caps (the bridge uses level 4, enough for 1080p30). Without one GStreamer programs level 1.0, the firmware refuses to start the input port at any real picture size and every frame fails (`STREAMON ... No such process`, kernel `Failed enabling i/p port`); this, not buffer sizes or `gpu_mem`, is what stopped the old hardware path. The Pi 5 has no H.264 hardware, so this is Pi 4 only.
`hw_decode=1` (phone video decoded by `v4l2h264dec`) works for Android Auto and Window but drops Split with the Energy panel to ~17 fps (a GStreamer worker thread saturates; probably CPU reads of the decoder's buffers while scaling). Leave it off until that is understood.
**Video size.** The Sync 4 offers 1920x1080, 1280x720 and 800x480; the bridge uses the one matching `video_width` x `video_height`. 1920x1080 with `video_bitrate=8000000` is noticeably sharper than the 1280x720 default. After changing it, delete `/etc/aa-bridge/window.conf` (the cached window of the old size) so the next truck connection learns the new one.
**Stopping.** `systemctl stop aa-bridge` takes about a second. (It used to wait out the stop timeout when no truck was attached, because the USB session thread sat in a blocking read; `Device::stop()` now unbinds the gadget to wake it, and the unit has `TimeoutStopSec=15` as a backstop.)

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
