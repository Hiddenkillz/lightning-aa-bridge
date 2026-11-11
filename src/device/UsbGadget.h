#pragma once
#include "../Config.h"
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

// UsbGadget: configures Linux USB gadget (configfs) so the Pi
// presents to Sync 4 as a Google Android device.
//
// TWO-STAGE SWITCH (new): some head units (per aawgd/aa-proxy-rs
// --legacy prior art and a real-world AAWirelessDongle compatibility
// report) expect to see a believable "default"/non-accessory phone
// identity FIRST, send the AOA GET_PROTOCOL/SEND_STRING/START vendor
// handshake, and only THEN expect the device to re-enumerate as the
// real AOA accessory PID. Presenting the accessory PID from power-on
// (the original single-stage approach) is what aa-proxy-rs still does
// by default and most head units tolerate it fine -- but Sync 4's
// behavior is unconfirmed, so this gadget now supports both profiles
// and Device.cpp drives the two-stage sequence with a timeout fallback.
//
// DEFAULT_MODE:    VID 0x18d1 / PID 0x4ee1 (Google Pixel MTP identity --
//                   this is the SAME PID that was wrongly used everywhere
//                   as the accessory PID in earlier builds; it is CORRECT
//                   here, for this specific purpose: a believable non-AOA
//                   phone identity for the pre-handshake stage).
// ACCESSORY_MODE:  VID 0x18d1 / PID 0x2d00 (AOA "already in accessory
//                   mode", 1 interface, 2 bulk EPs) -- per the official
//                   AOA spec (source.android.com/docs/core/interaction/
//                   accessories/aoa). This is what was previously the
//                   ONLY profile; it is still used as the final stage.
//
// Requires: CONFIG_USB_GADGET, CONFIG_USB_CONFIGFS, and the
// dwc2 overlay enabled in /boot/config.txt:
//   dtoverlay=dwc2,dr_mode=peripheral
//
// KNOWN GOTCHAS (hit in testing):
// 1. dwc2 can take a moment after module load to register as a UDC in
//    /sys/class/udc/ — binding too early fails with ENODEV.
// 2. If setup() is retried after a failed attempt WITHOUT tearing down
//    first, leftover symlinks/config from the previous attempt cause
//    secondary errors ("File exists") that mask the real problem. setup()
//    now force-cleans its own state before building it again, every call.
// 3. Never trust success from a single syscall return — the kernel-side
//    UDC core can reject a bind asynchronously. Always verify by reading
//    the UDC file back after binding.

class UsbGadget {
public:
    enum class Profile { DEFAULT_MODE, ACCESSORY_MODE };

    // Builds the configfs gadget structure and mounts FunctionFS, but does
    // NOT bind the UDC — binding must wait until FunctionFS's ep0 has had
    // its descriptors written (see FunctionFsGadget::activate), otherwise
    // the host sees D+ pull-up and starts enumerating before the function
    // has anything valid to answer with. That ordering bug was the root
    // cause of the earlier "-110 / not responding" symptoms.
    static bool buildStructure(const Config& config, Profile profile) {
        const std::string base = config.usb_gadget_dir;

        // --- Always force-clean before (re)building, so retries/profile
        //     switches are safe ---
        forceCleanup(config);

        run("modprobe libcomposite");
        run("mount -t configfs none /sys/kernel/config 2>/dev/null || true");

        run("mkdir -p " + base);

        if (profile == Profile::DEFAULT_MODE) {
            write(base + "/idVendor",  "0x18d1");
            write(base + "/idProduct", "0x4ee1");
            std::cout << "[UsbGadget] Building DEFAULT_MODE profile "
                         "(18d1:4ee1, pre-AOA-handshake identity)" << std::endl;
        } else {
            write(base + "/idVendor",  "0x18d1");
            write(base + "/idProduct", "0x2d00");
            std::cout << "[UsbGadget] Building ACCESSORY_MODE profile "
                         "(18d1:2d00, AOA accessory)" << std::endl;
        }
        write(base + "/bcdDevice", "0x0100");
        write(base + "/bcdUSB",    "0x0200");

        run("mkdir -p " + base + "/strings/0x409");
        if (profile == Profile::DEFAULT_MODE) {
            write(base + "/strings/0x409/manufacturer", "Google Inc.");
            write(base + "/strings/0x409/product",      "Pixel");
            write(base + "/strings/0x409/serialnumber", "AABRIDGE0001");
        } else {
            write(base + "/strings/0x409/manufacturer", "Google Inc.");
            write(base + "/strings/0x409/product",      "Android Auto");
            write(base + "/strings/0x409/serialnumber", "AA-BRIDGE-001");
        }

        run("mkdir -p " + base + "/configs/c.1/strings/0x409");
        write(base + "/configs/c.1/strings/0x409/configuration", "AA Bridge");
        write(base + "/configs/c.1/MaxPower", "500");

        run("mkdir -p " + base + "/functions/ffs.accessory");
        run("mkdir -p " + config.usb_ffs_mount + " 2>/dev/null || true");
        run("mount -t functionfs accessory " + config.usb_ffs_mount + " 2>/dev/null || true");
        run("ln -sf " + base + "/functions/ffs.accessory " + base + "/configs/c.1/ffs.accessory");

        // --- Diagnostic: dump everything /sys/class/udc/ actually has ---
        dumpUdcClass();

        // --- Confirm the configured UDC name exists (doesn't bind yet) ---
        const std::string udc_sysfs = "/sys/class/udc/" + config.udc_name;
        bool udc_present = false;
        for (int i = 0; i < 20; ++i) {  // up to ~10s
            if (access(udc_sysfs.c_str(), F_OK) == 0) {
                udc_present = true;
                break;
            }
            std::cerr << "[UsbGadget] UDC " << config.udc_name
                      << " not present yet, waiting... (" << i << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!udc_present) {
            std::cerr << "[UsbGadget] ERROR: UDC " << config.udc_name
                      << " never appeared in /sys/class/udc/ — giving up this attempt."
                      << std::endl;
            dumpUdcClass();
            return false;
        }
        return true;
    }

    // Call ONLY after FunctionFS ep0 has descriptors+strings written and
    // is held open — see FunctionFsGadget::activate(). Returns true only
    // if the UDC bind was verified to actually succeed.
    static bool bindUdc(const Config& config) {
        const std::string base = config.usb_gadget_dir;

        bool bound = false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (writeChecked(base + "/UDC", config.udc_name)) {
                bound = true;
                break;
            }
            std::cerr << "[UsbGadget] UDC bind attempt " << attempt
                      << " failed, retrying in 1s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!bound) {
            std::cerr << "[UsbGadget] ERROR: Failed to bind to UDC "
                      << config.udc_name << " after all retries this attempt. "
                      << "Gadget is NOT live — Sync 4 will see nothing." << std::endl;
            dumpUdcClass();
            return false;
        }

        // --- Verify the bind actually stuck ---
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string bound_value = readFile(base + "/UDC");
        if (bound_value.empty()) {
            std::cerr << "[UsbGadget] ERROR: UDC file is empty after bind — "
                      << "gadget did not actually attach." << std::endl;
            return false;
        }

        std::cout << "[UsbGadget] Verified bound to " << bound_value << std::endl;
        return true;
    }

    // Unbind the UDC only (keeps configfs structure + FunctionFS mount
    // intact) -- used mid-profile-switch when the FunctionFS side will be
    // deactivated and rebuilt separately by the caller.
    static void unbindUdc(const Config& config) {
        const std::string base = config.usb_gadget_dir;
        write(base + "/UDC", "\n");
        std::cout << "[UsbGadget] UDC unbound (profile switch)" << std::endl;
    }

    static void teardown(const Config& config) {
        forceCleanup(config);
        std::cout << "[UsbGadget] Torn down" << std::endl;
    }

private:
    // Idempotent cleanup: safe to call whether or not a previous attempt
    // left partial/stale state. Every step is best-effort.
    static void forceCleanup(const Config& config) {
        const std::string base = config.usb_gadget_dir;
        write(base + "/UDC", "\n");
        run("rm -f " + base + "/configs/c.1/ffs.accessory 2>/dev/null || true");
        run("umount " + config.usb_ffs_mount + " 2>/dev/null || true");
        run("rmdir " + base + "/functions/ffs.accessory 2>/dev/null || true");
        run("rmdir " + base + "/configs/c.1/strings/0x409 2>/dev/null || true");
        run("rmdir " + base + "/configs/c.1 2>/dev/null || true");
        run("rmdir " + base + "/strings/0x409 2>/dev/null || true");
        run("rmdir " + base + " 2>/dev/null || true");
    }

    static void run(const std::string& cmd) {
        int ret = system(cmd.c_str());
        (void)ret; // best-effort for mkdir/mount/mount-cleanup/rm steps
    }

    // Fire-and-forget write for non-critical config fields (strings, IDs).
    static void write(const std::string& path, const std::string& value) {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) {
            // Don't spam errors for cleanup paths that legitimately don't exist yet
            return;
        }
        fputs(value.c_str(), f);
        fclose(f);
    }

    // Checked write using raw write(2) so kernel errno (e.g. ENODEV for a
    // busy/missing UDC) is actually observed instead of silently dropped.
    static bool writeChecked(const std::string& path, const std::string& value) {
        int fd = open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            std::cerr << "[UsbGadget] open(" << path << ") failed: "
                       << strerror(errno) << std::endl;
            return false;
        }
        ssize_t n = ::write(fd, value.c_str(), value.size());
        int err = errno;
        close(fd);
        if (n < 0) {
            std::cerr << "[UsbGadget] write(" << path << ", " << value
                       << ") failed: " << strerror(err) << std::endl;
            return false;
        }
        return true;
    }

    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return "";
        std::string line;
        std::getline(f, line);
        return line;
    }

    // Dumps every entry under /sys/class/udc/ and, for each, every readable
    // attribute file — tells us definitively what the kernel thinks exists,
    // regardless of what name we assumed in Config.
    static void dumpUdcClass() {
        std::cerr << "[UsbGadget] --- /sys/class/udc/ dump ---" << std::endl;
        DIR* d = opendir("/sys/class/udc/");
        if (!d) {
            std::cerr << "[UsbGadget] /sys/class/udc/ does not exist or cannot be opened: "
                      << strerror(errno) << std::endl;
            return;
        }
        bool any = false;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            any = true;
            std::string udc_path = "/sys/class/udc/" + name;
            std::cerr << "[UsbGadget]   UDC entry: " << name << std::endl;
            std::string state = readFile(udc_path + "/state");
            if (!state.empty()) {
                std::cerr << "[UsbGadget]     state: " << state << std::endl;
            }
            std::string function = readFile(udc_path + "/function");
            if (!function.empty()) {
                std::cerr << "[UsbGadget]     function (currently bound): " << function << std::endl;
            }
        }
        closedir(d);
        if (!any) {
            std::cerr << "[UsbGadget]   (empty — no UDC entries registered at all)" << std::endl;
        }
        std::cerr << "[UsbGadget] --- end dump ---" << std::endl;
    }
};
