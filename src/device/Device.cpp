#include "Device.h"
#include "UsbGadget.h"
#include "FunctionFs.h"
#include "../headunit/HeadUnit.h"

#include "MdSession.h"

#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

class Device::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(const Config& config,
         std::shared_ptr<Compositor> compositor,
         std::shared_ptr<HeadUnit> headunit)
        : config_(config)
        , compositor_(compositor)
        , headunit_(headunit)
        , running_(false)
    {}

    void start() {
        running_ = true;
        compositor_->setOutputCallback([this](const uint8_t* data, size_t size, uint64_t pts) {
            sendVideoFrame(data, size, pts);
        });
        io_thread_ = std::thread([this]() { runUsbSession(); });
    }

    void stop() {
        running_ = false;
        {
            std::lock_guard<std::mutex> lk(session_m_);
            if (session_) session_->stop();
        }
        if (io_thread_.joinable()) io_thread_.join();
        UsbGadget::teardown(config_);
    }

    void setTouchCallback(Device::TouchCallback cb) { touch_cb_ = std::move(cb); }
    void setTouchFrameCallback(Device::TouchFrameCallback cb) { touch_frame_cb_ = std::move(cb); }
    void setKeyCallback(Device::KeyCallback cb) { key_cb_ = std::move(cb); }
    void setGeometryCallback(Device::GeometryCallback cb) { geom_cb_ = std::move(cb); }
    void setVisibleWindowCallback(Device::WindowCallback cb) { win_cb_ = std::move(cb); }
    void setVideoParamsCallback(Device::VideoParamsCallback cb) { vp_cb_ = std::move(cb); }
    void setVehicleCallback(Device::VehicleCallback cb) { veh_cb_ = std::move(cb); }
    void setNav(std::shared_ptr<NavRelay> r) { nav_ = std::move(r); }
    void setMedia(std::shared_ptr<RawRelay> r) { media_ = std::move(r); }
    void setAv(std::shared_ptr<AvRelay> r) { av_ = std::move(r); }
    void setCaps(Device::SensorCapsCallback cb) { sensor_caps_cb_ = std::move(cb); }
    void setBtAddr(std::function<std::string()> fn) { bt_addr_fn_ = std::move(fn); }
    void setCarAddr(std::function<void(const std::string&)> fn) { car_addr_cb_ = std::move(fn); }
    void setSensors(Device::SensorTypesCallback t, Device::SensorRawCallback r) { sensor_types_cb_ = std::move(t); sensor_raw_cb_ = std::move(r); }

private:
    // Two-stage USB identity switch: present a believable "default" phone
    // identity first (18d1:4ee1), give Sync 4 up to kDefaultModeTimeoutSec
    // to send the AOA GET_PROTOCOL/SEND_STRING/START handshake against it,
    // then switch to the real accessory identity (18d1:2d00) either on
    // receiving AOA_START or on timeout (best-effort fallback for head
    // units that skip the handshake and expect the accessory PID directly
    // -- this was the ONLY behavior before this change).
    static constexpr int kDefaultModeTimeoutSec = 5;

    void runUsbSession() {
        while (running_) {
            bool structure_ready = false;
            bool ffs_ready = false;
            bool udc_bound = false;
            if (config_.usb_two_stage) {
            // ---------- STAGE 1: DEFAULT MODE ----------
            while (running_ && !structure_ready) {
                structure_ready = UsbGadget::buildStructure(config_, UsbGadget::Profile::DEFAULT_MODE);
                if (!structure_ready) {
                    std::cerr << "[Device] USB gadget structure build (default) failed, retrying in 3s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }
            if (!running_) return;

            auto default_ffs = std::make_shared<FunctionFsGadget>();
            ffs_ready = false;
            while (running_ && !ffs_ready) {
                ffs_ready = default_ffs->activate(config_.usb_ffs_mount);
                if (!ffs_ready) {
                    std::cerr << "[Device] FunctionFS activation (default) failed, retrying in 2s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
            if (!running_) return;

            std::atomic<bool> switch_requested{false};
            default_ffs->setAoaStartCallback([&switch_requested]() { switch_requested = true; });

            udc_bound = false;
            while (running_ && !udc_bound) {
                udc_bound = UsbGadget::bindUdc(config_);
                if (!udc_bound) {
                    std::cerr << "[Device] UDC bind (default) failed, retrying in 3s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }
            if (!running_) return;

            std::cout << "[Device] Default-mode gadget live (18d1:4ee1). Waiting up to "
                       << kDefaultModeTimeoutSec << "s for Sync 4's AOA START..." << std::endl;

            auto wait_start = std::chrono::steady_clock::now();
            while (running_ && !switch_requested) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - wait_start).count();
                if (elapsed >= kDefaultModeTimeoutSec) {
                    std::cout << "[Device] No AOA START within " << kDefaultModeTimeoutSec
                               << "s, switching to accessory mode anyway (best-effort)." << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) return;

            // ---------- STAGE 2: ACCESSORY MODE ----------
            std::cout << "[Device] Switching default -> accessory mode..." << std::endl;
            // Unbind FIRST: that delivers DISABLE/UNBIND events on ep0, which is
            // what wakes the event thread out of its blocking read(). Closing the
            // fd from another thread does NOT wake a blocked read on Linux, so
            // deactivate()-then-unbind would hang forever in join().
            UsbGadget::unbindUdc(config_);
            default_ffs->deactivate();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the host see the disconnect
            } else {
                std::cout << "[Device] usb_two_stage=0: presenting accessory PID directly" << std::endl;
            }

            structure_ready = false;
            while (running_ && !structure_ready) {
                structure_ready = UsbGadget::buildStructure(config_, UsbGadget::Profile::ACCESSORY_MODE);
                if (!structure_ready) {
                    std::cerr << "[Device] USB gadget structure build (accessory) failed, retrying in 3s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }
            if (!running_) return;

            FunctionFsGadget ffs;
            ffs_ready = false;
            while (running_ && !ffs_ready) {
                ffs_ready = ffs.activate(config_.usb_ffs_mount);
                if (!ffs_ready) {
                    std::cerr << "[Device] FunctionFS activation (accessory) failed, retrying in 2s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
            if (!running_) return;

            udc_bound = false;
            while (running_ && !udc_bound) {
                udc_bound = UsbGadget::bindUdc(config_);
                if (!udc_bound) {
                    std::cerr << "[Device] UDC bind (accessory) failed, retrying in 3s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }
            if (!running_) return;

            std::cout << "[Device] Accessory-mode USB gadget + FunctionFS live. "
                         "Waiting for Sync 4 bulk I/O..." << std::endl;
            while (running_) {
                int out_fd = ffs.openBulkOut();   // host->device, we READ this
                int in_fd  = ffs.openBulkIn();    // device->host, we WRITE this
                if (out_fd < 0 || in_fd < 0) {
                    if (out_fd >= 0) ::close(out_fd);
                    if (in_fd >= 0) ::close(in_fd);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                std::cout << "[Device] Sync 4 connected (bulk endpoints open)" << std::endl;
                const bool silent = runAaSession(out_fd, in_fd);
                ::close(out_fd);
                ::close(in_fd);
                if (silent && running_) {
                    // Sync 4 stopped talking but never dropped the link (engine start, truck 29). Do what unplugging and replugging the cable does: pull the
                    // gadget off the bus, tear it down, and let the outer loop run the whole cold-start sequence again (default identity -> accessory).
                    ++reattach_count_;
                    const int pause_s = 2 + std::min(10, reattach_count_ - 1) * 2;      // 2 s, then a little longer each time so a dead port is not hammered
                    std::cout << "[Device] truck silent: re-attaching USB (#" << reattach_count_ << ", " << pause_s << " s off the bus)" << std::endl;
                    UsbGadget::unbindUdc(config_);
                    ffs.deactivate();
                    for (int i = 0; i < pause_s * 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;                                                       // back to the top of the outer loop = a fresh enumeration
                }
                if (running_) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    int reattach_count_ = 0;

    // Returns true when the session ended because the truck went silent (the caller then re-attaches USB).
    bool runAaSession(int read_fd, int write_fd) {
        auto session = std::make_shared<MdSession>(config_, read_fd, write_fd);
        session->setTouchHandler(touch_cb_);
        session->setTouchFrameHandler(touch_frame_cb_);
        session->setKeyHandler(key_cb_);
        session->onTouchGeometry = geom_cb_;
        session->onVisibleWindow = win_cb_;
        session->onVideoParams = vp_cb_;
        session->onVehicle = veh_cb_;
        session->btAddress = bt_addr_fn_; session->onCarAddress = car_addr_cb_; session->onSensorCaps = sensor_caps_cb_; session->onSensorTypes = sensor_types_cb_; session->onSensorRaw = sensor_raw_cb_;
        session->onDead = [this]() { std::cout << "[Device] truck declared dead: unbinding the USB gadget to wake the blocked endpoint I/O" << std::endl; UsbGadget::unbindUdc(config_); };
        if (nav_) {
            std::weak_ptr<MdSession> ws = session; auto r = nav_;
            session->onNav = [r](uint16_t id, const std::string& b) { r->fromTruck(id, b); };
            session->onNavChannel = [r, ws](bool open) {
                if (!open) return;
                r->setTruckSend([ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendNav(id, b); });
                r->setTruckFocusSend([ws](const std::string& b) { if (auto x = ws.lock()) x->sendNavFocus(b); });
            };
        }
        if (media_) {
            std::weak_ptr<MdSession> ws = session; auto r = media_;
            session->onMedia = [r](uint16_t id, const std::string& b) { r->fromTruck(id, b); };
            session->onMediaChannel = [r, ws](bool open) { if (open) r->setTruckSend([ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendMedia(id, b); }); };
        }
        if (av_) {
            std::weak_ptr<MdSession> ws = session; auto r = av_;
            session->onMicData = [r](uint64_t ts, const uint8_t* p, size_t n) { r->micDataFromTruck(ts, p, n); };
            session->onControlRaw = [r](uint16_t id, const std::string& b) { r->controlFromTruck(id, b); };
            r->setTruck([ws](int s, int se) { if (auto x = ws.lock()) x->startAudio(s, se); },
                        [ws](int s) { if (auto x = ws.lock()) x->stopAudio(s); },
                        [ws](int s, uint64_t ts, const uint8_t* p, size_t n) { if (auto x = ws.lock()) x->sendAudio(s, ts, p, n); },
                        [ws](bool o) { if (auto x = ws.lock()) x->micRequest(o); },
                        [ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendControl(id, b); });
        }
        { std::lock_guard<std::mutex> lk(session_m_); session_ = session; }
        session->run();
        if (nav_) { nav_->setTruckSend({}); nav_->setTruckFocusSend({}); }
        if (media_) media_->setTruckSend({});
        if (av_) av_->clearTruck();                       // blocks for the life of the AA session
        const bool silent = session->endedBySilence();
        { std::lock_guard<std::mutex> lk(session_m_); session_.reset(); }
        return silent;
    }

    void sendVideoFrame(const uint8_t* data, size_t size, uint64_t pts_us) {
        std::shared_ptr<MdSession> s;
        { std::lock_guard<std::mutex> lk(session_m_); s = session_; }
        if (s) s->sendVideo(data, size, pts_us);
    }

    Config config_;
    std::shared_ptr<Compositor> compositor_;
    std::shared_ptr<HeadUnit> headunit_;
    std::atomic<bool> running_;
    std::thread io_thread_;
    Device::TouchCallback touch_cb_;
    Device::TouchFrameCallback touch_frame_cb_;
    Device::KeyCallback key_cb_;
    Device::GeometryCallback geom_cb_;
    Device::WindowCallback win_cb_;
    Device::VideoParamsCallback vp_cb_;
    Device::VehicleCallback veh_cb_;
    std::function<void(const std::string&)> car_addr_cb_; std::function<std::string()> bt_addr_fn_; Device::SensorCapsCallback sensor_caps_cb_; Device::SensorTypesCallback sensor_types_cb_; Device::SensorRawCallback sensor_raw_cb_;
    std::shared_ptr<NavRelay> nav_; std::shared_ptr<RawRelay> media_;
    std::shared_ptr<AvRelay> av_;

    std::mutex session_m_;
    std::shared_ptr<MdSession> session_;
};

Device::Device(const Config& config,
               std::shared_ptr<Compositor> compositor,
               std::shared_ptr<HeadUnit> headunit)
    : impl_(std::make_shared<Impl>(config, compositor, headunit)) {}

Device::~Device() = default;
void Device::start() { impl_->start(); }
void Device::stop()  { impl_->stop(); }
void Device::setTouchCallback(TouchCallback cb) { impl_->setTouchCallback(std::move(cb)); }
void Device::setTouchFrameCallback(TouchFrameCallback cb) { impl_->setTouchFrameCallback(std::move(cb)); }
void Device::setKeyCallback(KeyCallback cb) { impl_->setKeyCallback(std::move(cb)); }
void Device::setGeometryCallback(GeometryCallback cb) { impl_->setGeometryCallback(std::move(cb)); }
void Device::setVisibleWindowCallback(WindowCallback cb) { impl_->setVisibleWindowCallback(std::move(cb)); }
void Device::setVideoParamsCallback(VideoParamsCallback cb) { impl_->setVideoParamsCallback(std::move(cb)); }
void Device::setVehicleCallback(VehicleCallback cb) { impl_->setVehicleCallback(std::move(cb)); }
void Device::setCarAddressCallback(std::function<void(const std::string&)> fn) { impl_->setCarAddr(std::move(fn)); }
void Device::setBtAddressProvider(std::function<std::string()> fn) { impl_->setBtAddr(std::move(fn)); }
void Device::setSensorCaps(SensorCapsCallback cb) { impl_->setCaps(std::move(cb)); }
void Device::setSensorRelay(SensorTypesCallback t, SensorRawCallback r) { impl_->setSensors(std::move(t), std::move(r)); }
void Device::setAvRelay(std::shared_ptr<AvRelay> r) { impl_->setAv(std::move(r)); }
void Device::setNavRelay(std::shared_ptr<NavRelay> r) { impl_->setNav(std::move(r)); }
void Device::setMediaRelay(std::shared_ptr<RawRelay> r) { impl_->setMedia(std::move(r)); }
