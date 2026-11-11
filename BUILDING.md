# Building and installing

Read the first section of `README.md` first: without an Android Auto **device** certificate the truck-facing half will not start. This document assumes you understand that and still want to build, read, or port the code.

## What you need
- Raspberry Pi 4 running Raspberry Pi OS (Bookworm, 64-bit). Kernel 6.6 is what the author runs.
- Packages: `cmake g++ pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libssl-dev libprotobuf-dev protobuf-compiler libboost-all-dev libbluetooth-dev libglib2.0-dev`
- [aasdk](https://github.com/f1xpl/aasdk) built and installed on the Pi (or in your cross sysroot); `CMakeLists.txt` finds it with `find_package(aasdk)`.
- A USB-C cable to the vehicle's Android Auto port (it powers the Pi), a UVC camera, two USB Bluetooth adapters, optionally a vLinker FS for OBD-II.

## Build
```
mkdir build && cd build
cmake .. && make -j4
```
The author cross-builds on an x86 host with a copied Pi sysroot and runs the test suite under `qemu-aarch64-static`; `tools/regress.sh` is that suite. Every test that touches the AA protocol takes certificate paths as arguments, which is why you cannot run the full suite without the certificates.

## Install on a card
`tools/deploy-card.sh <binary> --scripts --md-cert-dir <dir with md_cert.pem + md_key.pem> [--set key=value ...]` copies the binary, scripts, systemd units and config onto an already-imaged card mounted from this host. Read the script; it is short and every option is commented. Configuration keys and defaults are in `src/Config.h`.

## Layout
- `src/headunit/` the head-unit role (phone side): Wi-Fi AP, Bluetooth bootstrap, AA session with the phone
- `src/device/` the phone role (vehicle side): USB gadget, AA session with the head unit, sensor/audio relays
- `src/compositor/` GStreamer pipeline: decode, camera, overlay, encode
- `src/ui/` the on-screen UI: layout, menu, rasteriser, touch routing
- `src/btcall/` the hands-free call bridge between the two Bluetooth radios
- `src/obd/` OBD-II poller and the Lightning request table
- `tests/` one program per subsystem, run by `tools/regress.sh`
