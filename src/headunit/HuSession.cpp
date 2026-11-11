#include "HuSession.h"
#include <iterator>
#include <cstdio>
#include <fstream>
#include "../Trace.h"
#include "../LatencyProbe.h"
#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusRequest.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/navigationstatus/NavigationStatusService.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>
#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/control/message/UpdateUiConfigRequest.pb.h>
#include <aap_protobuf/service/control/message/UpdateUiConfigReply.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/shared/message/Start.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>
#include <aap_protobuf/service/inputsource/InputSourceService.pb.h>
#include <aap_protobuf/service/media/source/MediaSourceService.pb.h>
#include <aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/PointerAction.pb.h>
#include <aap_protobuf/service/sensorsource/SensorSourceService.pb.h>
#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h>
#include <aap_protobuf/service/sensorsource/message/NightModeData.pb.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

namespace ctl = aap_protobuf::service::control::message;
namespace med = aap_protobuf::service::media;
namespace ins = aap_protobuf::service::inputsource::message;
namespace sns = aap_protobuf::service::sensorsource::message;
using aap_protobuf::service::control::message::ControlMessageType;

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint64_t rd64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }
static inline void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xff); }
static inline void wr32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static std::string hexs(const uint8_t* d, size_t n, size_t max = 24) { std::string s; char b[4]; for (size_t i = 0; i < n && i < max; ++i) { snprintf(b, sizeof b, "%02x", d[i]); s += b; } if (n > max) s += "..."; return s; }
// (call with sensor_m_ held) the relayed types in effect: the vehicle's list once known, else the usual set so a phone that connects before the vehicle is still offered them
static std::vector<int> effectiveTypes(bool known, bool relay, const std::vector<int>& v) { if (!relay) return {}; return known ? v : std::vector<int>{3, 6, 7, 8}; }
static long msNow() { static auto t0 = std::chrono::steady_clock::now(); return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); }
#define LOG(x) do { std::cout << "[HuSession] " << x << std::endl; } while (0)
#define ERR(x) do { std::cerr << "[HuSession] " << x << std::endl; } while (0)
static int verify_accept_all(int, X509_STORE_CTX*) { return 1; }

HuSession::HuSession(const Config& cfg, int fd) : cfg_(cfg), fd_(fd), vw_(cfg.video_width), vh_(cfg.video_height) {}

HuSession::~HuSession() {
    stop();
    if (writer_.joinable()) writer_.join();
    if (ssl_) SSL_free(ssl_);       // frees both BIOs
    if (ctx_) SSL_CTX_free(ctx_);
}

void HuSession::stop() {
    running_ = false;
    tx_cv_.notify_all();
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);   // wakes a blocked poll/read in the reader
}

void HuSession::endSession(const std::string& why) {
    std::cout << ring_.dump("HuSession") << std::endl;   // what the phone said last
    LOG("ending session: " << why);
    running_ = false;
    tx_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// run / IO
// ---------------------------------------------------------------------------
void HuSession::run() {
    if (!initSsl()) { ERR("TLS init failed, session not started"); return; }
    running_ = true;
    trace::newSession();
    writer_ = std::thread([this] { writerLoop(); });
    touch_flusher_ = std::thread([this] { touchFlusherLoop(); });
    LOG("phone connected; sending VERSION_REQUEST");
    { std::vector<uint8_t> v; wr16(v, ControlMessageType::MESSAGE_VERSION_REQUEST); wr16(v, 1); wr16(v, 6); enqueueRaw(0, false, std::move(v)); }
    readerLoop();
    running_ = false;
    tx_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (touch_flusher_.joinable()) touch_flusher_.join();
    if (probe_thr_.joinable()) probe_thr_.join();
    video_started_ = false;
    LOG("session ended");
}

void HuSession::readerLoop() {
    std::vector<uint8_t> buf(64 * 1024);
    while (running_) {
        pollfd pfd{fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 500);
        if (pr == 0) continue;
        if (pr < 0) { if (errno == EINTR) continue; break; }
        ssize_t n = ::read(fd_, buf.data(), buf.size());
        if (n == 0) { LOG("phone closed the connection (TCP FIN from the phone; its last message was " << msNow() - last_rx_ms_ << " ms ago, " << rx_msgs_ << " received this session)"); break; }
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; ERR("read: " << strerror(errno) << " (phone link lost; its last message was " << msNow() - last_rx_ms_ << " ms ago)"); break; }
        last_rx_ms_ = msNow(); ++rx_msgs_;
        rx_acc_.insert(rx_acc_.end(), buf.begin(), buf.begin() + n);
        parseIncoming();
    }
}

void HuSession::parseIncoming() {
    size_t off = 0;
    while (rx_acc_.size() - off >= 4) {
        const uint8_t* h = rx_acc_.data() + off;
        uint8_t ch = h[0], flags = h[1];
        uint16_t len = rd16(h + 2);
        size_t hdr = 4;
        if ((flags & 0x03) == FT_FIRST) hdr += 4;
        if (rx_acc_.size() - off < hdr + len) break;
        std::vector<uint8_t> payload(h + hdr, h + hdr + len);
        off += hdr + len;
        onFrame(ch, flags, std::move(payload));
    }
    if (off) rx_acc_.erase(rx_acc_.begin(), rx_acc_.begin() + off);
}

void HuSession::onFrame(uint8_t ch, uint8_t flags, std::vector<uint8_t> payload) {
    std::vector<uint8_t> plain;
    if (flags & FLAG_ENCRYPTED) plain = sslDecrypt(payload); else plain = std::move(payload);
    uint8_t ft = flags & 0x03;
    if (ft == FT_BULK) { if (!plain.empty()) onMessage(ch, plain); return; }
    if (ft == FT_FIRST) reassembly_[ch].clear();
    reassembly_[ch].insert(reassembly_[ch].end(), plain.begin(), plain.end());
    if (ft == FT_LAST) { std::vector<uint8_t> whole; whole.swap(reassembly_[ch]); if (!whole.empty()) onMessage(ch, whole); }
}

void HuSession::onMessage(uint8_t ch, const std::vector<uint8_t>& msg) {
    if (msg.size() < 2) return;
    uint16_t id = rd16(msg.data());
    const uint8_t* p = msg.data() + 2; size_t n = msg.size() - 2;
    ring_.push(true, ch, id, msg.size());
    if (id != 0x0000 && id != 0x0001 && id != 0x0b && id != 0x0c)
        if (trace::ok()) LOG("t=" << msNow() << "ms RX ch=" << (int)ch << " id=0x" << std::hex << id << std::dec << " len=" << msg.size() << " " << hexs(p, n));
    if (ch == 0) onControl(id, p, n);
    else if (id == ControlMessageType::MESSAGE_CHANNEL_OPEN_REQUEST) {
        ctl::ChannelOpenRequest rq; rq.ParsePartialFromArray(p, (int)n);
        ctl::ChannelOpenResponse rs; rs.set_status(aap_protobuf::shared::STATUS_SUCCESS);
        LOG("channel " << (int)ch << " open request -> OK");
        enqueue(ch, true, ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE, rs.SerializeAsString(), FLAG_CONTROL);
        if (ch == CH_INPUT) input_open_ = true;
        if (ch == CH_NAV && cfg_.relay_nav && onNavOpen) onNavOpen();
        if (ch == CH_MEDIA_STATUS && cfg_.relay_media_status && onMediaOpen) onMediaOpen();
    }
    else if (ch == CH_NAV && cfg_.relay_nav) { if (onNavMessage) onNavMessage(id, std::string((const char*)p, n)); }
    else if (ch == CH_MEDIA_STATUS && cfg_.relay_media_status) { if (onMediaMessage) onMediaMessage(id, std::string((const char*)p, n)); }
    else if (ch == CH_VIDEO) onVideo(id, p, n);
    else if (ch == CH_INPUT) onInput(id, p, n);
    else if (ch == CH_SENSOR) onSensor(id, p, n);
    else if (ch == CH_MIC) onMic(id, p, n);
    else if (ch >= CH_AUD_MEDIA && ch < CH_MIC) onAudio(ch, id, p, n);
    else if (onOtherMessage) onOtherMessage((int)ch, id, std::string((const char*)p, n));
    else LOG("message id 0x" << std::hex << id << std::dec << " on unhandled channel " << (int)ch);
}

// ---------------------------------------------------------------------------
// control channel
// ---------------------------------------------------------------------------
void HuSession::onControl(uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
    case ControlMessageType::MESSAGE_VERSION_RESPONSE:
        if (n >= 4) LOG("phone AA version " << rd16(p) << "." << rd16(p + 2) << (n >= 6 ? (rd16(p + 4) == 0 ? " (match)" : " (MISMATCH)") : ""));
        startTls();
        return;
    case ControlMessageType::MESSAGE_ENCAPSULATED_SSL:
        onSslHandshake(p, n);
        return;
    case ControlMessageType::MESSAGE_SERVICE_DISCOVERY_REQUEST:
        {   // keep the phone's own introduction (icons, names, phone info) so the vehicle-facing side can repeat it verbatim (config truck_mirror_phone)
            const char* out = getenv("AA_TEST_SDREQ_OUT"); const std::string path = out ? out : cfg_.sdreq_file;
            if (!path.empty() && n > 0) {
                std::string old; { std::ifstream f(path, std::ios::binary); if (f) old.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
                if (old.size() != n || std::memcmp(old.data(), p, n) != 0) {
                    const std::string tmp = path + ".tmp";
                    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (f) f.write((const char*)p, (std::streamsize)n); }
                    if (::rename(tmp.c_str(), path.c_str()) == 0) LOG("recorded the phone's service-discovery request (" << n << " bytes) in " << path);
                    else LOG("could not record the phone's service-discovery request in " << path);
                }
            }
        }
        { ctl::ServiceDiscoveryRequest rq; if (rq.ParsePartialFromArray(p, (int)n)) { rq.clear_small_icon(); rq.clear_medium_icon(); rq.clear_large_icon();
              LOG("phone SDR request: " << rq.ShortDebugString()); } }
        LOG("SERVICE_DISCOVERY_REQUEST -> sending channel list");
        sendServiceDiscoveryResponse();
        return;
    case ControlMessageType::MESSAGE_PING_REQUEST: {
        ctl::PingRequest rq; rq.ParsePartialFromArray(p, (int)n);
        ctl::PingResponse rs; rs.set_timestamp(rq.timestamp());
        enqueue(0, true, ControlMessageType::MESSAGE_PING_RESPONSE, rs.SerializeAsString());
        return; }
    case ControlMessageType::MESSAGE_PING_RESPONSE: return;
    case ControlMessageType::MESSAGE_AUDIO_FOCUS_REQUEST: {
        ctl::AudioFocusRequest rq; rq.ParsePartialFromArray(p, (int)n);
        ctl::AudioFocusNotification nt;
        bool release = rq.has_audio_focus_type() && rq.audio_focus_type() == ctl::AUDIO_FOCUS_RELEASE;
        nt.set_focus_state(release ? ctl::AUDIO_FOCUS_STATE_LOSS : ctl::AUDIO_FOCUS_STATE_GAIN);
        LOG("audio focus request -> " << (release ? "LOSS" : "GAIN"));
        if (cfg_.relay_audio && onControlRaw) onControlRaw((uint16_t)id, std::string((const char*)p, n));   // only when audio is relayed (else the vehicle would give focus to a stream that never comes). The vehicle is the real arbiter: it hears the request too, its notification comes back raw
        enqueue(0, true, ControlMessageType::MESSAGE_AUDIO_FOCUS_NOTIFICATION, nt.SerializeAsString());
        return; }
    case ControlMessageType::MESSAGE_NAV_FOCUS_REQUEST: {
        if (cfg_.relay_nav && onNavFocus) onNavFocus(std::string((const char*)p, n));   // the vehicle gets the request too (its answer is not awaited: the phone is answered locally)
        ctl::NavFocusNotification nt; nt.set_focus_type(ctl::NAV_FOCUS_PROJECTED);
        enqueue(0, true, ControlMessageType::MESSAGE_NAV_FOCUS_NOTIFICATION, nt.SerializeAsString());
        return; }
    case ControlMessageType::MESSAGE_BYEBYE_REQUEST: {
        ctl::ByeByeResponse rs;
        enqueue(0, true, ControlMessageType::MESSAGE_BYEBYE_RESPONSE, rs.SerializeAsString());
        endSession("phone sent BYEBYE_REQUEST");
        return; }
    case ControlMessageType::MESSAGE_BATTERY_STATUS_NOTIFICATION:
    case ControlMessageType::MESSAGE_VOICE_SESSION_NOTIFICATION:
    case ControlMessageType::MESSAGE_CALL_AVAILABILITY_STATUS:
        if (cfg_.relay_control && onControlRaw) onControlRaw((uint16_t)id, std::string((const char*)p, n));
        return;
    default:
        LOG("control message id " << id << " (" << n << " bytes) not handled");
    }
}

void HuSession::sendServiceDiscoveryResponse() {
    ctl::ServiceDiscoveryResponse r;
    r.set_make("Ford"); r.set_model("F-150 Lightning"); r.set_year("2023"); r.set_vehicle_id("AABR-001");
    r.set_head_unit_make("AA-Bridge"); r.set_head_unit_model("1.0"); r.set_head_unit_software_build("1"); r.set_head_unit_software_version("1.0.0");
    r.set_display_name("AA-Bridge");
    {   // modern phones read headunit_info / driver_position / ping configuration; legacy top-level fields above stay for older ones
        auto* hi = r.mutable_headunit_info();
        hi->set_make("Ford"); hi->set_model("F-150 Lightning"); hi->set_year("2023"); hi->set_vehicle_id("AABR-001");
        hi->set_head_unit_make("AA-Bridge"); hi->set_head_unit_model("1.0"); hi->set_head_unit_software_build("1"); hi->set_head_unit_software_version("1.0.0");
        r.set_driver_position(ctl::DRIVER_POSITION_LEFT);
        r.set_session_configuration(0);
        auto* pc = r.mutable_connection_configuration()->mutable_ping_configuration();
        pc->set_timeout_ms(5000); pc->set_interval_ms(1500); pc->set_high_latency_threshold_ms(500); pc->set_tracked_ping_count(5);
    }
    {   // video sink
        auto* s = r.add_channels(); s->set_id(CH_VIDEO);
        auto* ms = s->mutable_media_sink_service();
        ms->set_available_type(med::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
        ms->set_available_while_in_call(true);
        auto* vc = ms->add_video_configs();
        vc->set_codec_resolution(vh_ >= 1080 ? med::sink::message::VIDEO_1920x1080 : med::sink::message::VIDEO_1280x720);
        vc->set_frame_rate(med::sink::message::VIDEO_FPS_30);
        int ml = 0, mr = 0, mt = 0, mb = 0;
        if (win_w_ > 0 && win_h_ > 0) { ml = win_x_; mr = std::max(0, vw_ - win_x_ - win_w_); mt = win_y_; mb = std::max(0, vh_ - win_y_ - win_h_); }
        else if (cfg_.hu_width_margin > 0 || cfg_.hu_height_margin > 0) { ml = cfg_.hu_width_margin / 2; mr = cfg_.hu_width_margin - ml; mt = cfg_.hu_height_margin / 2; mb = cfg_.hu_height_margin - mt; }
        vc->set_width_margin(ml + mr); vc->set_height_margin(mt + mb);
        if (ml + mr + mt + mb > 0) {                      // like Ford: total margins plus exact per-side UI margins
            auto* mg = vc->mutable_ui_config()->mutable_margins();
            mg->set_left(ml); mg->set_right(mr); mg->set_top(mt); mg->set_bottom(mb);
        }
        LOG("advertising video " << vw_ << "x" << vh_ << " with UI margins l" << ml << " r" << mr << " t" << mt << " b" << mb);
        int dens = vp_.valid ? vp_.density : 160;
        if (vp_.valid && vp_.ref_win_w > 0 && win_w_ > 0) {   // same layout (dp size) the phone gets from the truck's biggest config, at our smaller pixel window
            dens = (int)(vp_.density * (double)win_w_ / vp_.ref_win_w + 0.5);
            if (dens < 80) dens = 80; if (dens > vp_.density) dens = vp_.density;
        }
        vc->set_density(dens);
        if (vp_.valid) {   // same display description Sync 4 gives the phone in a real car
            if (vp_.real_density > 0) vc->set_real_density(vp_.real_density);
            if (vp_.viewing_distance > 0) vc->set_viewing_distance(vp_.viewing_distance);
            if (vp_.pixel_aspect_ratio_e4 > 0) vc->set_pixel_aspect_ratio_e4(vp_.pixel_aspect_ratio_e4);
            vc->set_decoder_additional_depth(vp_.decoder_additional_depth);
        }
        LOG("advertising density " << dens << " (truck " << vp_.density << ", ref window " << vp_.ref_win_w << "x" << vp_.ref_win_h << ", ours " << win_w_ << "x" << win_h_ << ")");
        vc->set_video_codec_type(med::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    }
    {   // touchscreen (coordinates we accept == the video size)
        auto* s = r.add_channels(); s->set_id(CH_INPUT);
        auto* is = s->mutable_input_source_service();
        for (int kc : {3, 4, 19, 20, 21, 22, 23, 84, 85, 86, 87, 88, 126, 127}) is->add_keycodes_supported(kc);   // HOME BACK dpad SEARCH media keys
        auto* ts = is->add_touchscreen();
        ts->set_type(ins::CAPACITIVE);          // unspecified type may be read as single-touch (two-finger gestures were ignored in truck test 15)
        ts->set_width(vw_ - (win_w_ > 0 ? vw_ - win_w_ : cfg_.hu_width_margin)); ts->set_height(vh_ - (win_h_ > 0 ? vh_ - win_h_ : cfg_.hu_height_margin));
    }
    {   // sensors: driving status + night mode (so the phone does not restrict its UI)
        auto* s = r.add_channels(); s->set_id(CH_SENSOR);
        auto* ss = s->mutable_sensor_source_service();
        ss->add_sensors()->set_sensor_type(sns::SENSOR_DRIVING_STATUS_DATA);
        ss->add_sensors()->set_sensor_type(sns::SENSOR_NIGHT_MODE);
        ss->add_sensors()->set_sensor_type(sns::SENSOR_LOCATION);
        { std::lock_guard<std::mutex> l(sensor_m_);
          for (int t : effectiveTypes(relay_types_known_, cfg_.relay_sensors, relay_types_)) {
              auto* sn = ss->add_sensors();
              if (sns::SensorType_IsValid(t)) sn->set_sensor_type((sns::SensorType)t); else sn->mutable_unknown_fields()->AddVarint(1, (uint64_t)t);     // 23 / 24: newer than our protobuf library
              LOG("advertising relayed vehicle sensor type " << t << (relay_types_known_ ? "" : " (vehicle list not known yet: default set)")); }
          if (cfg_.relay_sensors) { for (int f : relay_fuel_) if (sns::FuelType_IsValid(f)) ss->add_supported_fuel_types((sns::FuelType)f);
              for (int e : relay_ev_) if (sns::EvConnectorType_IsValid(e)) ss->add_supported_ev_connector_types((sns::EvConnectorType)e);
              if (!relay_fuel_.empty() || !relay_ev_.empty()) LOG("advertising the vehicle's fuel/EV declaration to the phone (" << relay_fuel_.size() << " fuel types, " << relay_ev_.size() << " EV connectors)"); } }
    }
    auto audioSink = [&](int id, med::sink::message::AudioStreamType type, int rate, int ch) {
        auto* s = r.add_channels(); s->set_id(id);
        auto* ms = s->mutable_media_sink_service();
        ms->set_available_type(med::shared::message::MEDIA_CODEC_AUDIO_PCM);
        ms->set_audio_type(type);
        ms->set_available_while_in_call(true);
        auto* ac = ms->add_audio_configs(); ac->set_sampling_rate(rate); ac->set_number_of_bits(16); ac->set_number_of_channels(ch);
    };
    audioSink(CH_AUD_MEDIA, med::sink::message::AUDIO_STREAM_MEDIA, 48000, 2);
    audioSink(CH_AUD_GUIDE, med::sink::message::AUDIO_STREAM_GUIDANCE, cfg_.relay_audio ? 48000 : 16000, 1);   // relayed PCM must match what the vehicle's guidance sink accepts (Sync 4: 48 kHz mono)
    audioSink(CH_AUD_SYS,   med::sink::message::AUDIO_STREAM_SYSTEM_AUDIO, 16000, 1);
    if (cfg_.relay_media_status) { auto* s = r.add_channels(); s->set_id(CH_MEDIA_STATUS); s->mutable_media_playback_service(); }   // now-playing metadata, as the vehicle offers it
    if (cfg_.relay_nav) {   // navigation status service, as on the vehicle's own head unit (400 ms, enumerated maneuvers): the phone only sends turn-by-turn / EV forecast to a HU that has one
        auto* s = r.add_channels(); s->set_id(CH_NAV);
        auto* ns = s->mutable_navigation_status_service(); ns->set_minimum_interval_ms(400);
        ns->set_type(aap_protobuf::service::navigationstatus::NavigationStatusService::ENUM);
    }
    {   // microphone (phone -> we never provide audio yet, but real head units advertise one)
        auto* s = r.add_channels(); s->set_id(CH_MIC);
        auto* mic = s->mutable_media_source_service();
        mic->set_available_type(med::shared::message::MEDIA_CODEC_AUDIO_PCM);
        auto* ac = mic->mutable_audio_config(); ac->set_sampling_rate(16000); ac->set_number_of_bits(16); ac->set_number_of_channels(1);
    }
    if (const char* tb = getenv("AA_TEST_VEHICLE_BT")) {                      // test hook: pretend to be a vehicle that offers a Bluetooth service (as Sync 4 does)
        auto* s = r.add_channels(); s->set_id(10);                             // 9 is the media playback status channel
        auto* bs = s->mutable_bluetooth_service(); bs->set_car_address(tb); bs->add_supported_pairing_methods(aap_protobuf::service::bluetooth::message::BLUETOOTH_PAIRING_NUMERIC_COMPARISON);
    }
    LOG("SDR channels: " << r.channels_size() << " (" << r.SerializePartialAsString().size() << " bytes)");
    enqueue(0, true, ControlMessageType::MESSAGE_SERVICE_DISCOVERY_RESPONSE, r.SerializePartialAsString());
}

// ---------------------------------------------------------------------------
// video channel (phone -> HU)
// ---------------------------------------------------------------------------
void HuSession::sendUiConfig(int l, int r, int t, int b) {
    aap_protobuf::service::control::message::UpdateUiConfigRequest rq;
    auto* m = rq.mutable_ui_config()->mutable_margins();
    m->set_left(l); m->set_right(r); m->set_top(t); m->set_bottom(b);
    LOG("UpdateUiConfigRequest -> phone: " << rq.ShortDebugString());
    enqueue(CH_VIDEO, true, 0x8009, rq.SerializeAsString());
}

void HuSession::onVideo(uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
    case 0x8000: {   // SETUP
        med::shared::message::Setup su; su.ParsePartialFromArray(p, (int)n);
        med::shared::message::Config cf;
        cf.set_status(med::shared::message::Config::STATUS_READY);
        cf.set_max_unacked(4);
        cf.add_configuration_indices(0);
        LOG("video SETUP type=" << (int)su.type() << " -> CONFIG ready, max_unacked 4");
        enqueue(CH_VIDEO, true, 0x8003, cf.SerializeAsString());
        {   // Like a real head unit (Sync 4 does this to us): tell the phone, unprompted, that it owns the screen. Without it a phone in
            // first-time "finish setup on the car's screen" state never asks for focus and never starts streaming (truck test 13).
            med::video::message::VideoFocusNotification f;
            f.set_focus(med::video::message::VIDEO_FOCUS_PROJECTED); f.set_unsolicited(true);
            LOG("video focus: PROJECTED (unsolicited, as a real head unit)");
            enqueue(CH_VIDEO, true, 0x8008, f.SerializeAsString());
        }
        return; }
    case 0x8007: {   // VIDEO_FOCUS_REQUEST
        med::video::message::VideoFocusNotification f;
        f.set_focus(med::video::message::VIDEO_FOCUS_PROJECTED); f.set_unsolicited(false);
        LOG("video focus request -> PROJECTED");
        enqueue(CH_VIDEO, true, 0x8008, f.SerializeAsString());
        return; }
    case 0x8001: {   // START
        med::shared::message::Start st; st.ParsePartialFromArray(p, (int)n);
        session_id_ = st.session_id(); video_started_ = true;
        LOG("video START session=" << session_id_ << " config=" << st.configuration_index());
        flushPendingUiConfig();
        if (cfg_.split_probe_s > 0 && !probe_started_) {
            probe_started_ = true;
            probe_thr_ = std::thread([this] {
                for (int i = 0; i < cfg_.split_probe_s * 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!running_ || !video_started_) return;
                LOG("SPLIT PROBE: reshaping the phone UI to ~half width (margins L" << vw_ / 4 << " R" << vw_ / 4 << ") after " << cfg_.split_probe_s << " s; frames so far " << frames_rx_.load());
                sendUiConfig(vw_ / 4, vw_ / 4, 0, 0);
            });
        }
        return; }
    case 0x8002: video_started_ = false; LOG("video STOP"); return;
    case 0x0001:     // MEDIA: codec config (SPS/PPS), untimestamped
        LOG("codec config " << n << " bytes");
        if (frame_sink_) frame_sink_(p, n, 0);
        return;
    case 0x0000: {   // MEDIA_WITH_TIMESTAMP: 8-byte BE timestamp + Annex-B access unit
        if (n < 8) return;
        uint64_t ts = rd64(p);
        uint64_t k = ++frames_rx_;
        { auto& lp = LatencyProbe::get(); if (lp.in_left > 0) { int64_t d = probeNowMs() - lp.touch_ms; if (d > 1500) lp.in_left = 0; else { --lp.in_left; LOG("LAT touch -> phone frame in: " << d << " ms (" << (n - 8) << " bytes)"); } } }
        if (k == 1 || k % 300 == 0) LOG("phone video frames received: " << k << " (last " << (n - 8) << " bytes)");
        if (frame_sink_) frame_sink_(p + 8, n - 8, ts);
        { std::lock_guard<std::mutex> lk(ack_m_); ack_ts_.push_back(probeNowMs()); }
        flushAcks();
        if (k % 300 == 0) { std::lock_guard<std::mutex> lk(ack_m_);
            LOG("flow control: " << ack_ts_.size() << " ACKs held now, max " << ack_backlog_max_ << ", forced " << acks_forced_ << ", decode queue " << (queue_depth_ ? queue_depth_() / 1024 : 0) << " KB");
            ack_backlog_max_ = 0; }
        return; }
    case 0x800A: {   // UPDATE_UI_CONFIG_REPLY
        aap_protobuf::service::control::message::UpdateUiConfigReply rp; rp.ParsePartialFromArray(p, (int)n);
        LOG("UpdateUiConfigReply: " << rp.ShortDebugString() << " (" << n << " bytes)");
        if (onUiConfigReply) onUiConfigReply();
        return; }
    default:
        LOG("video message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled");
    }
}

void HuSession::onInput(uint16_t id, const uint8_t* p, size_t n) {
    if (id == 0x8002) {   // BINDING_REQUEST
        LOG("input binding request (" << n << " bytes) -> OK");
        enqueue(CH_INPUT, true, 0x8003, std::string("\x08\x00", 2));
    } else LOG("input message 0x" << std::hex << id << std::dec << " not handled");
    (void)p;
}

void HuSession::onAudio(uint8_t ch, uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
    case 0x8000: {   // SETUP -> ready
        med::shared::message::Config cf; cf.set_status(med::shared::message::Config::STATUS_READY); cf.set_max_unacked(4); cf.add_configuration_indices(0);
        LOG("audio ch " << (int)ch << " SETUP -> ready");
        enqueue(ch, true, 0x8003, cf.SerializeAsString());
        return; }
    case 0x8001: { med::shared::message::Start st; st.ParsePartialFromArray(p, (int)n); audio_session_[ch] = st.session_id(); LOG("audio ch " << (int)ch << " START");
        const int st_idx = ch == CH_AUD_MEDIA ? 0 : ch == CH_AUD_GUIDE ? 1 : -1; if (cfg_.relay_audio && st_idx >= 0 && onAudioStart) onAudioStart(st_idx, st.session_id()); return; }
    case 0x8002: LOG("audio ch " << (int)ch << " STOP"); { const int st_idx = ch == CH_AUD_MEDIA ? 0 : ch == CH_AUD_GUIDE ? 1 : -1; if (cfg_.relay_audio && st_idx >= 0 && onAudioStop) onAudioStop(st_idx); } return;
    case 0x0000: case 0x0001: {   // PCM data (we do not play it yet): ACK so the phone keeps sending
        { const int st_idx = ch == CH_AUD_MEDIA ? 0 : ch == CH_AUD_GUIDE ? 1 : -1;
          if (cfg_.relay_audio && st_idx >= 0 && onAudioData) {
              if (id == 0x0000 && n >= 8) { uint64_t ts = 0; for (int i = 0; i < 8; ++i) ts = (ts << 8) | p[i]; onAudioData(st_idx, ts, p + 8, n - 8); }
              else onAudioData(st_idx, 0, p, n); } }
        med::source::message::Ack ack; ack.set_session_id(audio_session_[ch]); ack.set_ack(1);
        enqueue(ch, true, 0x8004, ack.SerializeAsString());
        return; }
    default: LOG("audio ch " << (int)ch << " message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled");
    }
}

// Microphone (AVInput) channel: the phone asks to open it (0x8005), we answer (0x8006) and then stream PCM that the vehicle's microphone produced.
void HuSession::onMic(uint16_t id, const uint8_t* p, size_t n) {
    if (!cfg_.relay_audio) { LOG("microphone channel message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled (relay_audio=0)"); return; }   // 'off' keeps the old behaviour exactly
    if (id == 0x8005) {
        aap_protobuf::service::media::source::message::MicrophoneRequest rq; rq.ParsePartialFromArray(p, (int)n);
        aap_protobuf::service::media::source::message::MicrophoneResponse rs; rs.set_status(0); rs.set_session_id(mic_session_);
        LOG("microphone " << (rq.open() ? "OPEN" : "CLOSE") << " request from the phone");
        enqueue(CH_MIC, true, 0x8006, rs.SerializeAsString());
        if (cfg_.relay_audio && onMicRequest) onMicRequest(rq.open());
        return;
    }
    if (id == 0x8004 || id == 0x8001 || id == 0x8002) return;     // acks / start / stop of a source: nothing to do
    LOG("microphone channel message 0x" << std::hex << id << std::dec << " (" << n << " bytes) not handled");
}
void HuSession::sendMic(uint64_t ts_us, const uint8_t* pcm, size_t n) {
    std::vector<uint8_t> v; v.reserve(2 + 8 + n); v.push_back(0); v.push_back(0);
    for (int i = 7; i >= 0; --i) v.push_back((uint8_t)(ts_us >> (8 * i)));
    v.insert(v.end(), pcm, pcm + n);
    enqueueRaw(CH_MIC, true, std::move(v));
}

void HuSession::onSensor(uint16_t id, const uint8_t* p, size_t n) {
    if (id != 0x8001) { LOG("sensor message 0x" << std::hex << id << std::dec << " not handled"); return; }
    int type = (n >= 2 && p[0] == 0x08) ? p[1] : -1;
    LOG("sensor start request type=" << type);
    enqueue(CH_SENSOR, true, 0x8002, std::string("\x08\x00", 2));
    sns::SensorBatch b; std::string lk_cached; bool have_cached = false;
    {
        std::lock_guard<std::mutex> l(sensor_m_);
        if (type == sns::SENSOR_DRIVING_STATUS_DATA) { sensor_driving_ = true; b.add_driving_status_data()->set_status(driving_status_); }
        else if (type == sns::SENSOR_NIGHT_MODE) { sensor_night_ = true; b.add_night_mode_data()->set_night_mode(night_); }
        else if ([&] { auto e = effectiveTypes(relay_types_known_, cfg_.relay_sensors, relay_types_); return std::find(e.begin(), e.end(), type) != e.end(); }()) {      // relayed vehicle sensor: answer, then send the last reading we have
            relay_started_[type] = true; auto it = relay_cache_.find(type);
            if (it == relay_cache_.end()) return;
            lk_cached = it->second; have_cached = true;
        }
        else return;
    }
    if (have_cached) { LOG("sensor type " << type << " relayed: sending the last vehicle reading"); enqueue(CH_SENSOR, true, 0x8003, lk_cached); return; }
    enqueue(CH_SENSOR, true, 0x8003, b.SerializeAsString());
}

static bool isRelayable(int t) { return t == 2 || t == 3 || t == 4 || t == 5 || t == 6 || t == 7 || t == 8 || t == 23 || t == 24; }   // compass speed rpm odometer fuel parking-brake gear
void HuSession::setVehicleSensorTypes(const std::vector<int>& offered) {
    std::lock_guard<std::mutex> l(sensor_m_); relay_types_.clear(); relay_types_known_ = !offered.empty() || relay_types_known_;
    if (!cfg_.relay_sensors) return;
    for (int t : offered) if (isRelayable(t) && std::find(relay_types_.begin(), relay_types_.end(), t) == relay_types_.end()) relay_types_.push_back(t);
}
// One raw SensorBatch from the vehicle: split by sensor type, remember the last reading of each (sent when the phone subscribes) and forward what the phone subscribed to.
// Driving status and night mode are NOT forwarded: those are ours (UI overrides). Location/GPS stay the phone's own. Rate limits keep the phone link quiet.
void HuSession::relayVehicleSensors(const std::string& raw) {
    if (!cfg_.relay_sensors) return;
    sns::SensorBatch in; if (!in.ParsePartialFromString(raw)) return;
    struct Out { int type; std::string bytes; };
    std::vector<Out> out; const int64_t now = msNow();
    auto take = [&](int type, int n, auto fill, int min_ms) {
        if (n <= 0) return; sns::SensorBatch b; fill(b); std::string s = b.SerializeAsString();
        std::lock_guard<std::mutex> l(sensor_m_);
        { auto e = effectiveTypes(relay_types_known_, cfg_.relay_sensors, relay_types_); if (std::find(e.begin(), e.end(), type) == e.end()) return; }
        relay_cache_[type] = s;
        if (!relay_started_[type]) return;
        auto& last = relay_last_ms_[type]; if (min_ms > 0 && now - last < min_ms) return;
        last = now; out.push_back({type, s}); };
    const int last = 1;
    take(3, in.speed_data_size(), [&](sns::SensorBatch& b) { b.add_speed_data()->CopyFrom(in.speed_data(in.speed_data_size() - last)); }, 200);
    take(8, in.gear_data_size(), [&](sns::SensorBatch& b) { b.add_gear_data()->CopyFrom(in.gear_data(in.gear_data_size() - last)); }, 0);
    take(7, in.parking_brake_data_size(), [&](sns::SensorBatch& b) { b.add_parking_brake_data()->CopyFrom(in.parking_brake_data(in.parking_brake_data_size() - last)); }, 0);
    take(6, in.fuel_data_size(), [&](sns::SensorBatch& b) { b.add_fuel_data()->CopyFrom(in.fuel_data(in.fuel_data_size() - last)); }, 1000);
    take(5, in.odometer_data_size(), [&](sns::SensorBatch& b) { b.add_odometer_data()->CopyFrom(in.odometer_data(in.odometer_data_size() - last)); }, 1000);
    take(4, in.rpm_data_size(), [&](sns::SensorBatch& b) { b.add_rpm_data()->CopyFrom(in.rpm_data(in.rpm_data_size() - last)); }, 200);
    take(2, in.compass_data_size(), [&](sns::SensorBatch& b) { b.add_compass_data()->CopyFrom(in.compass_data(in.compass_data_size() - last)); }, 500);
    for (int u = 0; u < in.unknown_fields().field_count(); ++u) {            // 23 / 24: raw, unchanged
        const auto& uf = in.unknown_fields().field(u); const int t = (int)uf.number(); if (t != 23 && t != 24) continue;
        sns::SensorBatch b2;
        if (uf.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) b2.mutable_unknown_fields()->AddLengthDelimited(t, uf.length_delimited());
        else if (uf.type() == google::protobuf::UnknownField::TYPE_VARINT) b2.mutable_unknown_fields()->AddVarint(t, uf.varint());
        else continue;
        const std::string s = b2.SerializeAsString(); std::lock_guard<std::mutex> l(sensor_m_);
        { auto e = effectiveTypes(relay_types_known_, cfg_.relay_sensors, relay_types_); if (std::find(e.begin(), e.end(), t) == e.end()) continue; }
        relay_cache_[t] = s; if (!relay_started_[t]) continue;
        auto& last = relay_last_ms_[t]; if (now - last < 500) continue; last = now; out.push_back({t, s});
    }
    for (auto& o : out) {
        if (relay_sent_++ % 100 == 0) LOG("vehicle sensor " << o.type << " -> phone (" << relay_sent_ << " sent so far)");
        enqueue(CH_SENSOR, true, 0x8003, o.bytes);
    }
}

void HuSession::setPhoneSensors(int driving_status, bool night) {
    sns::SensorBatch bd, bn; bool sd = false, sn = false;
    {
        std::lock_guard<std::mutex> l(sensor_m_);
        if (driving_status != driving_status_) { driving_status_ = driving_status; if (sensor_driving_) { bd.add_driving_status_data()->set_status(driving_status_); sd = true; } }
        if (night != night_) { night_ = night; if (sensor_night_) { bn.add_night_mode_data()->set_night_mode(night_); sn = true; } }
    }
    if (sd) { LOG("sensor -> phone: driving_status=" << driving_status); enqueue(CH_SENSOR, true, 0x8003, bd.SerializeAsString()); }
    if (sn) { LOG("sensor -> phone: night_mode=" << (night ? 1 : 0)); enqueue(CH_SENSOR, true, 0x8003, bn.SerializeAsString()); }
}

void HuSession::sendKey(int keycode, bool down, bool longpress) {
    if (!running_ || !input_open_) return;
    ins::InputReport r;
    r.set_timestamp((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    auto* k = r.mutable_key_event()->add_keys(); k->set_keycode((uint32_t)keycode); k->set_down(down); k->set_metastate(0); k->set_longpress(longpress);
    LOG("key " << keycode << (down ? " down" : " up") << " -> phone");
    enqueue(CH_INPUT, true, 0x8001, r.SerializeAsString());
}

void HuSession::sendTouch(int x, int y, int action) {
    TouchFrame f; f.action = action; f.index = 0; f.p.push_back({0, x, y});
    sendTouchFrame(f);
}

void HuSession::sendTouchFrame(const TouchFrame& f) {
    if (!running_ || !input_open_ || f.p.empty()) return;
    std::lock_guard<std::mutex> lk(touch_m_);
    const int64_t now = probeNowMs();
    if (f.action == 2) {                                   // MOVE
        if (now - touch_last_sent_ms_ >= 16) { touch_has_pending_ = false; touch_last_sent_ms_ = now; emitTouch(f); }
        else { touch_pending_ = f; touch_has_pending_ = true; }
        return;
    }
    if (touch_has_pending_) { touch_has_pending_ = false; emitTouch(touch_pending_); }   // keep order: last move before down/up
    touch_last_sent_ms_ = now;
    emitTouch(f);
}

void HuSession::flushAcks() {
    std::lock_guard<std::mutex> lk(ack_m_);
    if (ack_ts_.size() > ack_backlog_max_) ack_backlog_max_ = ack_ts_.size();
    const int64_t now = probeNowMs();
    while (!ack_ts_.empty()) {
        const bool ok = !queue_depth_ || queue_depth_() <= ack_limit_;
        const bool stale = now - ack_ts_.front() > 1500;      // never deadlock the phone
        if (!ok && !stale) break;
        if (!ok) ++acks_forced_;
        ack_ts_.pop_front();
        med::source::message::Ack ack; ack.set_session_id(session_id_); ack.set_ack(1);
        enqueue(CH_VIDEO, true, 0x8004, ack.SerializeAsString());
    }
}

void HuSession::touchFlusherLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        flushAcks();
        std::lock_guard<std::mutex> lk(touch_m_);
        if (touch_has_pending_ && probeNowMs() - touch_last_sent_ms_ >= 16) { touch_has_pending_ = false; touch_last_sent_ms_ = probeNowMs(); emitTouch(touch_pending_); }
    }
}

void HuSession::emitTouch(const TouchFrame& f) {
    if (!running_ || !input_open_ || f.p.empty()) return;
    if (f.action == 0) LatencyProbe::get().touched();
    ins::InputReport r;
    // Android Auto input timestamps are NANOseconds (Sync 4 sends ns: moves 6 ms apart differ by 6,000,000). We sent microseconds until int9: every event looked 1000x closer together to the phone.
    r.set_timestamp((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    auto* t = r.mutable_touch_event();
    for (const auto& q : f.p) { auto* pt = t->add_pointer_data(); pt->set_x(q.x); pt->set_y(q.y); pt->set_pointer_id(q.id); }
    t->set_action_index(std::clamp(f.index, 0, (int)f.p.size() - 1));
    ins::PointerAction a = ins::ACTION_MOVED;
    switch (f.action) { case 0: a = ins::ACTION_DOWN; break; case 1: a = ins::ACTION_UP; break; case 5: a = ins::ACTION_POINTER_DOWN; break;
                        case 6: a = ins::ACTION_POINTER_UP; break; default: break; }
    t->set_action(a);
    enqueue(CH_INPUT, true, 0x8001, r.SerializeAsString());
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
void HuSession::enqueue(uint8_t ch, bool enc, uint16_t id, const std::string& body, uint8_t extra) {
    if (id != 0x0b && id != 0x0c && id != 0x8004)
        if (trace::ok()) LOG("t=" << msNow() << "ms TX ch=" << (int)ch << " id=0x" << std::hex << id << std::dec << " len=" << body.size() + 2 << " " << hexs((const uint8_t*)body.data(), body.size()));
    std::vector<uint8_t> v; v.reserve(2 + body.size());
    wr16(v, id); v.insert(v.end(), body.begin(), body.end());
    enqueueRaw(ch, enc, std::move(v), extra);
}

void HuSession::enqueueRaw(uint8_t ch, bool enc, std::vector<uint8_t> plain, uint8_t extra) {
    std::lock_guard<std::mutex> enc_lk(enc_m_);   // encrypt + queue as one step (see enc_m_)
    constexpr size_t kChunk = 8 * 1024;
    std::vector<OutFrame> frames;
    size_t total = plain.size(), off = 0;
    bool single = total <= kChunk;
    while (off < total || frames.empty()) {
        size_t chunk = std::min(kChunk, total - off);
        uint8_t ft = single ? FT_BULK : (off == 0 ? FT_FIRST : (off + chunk >= total ? FT_LAST : FT_MIDDLE));
        std::vector<uint8_t> payload;
        if (enc) { if (!sslEncrypt(plain.data() + off, chunk, payload)) { ERR("encrypt failed"); return; } }
        else payload.assign(plain.begin() + off, plain.begin() + off + chunk);
        OutFrame f;
        f.bytes.push_back(ch);
        f.bytes.push_back((enc ? FLAG_ENCRYPTED : 0) | ft | extra);
        wr16(f.bytes, (uint16_t)payload.size());
        if (ft == FT_FIRST) wr32(f.bytes, (uint32_t)total);
        f.bytes.insert(f.bytes.end(), payload.begin(), payload.end());
        frames.push_back(std::move(f));
        off += chunk;
        if (total == 0) break;
    }
    { std::lock_guard<std::mutex> lk(tx_m_); for (auto& f : frames) tx_.push_back(std::move(f)); }
    tx_cv_.notify_one();
}

void HuSession::writerLoop() {
    while (running_) {
        OutFrame f;
        {
            std::unique_lock<std::mutex> lk(tx_m_);
            tx_cv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return !tx_.empty() || !running_; });
            if (tx_.empty()) { if (!running_) break; continue; }
            f = std::move(tx_.front()); tx_.pop_front();
        }
        const uint8_t* p = f.bytes.data(); size_t n = f.bytes.size();
        while (n > 0) {
            ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EINTR) continue; ERR("send: " << strerror(errno)); endSession("write failed"); return; }
            p += w; n -= (size_t)w;
        }
    }
}

// ---------------------------------------------------------------------------
// TLS: we are the CLIENT (like a real head unit); phone is the server.
// ---------------------------------------------------------------------------
bool HuSession::initSsl() {
    if (ctx_) return true;
    SSL_load_error_strings(); OpenSSL_add_ssl_algorithms();
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) return false;
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_TLSv1_3);
    SSL_CTX_set_security_level(ctx_, 0);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, verify_accept_all);
    if (SSL_CTX_use_certificate_file(ctx_, cfg_.hu_cert.c_str(), SSL_FILETYPE_PEM) <= 0) { ERR("cannot load HU certificate " << cfg_.hu_cert); ERR_print_errors_fp(stderr); return false; }
    if (SSL_CTX_use_PrivateKey_file(ctx_, cfg_.hu_key.c_str(), SSL_FILETYPE_PEM) <= 0) { ERR("cannot load HU key " << cfg_.hu_key); ERR_print_errors_fp(stderr); return false; }
    if (!SSL_CTX_check_private_key(ctx_)) { ERR("HU cert/key mismatch"); return false; }
    ssl_ = SSL_new(ctx_);
    rbio_ = BIO_new(BIO_s_mem()); wbio_ = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl_, rbio_, wbio_);
    SSL_set_connect_state(ssl_);
    return true;
}

void HuSession::drainWriteBio(std::vector<uint8_t>& out) {
    uint8_t buf[4096]; int len;
    while ((len = BIO_read(wbio_, buf, sizeof buf)) > 0) out.insert(out.end(), buf, buf + len);
}

void HuSession::startTls() {
    std::vector<uint8_t> hello;
    { std::lock_guard<std::mutex> lk(ssl_m_); SSL_do_handshake(ssl_); drainWriteBio(hello); }
    LOG("TLS: sending ClientHello (" << hello.size() << " bytes)");
    std::vector<uint8_t> v; wr16(v, ControlMessageType::MESSAGE_ENCAPSULATED_SSL); v.insert(v.end(), hello.begin(), hello.end());
    enqueueRaw(0, false, std::move(v));
}

void HuSession::onSslHandshake(const uint8_t* p, size_t n) {
    std::vector<uint8_t> reply; bool done = false;
    {
        std::lock_guard<std::mutex> lk(ssl_m_);
        BIO_write(rbio_, p, (int)n);
        int r = SSL_do_handshake(ssl_);
        if (r <= 0) {
            int e = SSL_get_error(ssl_, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR("TLS handshake failed: " << ERR_error_string(ERR_get_error(), nullptr)); endSession("TLS handshake failed"); return; }
        }
        drainWriteBio(reply);
        if (SSL_is_init_finished(ssl_) && !tls_done_) { tls_done_ = true; done = true; }
    }
    if (!reply.empty()) {
        std::vector<uint8_t> v; wr16(v, ControlMessageType::MESSAGE_ENCAPSULATED_SSL); v.insert(v.end(), reply.begin(), reply.end());
        enqueueRaw(0, false, std::move(v));
    }
    if (done) {   // ONLY now (finished handshake) is AUTH_COMPLETE sent
        LOG("TLS handshake complete: " << SSL_get_version(ssl_) << " " << SSL_get_cipher(ssl_) << " -> AUTH_COMPLETE");
        ctl::AuthResponse a; a.set_status(getenv("AA_TEST_AUTH_REJECT") ? atoi(getenv("AA_TEST_AUTH_REJECT")) : 0);   // test hook: a head unit that rejects the certificate (-24 = expired on Sync 4)
        enqueue(0, false, ControlMessageType::MESSAGE_AUTH_COMPLETE, a.SerializeAsString());
    }
}

std::vector<uint8_t> HuSession::sslDecrypt(const std::vector<uint8_t>& ct) {
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
        ERR("SSL_read error " << e); break;
    }
    return out;
}

bool HuSession::sslEncrypt(const uint8_t* p, size_t n, std::vector<uint8_t>& ct) {
    std::lock_guard<std::mutex> lk(ssl_m_);
    int r = SSL_write(ssl_, p, (int)n);
    if (r <= 0) { ERR("SSL_write error " << SSL_get_error(ssl_, r)); return false; }
    drainWriteBio(ct);
    return true;
}
