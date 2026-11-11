#include "MdSession.h"
#include <iterator>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <cstdio>
#include "AnnexB.h"
#include "../Trace.h"
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/Gear.pb.h>
#include <aap_protobuf/service/sensorsource/message/GearData.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h>
#include <aap_protobuf/service/sensorsource/message/ParkingBrakeData.pb.h>
#include <aap_protobuf/service/sensorsource/message/SpeedData.pb.h>
#include <aap_protobuf/service/sensorsource/message/NightModeData.pb.h>
#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/PointerAction.pb.h>

#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/ByeByeRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/shared/message/Start.pb.h>
#include <aap_protobuf/service/media/shared/message/Stop.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/dh.h>

#include <chrono>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <unistd.h>
#include <poll.h>

using aap_protobuf::service::control::message::ControlMessageType;
namespace ctl = aap_protobuf::service::control::message;
namespace med = aap_protobuf::service::media;

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xff); }
static inline void wr32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static inline void wr64(std::vector<uint8_t>& v, uint64_t x) { wr32(v, (uint32_t)(x >> 32)); wr32(v, (uint32_t)x); }

static std::string hexs(const uint8_t* d, size_t n, size_t max=24){ std::string s; char b[4]; for(size_t i=0;i<n&&i<max;++i){snprintf(b,sizeof b,"%02x",d[i]); s+=b;} if(n>max) s+="..."; return s; }
static long msNow(){ static auto t0=std::chrono::steady_clock::now(); return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count(); }
static void dumpQuietForensics(int64_t silent_ms);
#define LOG(x) do { std::cout << "[MdSession] " << x << std::endl; } while (0)
#define ERR(x) do { std::cerr << "[MdSession] " << x << std::endl; } while (0)

MdSession::MdSession(const Config& config, int read_fd, int write_fd)
    : config_(config), read_fd_(read_fd), write_fd_(write_fd) {}

MdSession::~MdSession() {
    stop();
    if (ssl_) SSL_free(ssl_);       // frees the BIOs too
    if (ctx_) SSL_CTX_free(ctx_);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
void MdSession::run() {
    if (!initSsl()) { ERR("TLS init failed, session not started"); return; }
    running_ = true;
    session_start_ms_ = msNow(); last_rx_ms_ = 0; ended_by_silence_ = false; reader_done_ = false;
    trace::newSession();
    if (getenv("AA_FORENSICS_TEST")) dumpQuietForensics(0);          // test hook: show what the dump looks like (never set in production)
    LOG("session started; waiting for VERSION_REQUEST from head unit");
    writer_ = std::thread([this]() { writerLoop(); });
    // The watchdog has its OWN thread: on the real USB endpoint poll() always says "readable" and read() then blocks inside FunctionFS (truck 30: tid in ffs_epfile_io),
    // so the reader loop never reaches its poll timeout and nothing in it can notice a dead truck.
    watchdog_ = std::thread([this]() { while (running_) { for (int i = 0; i < 4 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (running_) checkSilence(); } });
    readerLoop();                   // blocks until the session ends
    reader_done_ = true;
    running_ = false;
    tx_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (watchdog_.joinable()) watchdog_.join();
    LOG("session ended");
}

void MdSession::stop() {
    running_ = false;
    tx_cv_.notify_all();
}

// Truck-silence watchdog. Sync 4 sometimes stops talking while the USB link stays "configured" (truck 29: engine start, ping at 284.4 s, then NOTHING for 104 s; truck 26 looked the
// same). Nothing ends such a session by itself, so the Pi would sit there forever. A live truck pings about once a second, so a long silence means the session is dead: end it
// and let Device re-attach the USB gadget (a software unplug/replug) so Sync 4 starts over.
static void dumpQuietForensics(int64_t silent_ms);
void MdSession::checkSilence() {
    const int64_t now = msNow();
    const bool heard = last_rx_ms_ > 0;
    int limit_s = heard ? config_.truck_silence_s : config_.truck_silence_start_s;
    // Early in a session (the truck 33 boot race: Sync 4 took the device, answered the handshake for ~2 s and then went quiet) a much shorter silence is already conclusive,
    // and every second saved is a second sooner that the vehicle can be given a fresh enumeration.
    if (config_.truck_silence_s <= 0) return;      // watchdog disabled by config: ALL limits off, including the early one
    if (heard && config_.truck_silence_early_s > 0 && now - session_start_ms_ < (int64_t)config_.truck_silence_early_window_s * 1000 && (limit_s <= 0 || config_.truck_silence_early_s < limit_s)) limit_s = config_.truck_silence_early_s;
    if (limit_s <= 0) return;
    const int64_t silent = now - (heard ? (int64_t)last_rx_ms_ : session_start_ms_);
    if (silent < (int64_t)limit_s * 1000) return;
    ERR("truck silent for " << silent / 1000 << " s" << (heard ? " (it was talking before)" : " (never heard from it this session)") << " -> ending the session so USB is re-attached");
    if (quiet_dumps_ < 2) dumpQuietForensics(silent);   // evidence at EVERY declared-dead event (the heartbeat dump only runs while video flows)
    ended_by_silence_ = true;
    endSession("truck silent");
    if (onDead) onDead();           // the owner pulls the USB gadget off the bus: that makes the blocked endpoint read()/write() return, so run() can finish
    // Last resort: if the reader is STILL stuck 8 s later (the unbind did not wake it), a process restart is the one thing that surely re-enumerates USB from scratch
    // (systemd Restart=always; startup tears the gadget down and rebuilds it). Never reached in tests (the reader returns at once there).
    for (int i = 0; i < 80 && !reader_done_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!reader_done_ && config_.truck_silence_exit) { ERR("reader still blocked 8 s after the truck was declared dead: exiting so the service restarts and USB is re-enumerated"); std::cout.flush(); std::_Exit(3); }
}

void MdSession::endSession(const std::string& why) {
    LOG("ending session: " << why);
    std::cout << ring_.dump("MdSession") << std::endl;   // what the vehicle said last
    running_ = false;
    tx_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// reader: bulk OUT endpoint -> frames -> messages
// ---------------------------------------------------------------------------
void MdSession::readerLoop() {
    std::vector<uint8_t> buf(kReadBuf);
    while (running_) {
        struct pollfd pfd{read_fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 500);
        if (pr == 0) { static int idle = 0; if (++idle % 6 == 0) LOG("t=" << msNow() << "ms idle (no data from HU)"); continue; }
        if (pr < 0) { if (errno == EINTR) continue; ERR("poll: " << strerror(errno)); break; }
        ssize_t n = ::read(read_fd_, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            ERR("read(ep OUT) failed: " << strerror(errno) << " -> head unit gone");
            break;
        }
        if (n == 0) continue;
        rx_acc_.insert(rx_acc_.end(), buf.begin(), buf.begin() + n);
        parseIncoming();
    }
}

void MdSession::parseIncoming() {
    size_t off = 0;
    while (rx_acc_.size() - off >= 4) {
        const uint8_t* h = rx_acc_.data() + off;
        uint8_t channel = h[0];
        uint8_t flags   = h[1];
        uint16_t len    = rd16(h + 2);
        size_t hdr = 4;
        if ((flags & 0x03) == FT_FIRST) hdr += 4;         // total length, unused
        if (rx_acc_.size() - off < hdr + len) break;       // partial frame, wait
        last_rx_ms_ = msNow();
        if (trace::ok()) LOG("t=" << msNow() << "ms RXFRAME ch=" << (int)channel << " flags=0x" << std::hex << (int)flags << std::dec << " len=" << len);
        std::vector<uint8_t> payload(h + hdr, h + hdr + len);
        off += hdr + len;
        onFrame(channel, flags, std::move(payload));
    }
    if (off) rx_acc_.erase(rx_acc_.begin(), rx_acc_.begin() + off);
}

void MdSession::onFrame(uint8_t channel, uint8_t flags, std::vector<uint8_t> payload) {
    // An encrypted frame may legitimately yield NO plaintext yet (a TLS record
    // split across frames, or a post-handshake record with no app data), so an
    // empty decrypt is not an error. Plaintext is accumulated per channel and a
    // message is delivered only when its frame sequence completes.
    std::vector<uint8_t> plain;
    if (flags & FLAG_ENCRYPTED) plain = sslDecrypt(payload);
    else plain = std::move(payload);

    uint8_t ft = flags & 0x03;
    if (ft == FT_BULK) {
        if (!plain.empty()) onMessage(channel, plain);
        return;
    }
    if (ft == FT_FIRST) { reassembly_[channel].clear(); reassembling_[channel] = true; }
    reassembly_[channel].insert(reassembly_[channel].end(), plain.begin(), plain.end());
    if (ft == FT_LAST) {
        reassembling_[channel] = false;
        std::vector<uint8_t> whole;
        whole.swap(reassembly_[channel]);
        if (!whole.empty()) onMessage(channel, whole);
    }
}

void MdSession::openChannel(int id) {
    ctl::ChannelOpenRequest open;
    open.set_priority(0);
    open.set_service_id(id);
    enqueue((uint8_t)id, true, ControlMessageType::MESSAGE_CHANNEL_OPEN_REQUEST, open.SerializeAsString(), 0x04 /*control bit, as aasdk/behead*/);
}

void MdSession::onMessage(uint8_t channel, const std::vector<uint8_t>& msg) {
    if (msg.size() < 2) return;
    uint16_t id = rd16(msg.data());
    ring_.push(true, channel, id, msg.size());
    if (id != 0x0000 && id != 0x8004 && trace::ok()) LOG("t=" << msNow() << "ms RX ch=" << (int)channel << " id=0x" << std::hex << id << std::dec << " len=" << msg.size() << " " << hexs(msg.data()+2, msg.size()-2));
    const uint8_t* p = msg.data() + 2;
    size_t n = msg.size() - 2;
    if (false) {
        int nx = pending_open_.front(); pending_open_.erase(pending_open_.begin());
        LOG("channel " << (int)channel << " open response -> opening next: " << nx);
        openChannel(nx);
    }
    if (channel == 0) onControlMessage(id, p, n);
    else if ((int)channel == video_channel_) onVideoMessage(id, p, n);
    else if ((int)channel == input_channel_ && id == 0x8001) onInputMessage(id, p, n);
    else if ((int)channel == sensor_channel_ && (id == 0x8003 || id == 0x8002)) onSensorMessage(id, p, n);
    else if (config_.relay_audio && (((int)channel == aud_[0].ch && aud_[0].ch >= 0) || ((int)channel == aud_[1].ch && aud_[1].ch >= 0))) onAudioChannelMessage((int)channel == aud_[0].ch ? 0 : 1, id, p, n);
    else if (config_.relay_audio && (int)channel == mic_ch_ && mic_ch_ >= 0) onMicChannelMessage(id, p, n);
    else if ((int)channel == bt_channel_ && bt_channel_ >= 0) onBtMessage(id, p, n);
    else if ((int)channel == media_channel_ && media_channel_ >= 0) {
        if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) { media_open_ = true; LOG("media playback status channel " << media_channel_ << " opened"); if (onMediaChannel) onMediaChannel(true); }
        else if (onMedia) onMedia(id, std::string((const char*)p, n));
        else LOG("media playback status message id 0x" << std::hex << id << std::dec << " ignored (no relay)");
    }
    else if ((int)channel == nav_channel_ && nav_channel_ >= 0) {
        if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) { nav_open_ = true; LOG("navigation channel " << nav_channel_ << " opened"); if (onNavChannel) onNavChannel(true); }
        else if (onNav) onNav(id, std::string((const char*)p, n));
        else LOG("navigation message id 0x" << std::hex << id << std::dec << " ignored (no relay)");
    }
    else if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) {
        LOG("channel " << (int)channel << " opened");
        if ((int)channel == input_channel_) enqueue(channel, true, 0x8002 /*BINDING_REQUEST*/, binding_req_);
        if ((int)channel == sensor_channel_) for (int t : sensor_types_) {
            std::string req; req.push_back(0x08); req.push_back((char)t); req.push_back(0x10); req.push_back(0x00);   // {sensor_type, refresh_interval=0}
            enqueue(channel, true, 0x8001 /*SENSOR_START_REQUEST*/, req);
        }
    }
    else LOG("message id 0x" << std::hex << id << std::dec << " on channel " << (int)channel << " (not handled)");
}

// ---------------------------------------------------------------------------
// control channel (0)
// ---------------------------------------------------------------------------
void MdSession::onControlMessage(uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
    case ControlMessageType::MESSAGE_VERSION_REQUEST: {
        uint16_t maj = n >= 2 ? rd16(p) : 0, min = n >= 4 ? rd16(p + 2) : 0;
        const int vmin = config_.truck_mirror_phone ? std::max(config_.truck_ver_minor, 7) : config_.truck_ver_minor;
        LOG("VERSION_REQUEST from HU: " << maj << "." << min << " -> replying 1." << vmin << " / match");
        std::vector<uint8_t> r; wr16(r, 1); wr16(r, (uint16_t)vmin); wr16(r, 0);
        enqueueRaw(0, false, [&]{ std::vector<uint8_t> v; wr16(v, ControlMessageType::MESSAGE_VERSION_RESPONSE); v.insert(v.end(), r.begin(), r.end()); return v; }());
        return;
    }
    case ControlMessageType::MESSAGE_ENCAPSULATED_SSL:
        onSslHandshake(p, n);
        return;
    case ControlMessageType::MESSAGE_AUTH_COMPLETE: {
        ctl::AuthResponse a; a.ParsePartialFromArray(p, (int)n);
        { std::string hex; char b[4]; for (size_t i = 0; i < n && i < 16; ++i) { snprintf(b, sizeof b, "%02x", p[i]); hex += b; }
          LOG("AUTH_COMPLETE from HU, status=" << a.status() << " raw=" << hex << (a.status() == 0 ? "" : "  (NON-ZERO: head unit rejected our certificate)")
              << " -> sending SERVICE_DISCOVERY_REQUEST"); }
        ctl::ServiceDiscoveryRequest req;
        req.set_device_name("Pixel");
        req.set_label_text("AA Bridge");
        std::string introduction = req.SerializeAsString();
        if (config_.truck_mirror_phone) {                                    // repeat the real phone's introduction byte for byte, if we have recorded one
            std::ifstream f(config_.sdreq_file, std::ios::binary); std::string rec; if (f) rec.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            ctl::ServiceDiscoveryRequest chk;
            if (!rec.empty() && chk.ParsePartialFromString(rec)) { introduction = rec; LOG("introducing ourselves to the vehicle with the real phone's recorded request (" << rec.size() << " bytes): " << [&]{ chk.clear_small_icon(); chk.clear_medium_icon(); chk.clear_large_icon(); return chk.ShortDebugString(); }()); }
            else LOG("truck_mirror_phone: no recorded phone request in " << config_.sdreq_file << " yet (it is recorded the first time the phone connects); using the built-in one this boot");
        }
        enqueue(0, true, ControlMessageType::MESSAGE_SERVICE_DISCOVERY_REQUEST, introduction);
        return;
    }
    case ControlMessageType::MESSAGE_SERVICE_DISCOVERY_RESPONSE: {
        ctl::ServiceDiscoveryResponse sdr;
        if (!sdr.ParsePartialFromArray(p, (int)n)) { ERR("bad SERVICE_DISCOVERY_RESPONSE"); return; }
        LOG("SERVICE_DISCOVERY_RESPONSE: HU make=" << sdr.make() << " model=" << sdr.model()
            << " hu=" << sdr.head_unit_make() << "/" << sdr.head_unit_model()
            << " channels=" << sdr.channels_size());
        // Inventory of EVERYTHING the head unit offers (which services exist on which channel): this decides what the bridge must forward between the
        // phone and the vehicle (navigation, phone status, media metadata, notifications, ...). The raw bytes are also kept for offline decoding.
        { std::ofstream raw("/var/log/aa-bridge-sdr.bin", std::ios::binary | std::ios::trunc); if (raw) raw.write((const char*)p, (std::streamsize)n); }
        for (int i = 0; i < sdr.channels_size(); ++i) {
            const auto& ch = sdr.channels(i);
            { std::vector<const google::protobuf::FieldDescriptor*> fs; ch.GetReflection()->ListFields(ch, &fs); std::string names;
              for (auto* f : fs) if (f->name() != "id") names += (names.empty() ? "" : ",") + f->name();
              std::string txt = ch.ShortDebugString(); if (txt.size() > 360) txt = txt.substr(0, 360) + "...";
              LOG("  OFFERED channel " << ch.id() << " services=[" << names << "] " << txt); }
            if (ch.has_media_sink_service() && ch.media_sink_service().video_configs_size() > 0) {
                video_channel_ = ch.id();
                const auto& ms = ch.media_sink_service();
                LOG("  video sink service (text): " << ms.ShortDebugString());
                video_config_index_ = 0;
                for (int k = 0; k < ms.video_configs_size(); ++k) {
                    const auto& vc = ms.video_configs(k);
                    LOG("  video config[" << k << "] resolution=" << vc.codec_resolution()
                        << " fps=" << vc.frame_rate() << " density=" << vc.density()
                        << " width_margin=" << vc.width_margin() << " height_margin=" << vc.height_margin()
                        << " real_density=" << vc.real_density() << " viewing_distance=" << vc.viewing_distance()
                        << " decoder_depth=" << vc.decoder_additional_depth() << " codec=" << vc.video_codec_type()
                        << " ui_config=" << (vc.has_ui_config() ? vc.ui_config().ShortDebugString() : std::string("none")));
                    { // pick the truck config matching the configured output frame (video_width x video_height); default 1280x720
                      static const int dd[10][2] = {{0,0},{800,480},{1280,720},{1920,1080},{2560,1440},{3840,2160},{720,1280},{1080,1920},{1440,2560},{2160,3840}};
                      int rr = (int)vc.codec_resolution();
                      if (rr >= 1 && rr <= 9 && dd[rr][0] == config_.video_width && dd[rr][1] == config_.video_height) video_config_index_ = k; }
                }
                {   // codec size + margins of the config we will use
                    const auto& vc = ms.video_configs(video_config_index_);
                    static const int dims[10][2] = {{0,0},{800,480},{1280,720},{1920,1080},{2560,1440},{3840,2160},{720,1280},{1080,1920},{1440,2560},{2160,3840}};
                    int r = (int)vc.codec_resolution();
                    if (r >= 1 && r <= 9) { video_codec_w_ = dims[r][0]; video_codec_h_ = dims[r][1]; }
                    video_wmargin_ = (int)vc.width_margin(); video_hmargin_ = (int)vc.height_margin();
                    int refw = 0, refh = 0, best = 0;
                    for (int k = 0; k < ms.video_configs_size(); ++k) {   // the truck's largest config: its window size (in dp) is the layout the phone normally gets
                        const auto& v = ms.video_configs(k); int rr = (int)v.codec_resolution();
                        if (rr < 1 || rr > 9) continue;
                        int cw = dims[rr][0] - (int)v.width_margin(), ch2 = dims[rr][1] - (int)v.height_margin();
                        if (dims[rr][0] * dims[rr][1] > best) { best = dims[rr][0] * dims[rr][1]; refw = cw; refh = ch2; }
                    }
                    if (onVideoParams) { VideoParams vp; vp.valid = true; vp.ref_win_w = refw; vp.ref_win_h = refh; vp.density = (int)vc.density(); vp.real_density = (int)vc.real_density();
                        vp.viewing_distance = (int)vc.viewing_distance(); vp.pixel_aspect_ratio_e4 = (int)vc.pixel_aspect_ratio_e4();
                        vp.decoder_additional_depth = (int)vc.decoder_additional_depth(); onVideoParams(vp); }
                    video_ui_margins_ = vc.has_ui_config() && vc.ui_config().has_margins();
                    if (video_ui_margins_) { const auto& m = vc.ui_config().margins(); ui_l_ = (int)m.left(); ui_r_ = (int)m.right(); ui_t_ = (int)m.top(); ui_b_ = (int)m.bottom(); }
                }
                LOG("  -> video sink on channel " << video_channel_ << ", using config index " << video_config_index_);
            }
        }
        if (video_channel_ < 0) { endSession("HU advertised no video sink"); return; }
        // A real phone opens every service the HU advertises, video first.
        std::vector<int> ids; ids.push_back(video_channel_);
        for (int i = 0; i < sdr.channels_size(); ++i) {
            const auto& ch = sdr.channels(i);
            if (ch.has_input_source_service() && input_channel_ < 0) {
                input_channel_ = (int)ch.id();
                if (ch.input_source_service().touchscreen_size() > 0) {
                    touch_w_ = ch.input_source_service().touchscreen(0).width();
                    touch_h_ = ch.input_source_service().touchscreen(0).height();
                    LOG("  touchscreen " << touch_w_ << "x" << touch_h_);
                    if (onTouchGeometry) onTouchGeometry(touch_w_, touch_h_);
                }
                LOG("  input source service (text): " << ch.input_source_service().ShortDebugString());
                // BindingRequest = the keycodes WE (as the phone) want from the head unit: field 1, packed varints (like a real phone's). The first version re-emitted the
                // vehicle's own list assuming unpacked varints, got an empty request, and so the vehicle never sent a single steering-wheel key (truck 36).
                std::string b, packed; std::string wanted;
                static const int kPhoneKeys[] = {3, 4, 19, 20, 21, 22, 23, 84, 85, 86, 87, 88, 126, 127};          // what HuSession tells the real phone it accepts
                const auto& isv = ch.input_source_service();
                for (int q = 0; q < isv.keycodes_supported_size(); ++q) {
                    const int kc = (int)isv.keycodes_supported(q);
                    if (!config_.relay_keys || std::find(std::begin(kPhoneKeys), std::end(kPhoneKeys), kc) == std::end(kPhoneKeys)) continue;
                    uint32_t v = (uint32_t)kc; do { uint8_t byte = v & 0x7f; v >>= 7; if (v) byte |= 0x80; packed.push_back((char)byte); } while (v);
                    wanted += std::to_string(kc) + " ";
                }
                if (!packed.empty()) { b.push_back((char)0x0a); b.push_back((char)packed.size()); b += packed; }
                LOG("  key binding request: " << (wanted.empty() ? "none" : wanted));
                binding_req_ = b;
                ids.push_back(input_channel_);
            }
        }
        if (config_.relay_audio) for (int i = 0; i < sdr.channels_size(); ++i) {     // PCM media/guidance sinks + the microphone source, exactly as a phone would open them
            const auto& ch = sdr.channels(i);
            // Sync 4's VIDEO sink has no available_type/audio_type at all and proto2 defaults make it look like a PCM GUIDANCE sink: it stole the guidance slot (channel 1) and the real
            // guidance channel 6 was never opened, so navigation voice never played (truck 37). Only a sink that really declares PCM, an audio type and audio configs counts.
            if (ch.has_media_sink_service() && ch.media_sink_service().has_available_type() && ch.media_sink_service().has_audio_type() && ch.media_sink_service().audio_configs_size() > 0 && ch.media_sink_service().video_configs_size() == 0
                && ch.media_sink_service().available_type() == aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM) {
                const int t = (int)ch.media_sink_service().audio_type(); int idx = -1;
                if (t == (int)aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA) idx = 0; else if (t == (int)aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE) idx = 1;
                if (idx >= 0 && aud_[idx].ch < 0) { aud_[idx].ch = (int)ch.id(); ids.push_back(aud_[idx].ch); LOG("  audio sink " << (idx ? "GUIDANCE" : "MEDIA") << " on channel " << aud_[idx].ch << ": " << ch.media_sink_service().ShortDebugString()); }
            }
            if (ch.has_media_source_service() && mic_ch_ < 0 && ch.media_source_service().available_type() == aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM) { mic_ch_ = (int)ch.id(); ids.push_back(mic_ch_); LOG("  microphone source on channel " << mic_ch_ << ": " << ch.media_source_service().ShortDebugString()); }
        }
        if (config_.relay_media_status) for (int i = 0; i < sdr.channels_size(); ++i) {     // the vehicle's media playback status service (now-playing for the cluster): open it like a real phone does
            const auto& ch = sdr.channels(i);
            if (ch.has_media_playback_service() && media_channel_ < 0) { media_channel_ = (int)ch.id(); ids.push_back(media_channel_); LOG("  media playback status service on channel " << media_channel_); }
        }
        if (config_.relay_nav) for (int i = 0; i < sdr.channels_size(); ++i) {     // the vehicle's navigation service: open it like a real phone does
            const auto& ch = sdr.channels(i);
            if (ch.has_navigation_status_service() && nav_channel_ < 0) { nav_channel_ = (int)ch.id(); ids.push_back(nav_channel_); LOG("  navigation service on channel " << nav_channel_ << ": " << ch.navigation_status_service().ShortDebugString()); }
        }
        if (onVisibleWindow) {
            int W = video_codec_w_, H = video_codec_h_, wm = video_wmargin_, hm = video_hmargin_;
            if (W > 0 && video_ui_margins_ && (ui_l_ + ui_r_ + ui_t_ + ui_b_) > 0 && W - ui_l_ - ui_r_ > 0 && H - ui_t_ - ui_b_ > 0) {
                // Preferred: the head unit's exact per-side UI margins (ui_config.margins) - no total/per-side guessing, handles asymmetry.
                int w = W - ui_l_ - ui_r_, h = H - ui_t_ - ui_b_;
                LOG("  visible window (codec " << W << "x" << H << " ui_config margins l" << ui_l_ << " r" << ui_r_ << " t" << ui_t_ << " b" << ui_b_ << "): " << w << "x" << h << " at " << ui_l_ << "," << ui_t_);
                onVisibleWindow(ui_l_, ui_t_, w, h);
            } else if (W > 0 && (wm > 0 || hm > 0)) {
                // AAP: UI resolution = codec resolution minus margins, clipped centred. Whether the margin fields are the TOTAL or PER-SIDE
                // amount is not stated in the guide: pick the interpretation whose aspect matches the touchscreen when known (total otherwise).
                struct Cand { int w, h; } tot{W - wm, H - hm}, per{W - 2 * wm, H - 2 * hm};
                auto ok = [&](Cand c) { return c.w > 0 && c.h > 0; };
                Cand use = tot; const char* how = "total";
                if (ok(per) && touch_w_ > 0 && touch_h_ > 0) {
                    double ta = (double)touch_w_ / touch_h_;
                    double et = ok(tot) ? std::abs((double)tot.w / tot.h - ta) : 1e9, ep = std::abs((double)per.w / per.h - ta);
                    if (ep < et) { use = per; how = "per-side"; }
                }
                if (ok(use)) {
                    LOG("  visible window (codec " << W << "x" << H << " margins " << wm << "," << hm << " read as " << how << "): " << use.w << "x" << use.h
                        << " at " << (W - use.w) / 2 << "," << (H - use.h) / 2);
                    onVisibleWindow((W - use.w) / 2, (H - use.h) / 2, use.w, use.h);
                }
            } else {
                LOG("  head unit advertised no margins for the chosen video config");
                onVisibleWindow(0, 0, 0, 0);
            }
        }
        std::vector<int> offered_sensors;
        for (int i = 0; i < sdr.channels_size(); ++i) {
            const auto& ch = sdr.channels(i);
            if (ch.has_sensor_source_service() && sensor_channel_ < 0) {
                sensor_channel_ = (int)ch.id();
                for (int k = 0; k < ch.sensor_source_service().sensors_size(); ++k) {
                    const auto& sn = ch.sensor_source_service().sensors(k);
                    int t = (int)sn.sensor_type();
                    for (int u = 0; u < sn.unknown_fields().field_count(); ++u) { const auto& uf = sn.unknown_fields().field(u); if (uf.number() == 1 && uf.type() == google::protobuf::UnknownField::TYPE_VARINT) t = (int)uf.varint(); }     // types newer than our protobuf library (Sync 4 offers 23 and 24)
                    LOG("  sensor offered: type " << t);
                    offered_sensors.push_back(t);
                    if (t == 3 || t == 7 || t == 8 || t == 10 || t == 13 || (config_.relay_sensors && (t == 2 || t == 4 || t == 5 || t == 6 || t == 23 || t == 24))) sensor_types_.push_back(t);     // speed, parking brake, gear, night mode, driving status (+ compass, rpm, odometer, fuel when relaying)
                }
                LOG("  sensor channel " << sensor_channel_ << " offers " << ch.sensor_source_service().sensors_size() << " sensors; subscribing to " << sensor_types_.size());
                ids.push_back(sensor_channel_);
                if (onSensorTypes) onSensorTypes(offered_sensors);
                if (onSensorCaps) { std::vector<int> fuel, ev; const auto& ss = ch.sensor_source_service(); for (int q = 0; q < ss.supported_fuel_types_size(); ++q) fuel.push_back((int)ss.supported_fuel_types(q)); for (int q = 0; q < ss.supported_ev_connector_types_size(); ++q) ev.push_back((int)ss.supported_ev_connector_types(q));
                    std::string fs, es; for (int v : fuel) fs += std::to_string(v) + " "; for (int v : ev) es += std::to_string(v) + " "; LOG("  vehicle fuel types: " << fs << " ev connectors: " << es); onSensorCaps(fuel, ev); }
            }
        }
        for (int i = 0; i < sdr.channels_size(); ++i) {                         // the vehicle's Bluetooth service: where a real phone announces its Bluetooth address so the vehicle can pair + link hands-free
            const auto& ch = sdr.channels(i);
            if (ch.has_bluetooth_service() && bt_channel_ < 0) {
                bt_car_address_ = ch.bluetooth_service().car_address();
                LOG("  vehicle Bluetooth service on channel " << ch.id() << ": car address " << bt_car_address_ << ", " << ch.bluetooth_service().supported_pairing_methods_size() << " pairing method(s)" << (config_.bt_pair_proxy ? " -> answering it" : " (ignored: bt_pair_proxy=0)"));
                if (config_.bt_pair_proxy) { bt_channel_ = (int)ch.id(); ids.push_back(bt_channel_); }
            }
        }
        pending_open_.clear();
        for (int id : ids) openChannel(id);
        return;
    }
    case ControlMessageType::MESSAGE_PING_REQUEST: {
        ctl::PingRequest rq; rq.ParsePartialFromArray(p, (int)n);
        ctl::PingResponse rs; rs.set_timestamp(rq.timestamp());
        enqueue(0, true, ControlMessageType::MESSAGE_PING_RESPONSE, rs.SerializeAsString());
        return;
    }
    case ControlMessageType::MESSAGE_BYEBYE_REQUEST: {
        ctl::ByeByeResponse r;
        enqueue(0, true, ControlMessageType::MESSAGE_BYEBYE_RESPONSE, r.SerializeAsString());
        endSession("HU sent BYEBYE_REQUEST");
        return;
    }
    case ControlMessageType::MESSAGE_VOICE_SESSION_NOTIFICATION:
    case ControlMessageType::MESSAGE_AUDIO_FOCUS_NOTIFICATION:
        if ((id == ControlMessageType::MESSAGE_AUDIO_FOCUS_NOTIFICATION ? config_.relay_audio : config_.relay_control) && onControlRaw) onControlRaw((uint16_t)id, std::string((const char*)p, n));
        return;
    case ControlMessageType::MESSAGE_NAV_FOCUS_NOTIFICATION:
        LOG("vehicle NAV FOCUS notification raw=" << hexs(p, n));      // evidence for the navigation relay: does the vehicle grant/refuse navigation focus?
        return;
    case ControlMessageType::MESSAGE_CAR_CONNECTED_DEVICES_RESPONSE:
    case ControlMessageType::MESSAGE_CALL_AVAILABILITY_STATUS:
        return;
    default:
        LOG("control message id " << id << " (" << n << " bytes) not handled");
    }
}

// ---------------------------------------------------------------------------
// video channel
// ---------------------------------------------------------------------------
// vehicle Bluetooth service: we are the "phone". Announce a Bluetooth address (field 1) with numeric-comparison pairing (field 2 = 2); the vehicle then pairs/links to it.
void MdSession::onBtMessage(uint16_t id, const uint8_t* p, size_t n) {
    if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) {
        std::string addr = btAddress ? btAddress() : std::string();
        if (addr.empty()) { LOG("[Bt] vehicle Bluetooth channel opened but no local Bluetooth address is available: nothing announced"); return; }
        std::string m; m.push_back(0x0a); m.push_back((char)addr.size()); m += addr; m.push_back(0x10); m.push_back(0x02);
        LOG("[Bt] vehicle Bluetooth channel opened -> announcing our address " << addr << " (pairing method numeric comparison)");
        enqueue((uint8_t)bt_channel_, true, 0x8001 /*PAIRING_REQUEST*/, m);
        if (onCarAddress && !bt_car_address_.empty()) onCarAddress(bt_car_address_);
        return;
    }
    LOG("[Bt] vehicle Bluetooth message 0x" << std::hex << id << std::dec << " len=" << n << " " << hexs(p, n));
    if (id == 0x8002) LOG("[Bt] vehicle pairing response: status/already_paired in the bytes above (field 1 status, field 2 already_paired)");
    if (id == 0x8003) {                                                    // AUTHENTICATION_DATA from the vehicle: acknowledge, our agent auto-accepts the actual pairing
        std::string r; r.push_back(0x08); r.push_back(0x00);
        enqueue((uint8_t)bt_channel_, true, 0x8004 /*AUTHENTICATION_RESULT*/, r);
        LOG("[Bt] answered the vehicle's authentication data with result OK");
    }
}

void MdSession::onInputMessage(uint16_t, const uint8_t* p, size_t n) {
    namespace im = aap_protobuf::service::inputsource::message;
    im::InputReport r;
    if (!r.ParsePartialFromArray(p, (int)n)) return;
    if (r.has_key_event()) {                                                   // steering wheel / media keys
        for (int i = 0; i < r.key_event().keys_size(); ++i) {
            const auto& k = r.key_event().keys(i);
            LOG("KEY " << k.keycode() << (k.down() ? " down" : " up") << (k.longpress() ? " long" : "") << " meta=" << k.metastate());
            if (key_handler_) key_handler_((int)k.keycode(), k.down(), k.longpress(), (int)k.metastate());
        }
        return;
    }
    if (!r.has_touch_event()) return;
    const auto& t = r.touch_event();
    if (t.pointer_data_size() == 0) return;
    int idx = t.has_action_index() ? (int)t.action_index() : 0;
    if (idx < 0 || idx >= t.pointer_data_size()) idx = 0;
    const auto& pt = t.pointer_data(idx);
    int act = (int)t.action();
    int a = (act == im::ACTION_DOWN || act == im::ACTION_POINTER_DOWN) ? 0 : (act == im::ACTION_UP || act == im::ACTION_POINTER_UP) ? 1 : 2;
    if (a == 0 && focus_native_ && config_.focus_reclaim) reclaimFocus();             // the vehicle says we have no screen, yet it sends us touches: the projection IS on screen
    static unsigned cnt = 0;
    if (a != 2 || (++cnt % 20) == 0) LOG("TOUCH x=" << pt.x() << " y=" << pt.y() << " action=" << act << " pointers=" << t.pointer_data_size());
    if (touch_frame_handler_) {
        TouchFrame f; f.action = act; f.index = idx;
        for (int i = 0; i < t.pointer_data_size(); ++i) f.p.push_back({(int)t.pointer_data(i).pointer_id(), (int)t.pointer_data(i).x(), (int)t.pointer_data(i).y()});
        touch_frame_handler_(f);
        return;
    }
    if (touch_handler_) touch_handler_((int)pt.x(), (int)pt.y(), a);
}

void MdSession::onSensorMessage(uint16_t id, const uint8_t* p, size_t n) {
    if (id == 0x8002) { LOG("sensor start response " << hexs(p, n)); return; }
    namespace sm = aap_protobuf::service::sensorsource::message;
    if (config_.relay_sensors && onSensorRaw) onSensorRaw(std::string((const char*)p, n));     // the phone gets the readings too (HuSession filters by what the phone subscribed to)
    sm::SensorBatch b;
    if (!b.ParsePartialFromArray(p, (int)n)) return;
    for (int u = 0; u < b.unknown_fields().field_count(); ++u) {            // sensor data our protobuf library does not know (23, 24): log the raw bytes (first few) so it can be decoded later
        const auto& uf = b.unknown_fields().field(u); static int shown = 0;
        if (shown++ < 12) LOG("SENSOR RAW field " << uf.number() << (uf.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED ? " bytes=" + hexs((const uint8_t*)uf.length_delimited().data(), uf.length_delimited().size()) : " varint=" + std::to_string(uf.varint())));
    }
    bool reverse = veh_reverse_, moving = veh_moving_, changed = false;
    for (int i = 0; i < b.gear_data_size(); ++i) { reverse = (b.gear_data(i).gear() == sm::GEAR_REVERSE); changed = true; LOG("VEHICLE gear=" << (int)b.gear_data(i).gear()); }
    for (int i = 0; i < b.driving_status_data_size(); ++i) { moving = (b.driving_status_data(i).status() != 0); changed = true; LOG("VEHICLE driving_status=" << b.driving_status_data(i).status()); }
    for (int i = 0; i < b.speed_data_size(); ++i) {
        static int cnt = 0; if (++cnt % 20 == 1) LOG("VEHICLE speed_e3=" << b.speed_data(i).speed_e3());
        if (b.driving_status_data_size() == 0 && sensor_types_.size() && std::find(sensor_types_.begin(), sensor_types_.end(), 13) == sensor_types_.end()) { moving = b.speed_data(i).speed_e3() > 500; changed = true; }   // fallback when no driving-status sensor
    }
    for (int i = 0; i < b.parking_brake_data_size(); ++i) { LOG("VEHICLE parking_brake=" << b.parking_brake_data(i).parking_brake()); if (onVehicle) onVehicle(4, b.parking_brake_data(i).parking_brake() ? 1 : 0); }
    // Raw readings for the Debug page and the phone relay: 3 gear (enum value), 5 speed (m/s * 1000), 6 driving status (bit mask), 7 night mode (0/1)
    for (int i = 0; i < b.gear_data_size(); ++i) if (onVehicle) onVehicle(3, (int)b.gear_data(i).gear());
    for (int i = 0; i < b.driving_status_data_size(); ++i) if (onVehicle) onVehicle(6, (int)b.driving_status_data(i).status());
    for (int i = 0; i < b.speed_data_size(); ++i) if (onVehicle) { int s = (int)b.speed_data(i).speed_e3(); if (std::abs(s - last_speed_e3_) >= 250) { last_speed_e3_ = s; onVehicle(5, s); } }
    for (int i = 0; i < b.night_mode_data_size(); ++i) { LOG("VEHICLE night_mode=" << b.night_mode_data(i).night_mode()); if (onVehicle) onVehicle(7, b.night_mode_data(i).night_mode() ? 1 : 0); }
    if (!changed) return;
    if (!veh_known_ || reverse != veh_reverse_) { veh_reverse_ = reverse; if (onVehicle) onVehicle(1, reverse ? 1 : 0); }
    if (!veh_known_ || moving != veh_moving_) { veh_moving_ = moving; if (onVehicle) onVehicle(2, moving ? 1 : 0); }
    veh_known_ = true;
}

void MdSession::onVideoMessage(uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
    case ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE: {
        ctl::ChannelOpenResponse r; r.ParsePartialFromArray(p, (int)n);
        LOG("CHANNEL_OPEN_RESPONSE status=" << r.status() << " -> MEDIA_SETUP (H264)");
        med::shared::message::Setup s;
        s.set_type(med::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
        enqueue((uint8_t)video_channel_, true, 0x8000 /*MEDIA_MESSAGE_SETUP*/, s.SerializeAsString());
        return;
    }
    case 0x8003: { // MEDIA_MESSAGE_CONFIG (setup response)
        med::shared::message::Config c; c.ParsePartialFromArray(p, (int)n);
        max_unacked_ = c.max_unacked() > 0 ? c.max_unacked() : 1;
        LOG("MEDIA_CONFIG status=" << c.status() << " max_unacked=" << max_unacked_
            << " indices=" << c.configuration_indices_size());
        got_media_config_ = true;
        { std::string fr; fr.push_back(0x10); fr.push_back(0x01);   // VideoFocusRequest{mode=PROJECTED}
          enqueue((uint8_t)video_channel_, true, 0x8007, fr); }
        config_time_ = std::chrono::steady_clock::now();
        startVideoIfReady();
        return;
    }
    case 0x8008: { // MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION
        med::video::message::VideoFocusNotification f; f.ParsePartialFromArray(p, (int)n);
        LOG("VIDEO_FOCUS_NOTIFICATION focus=" << f.focus() << " unsolicited=" << f.unsolicited());
        if (f.focus() == med::video::message::VIDEO_FOCUS_PROJECTED) {
            if (focus_native_) LOG("video focus back after " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - focus_lost_t_).count() << " ms" << (focus_reqs_ ? " (we asked)" : ""));
            focus_native_ = false; focus_reqs_ = 0;
            got_video_focus_ = true;
            startVideoIfReady();
        } else {
            video_ready_ = false;   // HU took the screen back (native UI); keep session, pause video
            video_started_ = false;
            focus_native_ = true; focus_reqs_ = 0; focus_lost_t_ = std::chrono::steady_clock::now();
        }
        return;
    }
    case 0x8004: // MEDIA_MESSAGE_ACK
        if (unacked_ > 0) --unacked_;
        return;
    case 0x8009: // UPDATE_UI_CONFIG_REQUEST etc.
    default:
        LOG("video channel message id " << id << " (" << n << " bytes) not handled");
    }
}

// focus_reclaim (ui34a: Sync sent an unsolicited NATIVE focus and never followed with PROJECTED while still forwarding touches -> frozen image for the rest
// of the drive). A touch while paused means the projection surface is visible: ask for focus back (VideoFocusRequest, as a phone may); if the vehicle
// does not answer three requests, resume the video anyway.
void MdSession::reclaimFocus() {
    const auto now = std::chrono::steady_clock::now();
    if (now - focus_lost_t_ < std::chrono::milliseconds(1500) || now - focus_req_t_ < std::chrono::milliseconds(2000)) return;
    focus_req_t_ = now; ++focus_reqs_;
    const long lost_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(now - focus_lost_t_).count();
    if (focus_reqs_ <= 3) {
        LOG("video focus: the vehicle took the screen " << lost_ms / 1000 << " s ago but still sends touches -> asking for focus back (" << focus_reqs_ << "/3)");
        std::string fr; fr.push_back(0x10); fr.push_back(0x01);           // VideoFocusRequest{mode=PROJECTED}
        enqueue((uint8_t)video_channel_, true, 0x8007, fr);
    } else {
        LOG("video focus: no answer to 3 focus requests while touches keep coming -> resuming video without it");
        focus_native_ = false; got_video_focus_ = true; startVideoIfReady();
    }
}

void MdSession::startVideoIfReady() {
    if (video_started_ || !got_media_config_) return;
    if (!got_video_focus_) return;
    med::shared::message::Start st;
    st.set_session_id(0);
    st.set_configuration_index(video_config_index_);
    enqueue((uint8_t)video_channel_, true, 0x8001 /*MEDIA_MESSAGE_START*/, st.SerializeAsString());
    video_started_ = true;
    unacked_ = 0;
    video_ready_ = true;
    LOG("MEDIA_START sent -> video channel live, streaming compositor output");
}

// ---------------------------------------------------------------------------
// Link-quiet forensics. When the truck goes silent for seconds while the Pi itself looks healthy, the log alone cannot say whether the truck stopped
// talking or WE stopped listening (reader thread blocked/starved, USB controller stuck). Dump, once per quiet episode: every thread's name, scheduler
// state and kernel wait channel, the USB controller state/interrupt counters, pressure/load, and the kernel log tail (USB errors show up there).
// Everything is read-only and written straight to the log (fsynced by the main sync thread).
// ---------------------------------------------------------------------------
static std::string slurp(const std::string& path, size_t max = 600) {
    std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (s.size() > max) s.resize(max); for (auto& ch : s) if (ch == '\n') ch = '|'; return s;
}
static void dumpQuietForensics(int64_t silent_ms) {
    std::cout << "[Forensics] ==== truck silent " << silent_ms << " ms: dump ====" << std::endl;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) { if (e->d_name[0] == '.') continue; const std::string t = std::string("/proc/self/task/") + e->d_name;
            std::string stat = slurp(t + "/stat", 400); auto rp = stat.rfind(')'); const char st = rp != std::string::npos && rp + 2 < stat.size() ? stat[rp + 2] : '?';
            std::cout << "[Forensics] tid " << e->d_name << " " << slurp(t + "/comm") << " state=" << st << " wchan=" << slurp(t + "/wchan", 60) << std::endl; }
        closedir(d);
    }
    std::cout << "[Forensics] loadavg " << slurp("/proc/loadavg") << " | psi cpu " << slurp("/proc/pressure/cpu") << std::endl;
    if (DIR* d = opendir("/sys/class/hwmon")) { while (dirent* e = readdir(d)) { if (e->d_name[0] == '.') continue; const std::string h = std::string("/sys/class/hwmon/") + e->d_name;
        std::cout << "[Forensics] hwmon " << slurp(h + "/name") << " undervoltage_now=" << slurp(h + "/in0_lcrit_alarm") << " temp=" << slurp(h + "/temp1_input") << std::endl; } closedir(d); }
    if (DIR* d = opendir("/sys/class/udc")) { while (dirent* e = readdir(d)) { if (e->d_name[0] == '.') continue; const std::string u = std::string("/sys/class/udc/") + e->d_name;
        std::cout << "[Forensics] udc " << e->d_name << " state=" << slurp(u + "/state") << " speed=" << slurp(u + "/current_speed") << std::endl; } closedir(d); }
    for (const char* cmd : {"grep -iE 'dwc2|usb' /proc/interrupts 2>&1", "dmesg 2>&1 | tail -30"}) {
        if (FILE* p = popen(cmd, "r")) { char line[300]; while (fgets(line, sizeof line, p)) { std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); std::cout << "[Forensics] " << l << std::endl; } pclose(p); }
    }
    std::cout << "[Forensics] ==== end dump ====" << std::endl;
}

void MdSession::sendVideo(const uint8_t* data, size_t size, uint64_t pts_us) {
    if (!running_ || !video_ready_ || video_channel_ < 0) return;
    {   // truck-link heartbeat every 2 s: if Sync 4 goes quiet, this shows how long, how many frames were un-ACKed / dropped, and how big we were sending
        const int64_t now = msNow();
        if (now - last_hb_ms_ >= 2000) { last_hb_ms_ = now;
            const int64_t age = now - last_rx_ms_;
            LOG("t=" << msNow() << "ms USB hb: truck silent " << age << " ms, unacked " << unacked_ << "/" << max_unacked_ << ", dropped pictures " << frames_dropped_ << ", last picture " << size << " bytes"
                << (age > 1500 ? "   <-- TRUCK QUIET" : ""));
            if (age < 1500) quiet_dumps_ = 0;
            else if ((quiet_dumps_ == 0 && age > 3000) || (quiet_dumps_ == 1 && age > 15000)) { ++quiet_dumps_; dumpQuietForensics(age); } }
    }
    if (unacked_ > max_unacked_ + 2) { ++frames_dropped_; return; }   // drop frame rather than back up
    std::string cfg, pic;
    splitAnnexB(data, size, cfg, pic);
    if (!cfg.empty() && cfg != sent_config_) {   // SPS/PPS: untimestamped MEDIA (0x0001), only when changed, like a phone
        sent_config_ = cfg;
        std::vector<uint8_t> v; wr16(v, 0x0001); v.insert(v.end(), cfg.begin(), cfg.end());
        enqueueRaw((uint8_t)video_channel_, true, std::move(v));
        LOG("sent codec config (SPS/PPS) " << cfg.size() << " bytes");
    }
    if (sent_config_.empty() || pic.empty()) return;   // never send pictures before the decoder has its parameter sets
    std::vector<uint8_t> v;
    v.reserve(2 + 8 + pic.size());
    wr16(v, 0x0000 /*MEDIA_WITH_TIMESTAMP*/);
    wr64(v, pts_us);
    v.insert(v.end(), pic.begin(), pic.end());
    ++unacked_;
    static unsigned n = 0; if (++n == 1 || n % 300 == 0) LOG("video frames sent: " << n << " (last " << pic.size() << " bytes)");
    enqueueRaw((uint8_t)video_channel_, true, std::move(v));
}

// ---------------------------------------------------------------------------
// writer: messages -> (encrypt) -> frames -> bulk IN endpoint
// ---------------------------------------------------------------------------
// The phone's AudioFocusRequest (18) is held until the vehicle's MEDIA audio channel is ready and goes out right before the (deferred) START.
// ui57a: in every boot where the phone connected during Sync's start-up USB flap the focus request was replayed at session creation, i.e. before
// Sync had even answered VERSION_REQUEST; the real session then got a START and no focus request -> Sync acked every chunk and played nothing until
// the user switched apps (a fresh request + START). Other control ids (battery, call availability) still go out immediately.
void MdSession::sendControl(uint16_t id, const std::string& body) {
    if (id == 18 && (!running_ || !aud_[0].ready)) { pending_focus_ = body; LOG("audio focus request held until the vehicle's MEDIA channel is ready"); return; }   // before run() too: the replay at session creation used to be dropped here
    if (!running_) return;
    enqueue(0, true, id, body);
}
void MdSession::flushPendingFocus(const char* why) {
    std::string f; f.swap(pending_focus_); if (f.empty()) return;
    LOG("audio focus request -> vehicle (" << why << ")"); enqueue(0, true, 18, f);
}

void MdSession::onAudioChannelMessage(int idx, uint16_t id, const uint8_t* p, size_t n) {
    namespace shm = aap_protobuf::service::media::shared::message;
    AudioCh& a = aud_[idx];
    if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) {       // channel open -> SETUP (PCM), like a phone
        shm::Setup su; su.set_type(shm::MEDIA_CODEC_AUDIO_PCM);
        LOG("audio " << (idx ? "GUIDANCE" : "MEDIA") << " channel " << a.ch << " opened -> SETUP(PCM)");
        enqueue((uint8_t)a.ch, true, 0x8000, su.SerializeAsString()); return;
    }
    if (id == 0x8003) {                                                  // CONFIG: ready (or not)
        shm::Config cf; cf.ParsePartialFromArray(p, (int)n);
        a.max_unacked = cf.has_max_unacked() && cf.max_unacked() > 0 ? (int)cf.max_unacked() : 4; a.ready = cf.status() == shm::Config::STATUS_READY;
        LOG("audio " << (idx ? "GUIDANCE" : "MEDIA") << " CONFIG status=" << cf.status() << " max_unacked=" << a.max_unacked << (a.ready ? " -> ready" : " -> NOT ready"));
        if (a.ready && idx == 0) flushPendingFocus("MEDIA channel ready");
        if (a.ready && a.want_start) startAudio(idx, a.session);
        return;
    }
    if (id == 0x8004) {                                                  // ACK from the vehicle: window opens
        aap_protobuf::service::media::source::message::Ack ak; ak.ParsePartialFromArray(p, (int)n);
        const int d = ak.has_ack() ? (int)ak.ack() : 1;                 // atomic: sendAudio() increments from the relay thread concurrently
        int cur = a.unacked.load(); while (!a.unacked.compare_exchange_weak(cur, cur - d < 0 ? 0 : cur - d)) {} return;
    }
    LOG("audio " << (idx ? "GUIDANCE" : "MEDIA") << " message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled");
}

void MdSession::startAudio(int s, int session) {
    if (s < 0 || s > 1) return;
    AudioCh& a = aud_[s]; a.session = session; a.want_start = true;          // remember the wish FIRST: the vehicle's channel may not even be discovered yet (phone faster than Sync after a key cycle)
    if (a.ch < 0) { LOG("audio " << (s ? "GUIDANCE" : "MEDIA") << " START deferred: vehicle audio channel not discovered yet"); return; }
    if (!running_) return;
    if (!a.ready) { LOG("audio " << (s ? "GUIDANCE" : "MEDIA") << " START deferred: vehicle channel not ready yet"); return; }
    aap_protobuf::service::media::shared::message::Start st; st.set_session_id(session); st.set_configuration_index(0);
    a.unacked = 0; a.started = true; LOG("audio " << (s ? "GUIDANCE" : "MEDIA") << " START session " << session);
    enqueue((uint8_t)a.ch, true, 0x8001, st.SerializeAsString());
}
void MdSession::stopAudio(int s) {
    if (s < 0 || s > 1 || aud_[s].ch < 0) return;
    AudioCh& a = aud_[s]; a.want_start = false; if (!a.started) return; a.started = false;
    LOG("audio " << (s ? "GUIDANCE" : "MEDIA") << " STOP (sent " << a.sent << ", dropped " << a.dropped << ")");
    if (running_) enqueue((uint8_t)a.ch, true, 0x8002, aap_protobuf::service::media::shared::message::Stop().SerializeAsString());
}
void MdSession::sendAudio(int s, uint64_t ts_us, const uint8_t* pcm, size_t n) {
    if (s < 0 || s > 1 || aud_[s].ch < 0 || !running_) return;
    AudioCh& a = aud_[s]; if (!a.started) return;
    if (a.unacked >= a.max_unacked) { if (++a.dropped % 200 == 1) LOG("audio " << (s ? "GUIDANCE" : "MEDIA") << ": vehicle window full, dropped " << a.dropped << " chunks so far"); return; }
    ++a.unacked; ++a.sent;
    std::vector<uint8_t> v; v.reserve(2 + 8 + n); wr16(v, 0x0000); wr64(v, ts_us); v.insert(v.end(), pcm, pcm + n);
    enqueueRaw((uint8_t)a.ch, true, std::move(v));
}
void MdSession::micRequest(bool open) {
    if (mic_ch_ < 0 || !running_) return;
    mic_want_ = open; if (!mic_open_) { LOG("microphone request deferred: channel not open yet"); return; }
    aap_protobuf::service::media::source::message::MicrophoneRequest rq; rq.set_open(open); if (open) { rq.set_anc_enabled(false); rq.set_ec_enabled(false); rq.set_max_unacked(config_.mic_max_unacked); }   // full request like a phone's (Sync 4 sent no audio for the bare open=true)
    LOG("microphone " << (open ? "OPEN" : "CLOSE") << " request -> vehicle"); enqueue((uint8_t)mic_ch_, true, 0x8005, rq.SerializeAsString());
}
void MdSession::onMicChannelMessage(uint16_t id, const uint8_t* p, size_t n) {
    if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE) { mic_open_ = true; LOG("microphone channel " << mic_ch_ << " opened"); if (mic_want_) micRequest(true); return; }
    if (id == 0x8006) { aap_protobuf::service::media::source::message::MicrophoneResponse r; r.ParsePartialFromArray(p, (int)n); mic_session_ = r.session_id(); mic_frames_ = 0; LOG("microphone response status=" << r.status() << " session=" << r.session_id()); return; }
    if (id == 0x0000 || id == 0x0001) {
        if (mic_frames_++ == 0) LOG("microphone: FIRST audio data from the vehicle (" << n << " bytes)");
        { aap_protobuf::service::media::source::message::Ack ak; ak.set_session_id(mic_session_); ak.set_ack(1); enqueue((uint8_t)mic_ch_, true, 0x8004, ak.SerializeAsString()); }   // the phone acknowledges each microphone frame
    }
    if ((id == 0x0000 || id == 0x0001) && onMicData) {
        if (id == 0x0000 && n >= 8) { uint64_t ts = 0; for (int i = 0; i < 8; ++i) ts = (ts << 8) | p[i]; onMicData(ts, p + 8, n - 8); }
        else onMicData(0, p, n);
        return;
    }
    LOG("microphone channel message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled");
}

void MdSession::sendNav(uint16_t id, const std::string& body) { if (nav_open_ && running_ && nav_channel_ >= 0) enqueue((uint8_t)nav_channel_, true, id, body); }
void MdSession::sendMedia(uint16_t id, const std::string& body) { if (media_open_ && running_ && media_channel_ >= 0) enqueue((uint8_t)media_channel_, true, id, body); }
void MdSession::sendNavFocus(const std::string& body) { if (running_) enqueue(0, true, ControlMessageType::MESSAGE_NAV_FOCUS_REQUEST, body); }

void MdSession::enqueue(uint8_t channel, bool encrypted, uint16_t msg_id, const std::string& body, uint8_t extra) {
    if (trace::ok()) LOG("t=" << msNow() << "ms TX ch=" << (int)channel << " id=0x" << std::hex << msg_id << std::dec << " len=" << body.size()+2 << " " << hexs((const uint8_t*)body.data(), body.size()));
    std::vector<uint8_t> v; v.reserve(2 + body.size());
    wr16(v, msg_id);
    v.insert(v.end(), body.begin(), body.end());
    enqueueRaw(channel, encrypted, std::move(v), extra);
}

void MdSession::enqueueRaw(uint8_t channel, bool encrypted, std::vector<uint8_t> plain, uint8_t extra) {
    std::lock_guard<std::mutex> enc_lk(enc_m_);   // encrypt + queue as one step (see enc_m_)
    // Fragment into frames. Each frame's payload is one TLS record when
    // encrypted. First frame carries the total plaintext length.
    std::vector<OutFrame> frames;
    size_t total = plain.size();
    size_t off = 0;
    bool single = total <= kMaxPlainChunk;
    while (off < total || (total == 0 && frames.empty())) {
        size_t chunk = std::min(kMaxPlainChunk, total - off);
        uint8_t ft = single ? FT_BULK : (off == 0 ? FT_FIRST : (off + chunk >= total ? FT_LAST : FT_MIDDLE));
        std::vector<uint8_t> payload;
        if (encrypted) {
            if (!sslEncrypt(plain.data() + off, chunk, payload)) { ERR("encrypt failed"); return; }
        } else {
            payload.assign(plain.begin() + off, plain.begin() + off + chunk);
        }
        OutFrame f;
        f.bytes.push_back(channel);
        f.bytes.push_back((encrypted ? FLAG_ENCRYPTED : 0) | ft | extra);
        wr16(f.bytes, (uint16_t)payload.size());
        if (ft == FT_FIRST) wr32(f.bytes, (uint32_t)total);
        f.bytes.insert(f.bytes.end(), payload.begin(), payload.end());
        frames.push_back(std::move(f));
        off += chunk;
        if (total == 0) break;
    }
    {
        std::lock_guard<std::mutex> lk(tx_m_);
        for (auto& f : frames) tx_.push_back(std::move(f));
    }
    tx_cv_.notify_one();
}

void MdSession::writerLoop() {
    while (running_) {
        OutFrame f;
        {
            std::unique_lock<std::mutex> lk(tx_m_);
            tx_cv_.wait_for(lk, std::chrono::milliseconds(200), [&]{ return !tx_.empty() || !running_; });
            if (!running_) break;
            if (tx_.empty()) continue;
            f = std::move(tx_.front());
            tx_.pop_front();
        }
        if (!writeAll(f.bytes.data(), f.bytes.size())) {
            ERR("write(ep IN) failed: " << strerror(errno) << " -> head unit gone");
            endSession("write failed");
            break;
        }
    }
}

bool MdSession::writeAll(const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(write_fd_, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}

// ---------------------------------------------------------------------------
// TLS server side (mirror of AACS AaCommunicator::initializeSslContext)
// ---------------------------------------------------------------------------
static int verify_accept_all(int, X509_STORE_CTX*) { return 1; }

bool MdSession::initSsl() {
    if (ctx_) return true;
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) { ERR("SSL_CTX_new failed"); return false; }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_TLSv1_3);
    SSL_CTX_set_ecdh_auto(ctx_, 1);
    std::string cert = config_.md_cert, key = config_.md_key;
    if (SSL_CTX_use_certificate_file(ctx_, cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
        ERR("cannot load MD certificate " << cert); ERR_print_errors_fp(stderr); return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key.c_str(), SSL_FILETYPE_PEM) <= 0) {
        ERR("cannot load MD key " << key); ERR_print_errors_fp(stderr); return false;
    }
    if (!SSL_CTX_check_private_key(ctx_)) { ERR("MD cert/key mismatch"); return false; }
    if (!config_.md_dhparam.empty()) {
        if (FILE* f = fopen(config_.md_dhparam.c_str(), "r")) {
            DH* dh = PEM_read_DHparams(f, nullptr, nullptr, nullptr);
            fclose(f);
            if (dh) { SSL_CTX_set_tmp_dh(ctx_, dh); DH_free(dh); }
            else ERR("dhparam file unreadable, continuing with ECDHE only");
        }
    }
    // Old head units offer legacy suites; don't restrict beyond defaults.
    SSL_CTX_set_security_level(ctx_, 0);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, verify_accept_all);
    // Trust anchor only matters if we ever enable real verification.
    if (!config_.md_ca.empty()) SSL_CTX_load_verify_locations(ctx_, config_.md_ca.c_str(), nullptr);

    ssl_ = SSL_new(ctx_);
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl_, rbio_, wbio_);
    SSL_set_accept_state(ssl_);
    LOG("TLS server context ready (cert " << cert << ")");
    return true;
}

void MdSession::drainWriteBio(std::vector<uint8_t>& out) {
    uint8_t buf[4096];
    int len;
    while ((len = BIO_read(wbio_, buf, sizeof buf)) > 0)
        out.insert(out.end(), buf, buf + len);
}

void MdSession::onSslHandshake(const uint8_t* p, size_t n) {
    std::vector<uint8_t> reply;
    {
        std::lock_guard<std::mutex> lk(ssl_m_);
        BIO_write(rbio_, p, (int)n);
        int r = SSL_do_handshake(ssl_);
        if (r <= 0) {
            int e = SSL_get_error(ssl_, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                ERR("TLS handshake failed: " << ERR_error_string(ERR_get_error(), nullptr));
                endSession("TLS handshake failed");
                return;
            }
        }
        drainWriteBio(reply);
        if (SSL_is_init_finished(ssl_) && !tls_done_) {
            tls_done_ = true;
            LOG("TLS handshake complete: " << SSL_get_version(ssl_) << " " << SSL_get_cipher(ssl_)
                << " -> waiting for AUTH_COMPLETE");
        }
    }
    if (!reply.empty()) {
        std::vector<uint8_t> v; wr16(v, ControlMessageType::MESSAGE_ENCAPSULATED_SSL);
        v.insert(v.end(), reply.begin(), reply.end());
        enqueueRaw(0, false, std::move(v));
    }
}

std::vector<uint8_t> MdSession::sslDecrypt(const std::vector<uint8_t>& ct) {
    std::lock_guard<std::mutex> lk(ssl_m_);
    std::vector<uint8_t> out;
    if (ct.empty()) return out;
    BIO_write(rbio_, ct.data(), (int)ct.size());
    uint8_t buf[16384 + 64];
    for (;;) {
        int r = SSL_read(ssl_, buf, sizeof buf);
        if (r > 0) { out.insert(out.end(), buf, buf + r); continue; }
        int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ) break;
        ERR("SSL_read error " << e << ": " << ERR_error_string(ERR_get_error(), nullptr));
        break;
    }
    return out;
}

bool MdSession::sslEncrypt(const uint8_t* p, size_t n, std::vector<uint8_t>& ct) {
    std::lock_guard<std::mutex> lk(ssl_m_);
    int r = SSL_write(ssl_, p, (int)n);
    if (r <= 0) { ERR("SSL_write error " << SSL_get_error(ssl_, r)); return false; }
    drainWriteBio(ct);
    return true;
}
