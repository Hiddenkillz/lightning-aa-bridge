#pragma once
// FunctionFs: implements the userspace side of the Linux FunctionFS gadget
// protocol, which we need because this kernel does NOT have the Android-
// specific CONFIG_USB_CONFIGFS_F_ACC driver (that's an out-of-tree Android
// kernel patch, not present in Raspberry Pi OS's mainline-derived kernel).
//
// FunctionFS requires a userspace program to:
//   1. open ep0
//   2. write a "descriptors" blob (full-speed + high-speed descriptor sets
//      for our interface + 2 bulk endpoints)
//   3. write a "strings" blob
//   4. KEEP ep0 OPEN for the lifetime of the function — closing it tears
//      the function down.
//   5. only then may ep1 (bulk OUT, host->device) and ep2 (bulk IN,
//      device->host) be opened and used for real data.
//
// Without step 1-4 ever happening (which was the actual root cause of
// "USB device is not responding"/-110 timeouts), the kernel has no valid
// descriptor set to answer ANY USB request with — not just AOA/AA ones.
//
// Struct layouts below are the stable FunctionFS UAPI (unchanged since
// ~2015, see <linux/usb/functionfs.h> / <linux/usb/ch9.h> upstream) —
// hand-rolled here instead of depending on those headers being present
// in the cross-build sysroot.
//
// Also implements a continuous ep0 event-read loop, answering AOA's vendor
// control requests (GET_PROTOCOL/SEND_STRING/START) per the official spec
// — without this, any control request the kernel can't auto-handle stalls
// the endpoint permanently once a real AOA-aware host (not a generic PC)
// sends one.

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <atomic>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>

namespace ffs_proto {

#pragma pack(push, 1)

struct DescsHeadV2 {
    uint32_t magic;
    uint32_t length;
    uint32_t flags;
};

struct CtrlRequest {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

struct Event {
    union {
        CtrlRequest setup;
        uint8_t raw[8];
    } u;
    uint8_t type;
    uint8_t _pad[3];
};

constexpr uint8_t FUNCTIONFS_BIND    = 0;
constexpr uint8_t FUNCTIONFS_UNBIND  = 1;
constexpr uint8_t FUNCTIONFS_ENABLE  = 2;
constexpr uint8_t FUNCTIONFS_DISABLE = 3;
constexpr uint8_t FUNCTIONFS_SETUP   = 4;
constexpr uint8_t FUNCTIONFS_SUSPEND = 5;
constexpr uint8_t FUNCTIONFS_RESUME  = 6;

constexpr uint8_t USB_DIR_IN = 0x80;

constexpr uint8_t AOA_GET_PROTOCOL = 51;
constexpr uint8_t AOA_SEND_STRING  = 52;
constexpr uint8_t AOA_START        = 53;
constexpr uint8_t AOA_REGISTER_HID = 54;
constexpr uint8_t AOA_UNREGISTER_HID = 55;
constexpr uint8_t AOA_SET_HID_REPORT_DESC = 56;
constexpr uint8_t AOA_SEND_HID_EVENT = 57;
constexpr uint8_t AOA_SET_AUDIO_MODE = 58;

struct InterfaceDesc {
    uint8_t  bLength = 9;
    uint8_t  bDescriptorType = 4;
    uint8_t  bInterfaceNumber = 0;
    uint8_t  bAlternateSetting = 0;
    uint8_t  bNumEndpoints = 2;
    uint8_t  bInterfaceClass = 0xFF;
    uint8_t  bInterfaceSubClass = 0xFF;
    uint8_t  bInterfaceProtocol = 0;
    uint8_t  iInterface = 1;
};

struct EndpointDescNoAudio {
    uint8_t  bLength = 7;
    uint8_t  bDescriptorType = 5;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes = 2;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval = 0;
};

struct StringsHead {
    uint32_t magic;
    uint32_t length;
    uint32_t str_count;
    uint32_t lang_count;
};

#pragma pack(pop)

constexpr uint32_t FUNCTIONFS_DESCRIPTORS_MAGIC_V2 = 3;
constexpr uint32_t FUNCTIONFS_STRINGS_MAGIC        = 2;
constexpr uint32_t FUNCTIONFS_HAS_FS_DESC = 1;
constexpr uint32_t FUNCTIONFS_HAS_HS_DESC = 2;

inline std::vector<uint8_t> buildDescriptors() {
    InterfaceDesc iface;

    // Declaration order defines ep1/ep2 file naming. Android's real
    // f_accessory declares IN first (host sees 0x81 IN, 0x02 OUT) and AACS
    // matched that against a real car; so do we. ep1 = IN (we write),
    // ep2 = OUT (we read).
    EndpointDescNoAudio ep_in_fs;
    ep_in_fs.bEndpointAddress = 0x81;
    ep_in_fs.wMaxPacketSize = 64;

    EndpointDescNoAudio ep_out_fs;
    ep_out_fs.bEndpointAddress = 0x02;
    ep_out_fs.wMaxPacketSize = 64;

    EndpointDescNoAudio ep_out_hs = ep_out_fs;
    ep_out_hs.wMaxPacketSize = 512;

    EndpointDescNoAudio ep_in_hs = ep_in_fs;
    ep_in_hs.wMaxPacketSize = 512;

    std::vector<uint8_t> fs_block;
    fs_block.insert(fs_block.end(), (uint8_t*)(void*)&iface, (uint8_t*)(void*)&iface + sizeof(iface));
    fs_block.insert(fs_block.end(), (uint8_t*)(void*)&ep_in_fs, (uint8_t*)(void*)&ep_in_fs + sizeof(ep_in_fs));
    fs_block.insert(fs_block.end(), (uint8_t*)(void*)&ep_out_fs, (uint8_t*)(void*)&ep_out_fs + sizeof(ep_out_fs));

    std::vector<uint8_t> hs_block;
    hs_block.insert(hs_block.end(), (uint8_t*)(void*)&iface, (uint8_t*)(void*)&iface + sizeof(iface));
    hs_block.insert(hs_block.end(), (uint8_t*)(void*)&ep_in_hs, (uint8_t*)(void*)&ep_in_hs + sizeof(ep_in_hs));
    hs_block.insert(hs_block.end(), (uint8_t*)(void*)&ep_out_hs, (uint8_t*)(void*)&ep_out_hs + sizeof(ep_out_hs));

    uint32_t total_len = sizeof(DescsHeadV2) + 4 + 4
                        + (uint32_t)fs_block.size() + (uint32_t)hs_block.size();

    DescsHeadV2 head;
    head.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
    head.length = total_len;
    head.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;

    uint32_t fs_count = 3;
    uint32_t hs_count = 3;

    std::vector<uint8_t> out;
    out.insert(out.end(), (uint8_t*)(void*)&head, (uint8_t*)(void*)&head + sizeof(head));
    out.insert(out.end(), (uint8_t*)(void*)&fs_count, (uint8_t*)(void*)&fs_count + 4);
    out.insert(out.end(), (uint8_t*)(void*)&hs_count, (uint8_t*)(void*)&hs_count + 4);
    out.insert(out.end(), fs_block.begin(), fs_block.end());
    out.insert(out.end(), hs_block.begin(), hs_block.end());
    return out;
}

inline std::vector<uint8_t> buildStrings(const std::string& iface_name) {
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), iface_name.begin(), iface_name.end());
    payload.push_back(0);

    uint32_t total_len = sizeof(StringsHead) + 2 + (uint32_t)payload.size();

    StringsHead head;
    head.magic = FUNCTIONFS_STRINGS_MAGIC;
    head.length = total_len;
    head.str_count = 1;
    head.lang_count = 1;

    uint16_t lang = 0x0409;

    std::vector<uint8_t> out;
    out.insert(out.end(), (uint8_t*)(void*)&head, (uint8_t*)(void*)&head + sizeof(head));
    out.insert(out.end(), (uint8_t*)(void*)&lang, (uint8_t*)(void*)&lang + 2);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace ffs_proto

class FunctionFsGadget {
public:
    bool activate(const std::string& mount_point) {
        std::string ep0_path = mount_point + "/ep0";
        ep0_fd_ = ::open(ep0_path.c_str(), O_RDWR);
        if (ep0_fd_ < 0) {
            std::cerr << "[FunctionFs] open(" << ep0_path << ") failed: "
                      << strerror(errno) << std::endl;
            return false;
        }

        auto descs = ffs_proto::buildDescriptors();
        if (::write(ep0_fd_, descs.data(), descs.size()) != (ssize_t)descs.size()) {
            std::cerr << "[FunctionFs] writing descriptors to ep0 failed: "
                      << strerror(errno) << std::endl;
            ::close(ep0_fd_);
            ep0_fd_ = -1;
            return false;
        }

        auto strs = ffs_proto::buildStrings("AA Bridge Accessory");
        if (::write(ep0_fd_, strs.data(), strs.size()) != (ssize_t)strs.size()) {
            std::cerr << "[FunctionFs] writing strings to ep0 failed: "
                      << strerror(errno) << std::endl;
            ::close(ep0_fd_);
            ep0_fd_ = -1;
            return false;
        }

        std::cout << "[FunctionFs] Activated: descriptors+strings written, "
                     "ep0 held open." << std::endl;
        mount_point_ = mount_point;

        stop_event_loop_ = false;
        event_thread_ = std::thread([this]() { eventLoop(); });

        return true;
    }

    // Called from the ep0 event-loop thread the moment an AOA_START
    // vendor control request arrives -- lets Device.cpp drive a
    // profile switch (default -> accessory) instead of a silent no-op.
    void setAoaStartCallback(std::function<void()> cb) { on_aoa_start_ = std::move(cb); }

    int openBulkIn()  { return ::open((mount_point_ + "/ep1").c_str(), O_WRONLY); }  // IN: device->host
    int openBulkOut() { return ::open((mount_point_ + "/ep2").c_str(), O_RDONLY); }  // OUT: host->device

    void deactivate() {
        // Order matters: signal, JOIN (the loop polls with a timeout so it
        // notices), and only then close ep0. Closing the fd first does not
        // wake a thread blocked in read() and join() would hang forever --
        // that hung the default->accessory switch on real hardware.
        stop_event_loop_ = true;
        if (event_thread_.joinable()) event_thread_.join();
        if (ep0_fd_ >= 0) {
            ::close(ep0_fd_);
            ep0_fd_ = -1;
        }
    }

    ~FunctionFsGadget() { deactivate(); }

private:
    void eventLoop() {
        while (!stop_event_loop_) {
            struct pollfd pfd{ep0_fd_, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 200);
            if (pr == 0) continue;
            if (pr < 0) { if (errno == EINTR) continue; break; }
            ffs_proto::Event ev;
            ssize_t n = ::read(ep0_fd_, &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) {
                if (stop_event_loop_) break;
                if (errno == EINTR) continue;
                std::cerr << "[FunctionFs] ep0 event read failed/short ("
                          << n << "): " << strerror(errno) << std::endl;
                break;
            }
            handleEvent(ev);
        }
    }

    void handleEvent(const ffs_proto::Event& ev) {
        using namespace ffs_proto;
        switch (ev.type) {
            case FUNCTIONFS_BIND:    std::cout << "[FunctionFs] event: BIND" << std::endl; break;
            case FUNCTIONFS_UNBIND:  std::cout << "[FunctionFs] event: UNBIND" << std::endl; break;
            case FUNCTIONFS_ENABLE:  std::cout << "[FunctionFs] event: ENABLE" << std::endl; break;
            case FUNCTIONFS_DISABLE: std::cout << "[FunctionFs] event: DISABLE" << std::endl; break;
            case FUNCTIONFS_SUSPEND: std::cout << "[FunctionFs] event: SUSPEND" << std::endl; break;
            case FUNCTIONFS_RESUME:  std::cout << "[FunctionFs] event: RESUME" << std::endl; break;
            case FUNCTIONFS_SETUP:   handleSetup(ev.u.setup); break;
            default:
                std::cout << "[FunctionFs] event: unknown type " << (int)ev.type << std::endl;
        }
    }

    void handleSetup(const ffs_proto::CtrlRequest& req) {
        using namespace ffs_proto;
        bool is_in = (req.bRequestType & USB_DIR_IN) != 0;

        std::cout << "[FunctionFs] SETUP bRequestType=0x" << std::hex << (int)req.bRequestType
                   << " bRequest=" << std::dec << (int)req.bRequest
                   << " wValue=" << req.wValue << " wIndex=" << req.wIndex
                   << " wLength=" << req.wLength << std::endl;

        switch (req.bRequest) {
            case AOA_GET_PROTOCOL: {
                uint16_t version = 2;
                ::write(ep0_fd_, &version, sizeof(version));
                std::cout << "[FunctionFs] answered GET_PROTOCOL -> v" << version << std::endl;
                return;
            }
            case AOA_SEND_STRING: {
                if (req.wLength > 0) {
                    std::vector<uint8_t> buf(req.wLength);
                    ::read(ep0_fd_, buf.data(), buf.size());
                }
                return;
            }
            case AOA_START: {
                std::cout << "[FunctionFs] AOA START received" << std::endl;
                // No data stage: a zero-length read on ep0 completes the
                // status stage (ACK). Without it, the next event read()
                // STALLs this request and the host sees START fail.
                ::read(ep0_fd_, nullptr, 0);
                if (on_aoa_start_) on_aoa_start_();
                return;
            }
            default:
                break;
        }

        if (!is_in && req.wLength > 0) {
            std::vector<uint8_t> buf(req.wLength);
            ::read(ep0_fd_, buf.data(), buf.size());
        } else if (is_in && req.wLength > 0) {
            std::vector<uint8_t> buf(req.wLength, 0);
            ::write(ep0_fd_, buf.data(), buf.size());
        } else if (!is_in) {
            ::read(ep0_fd_, nullptr, 0);   // ACK zero-length OUT
        } else {
            ::write(ep0_fd_, nullptr, 0);  // ACK zero-length IN
        }
    }

    int ep0_fd_ = -1;
    std::string mount_point_;
    std::function<void()> on_aoa_start_;
    std::thread event_thread_;
    std::atomic<bool> stop_event_loop_{false};
};
