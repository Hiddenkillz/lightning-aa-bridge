// AvRelay: audio + control-channel hub between the two AA sessions (phone <-> vehicle).
//   audio  : phone -> vehicle PCM for MEDIA and GUIDANCE (start/stop/data), vehicle microphone -> phone (open request / PCM)
//   control: raw control-channel messages that are not ours to answer: phone->vehicle {18 AudioFocusRequest, 23 BatteryStatus, 24 CallAvailability},
//            vehicle->phone {17 VoiceSessionNotification, 19 AudioFocusNotification}
// Nothing here parses audio: PCM is carried as it arrives (both sides are configured to the same format; the vehicle's own advertised format decides it).
// Senders are always called OUTSIDE the lock. Counters are logged by the owner.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>

class AvRelay {
public:
    enum Stream { MEDIA = 0, GUIDANCE = 1, kStreams = 2 };
    using Raw = std::function<void(uint16_t id, const std::string& body)>;
    using Start = std::function<void(int stream, int session)>;
    using Stop = std::function<void(int stream)>;
    using Data = std::function<void(int stream, uint64_t ts_us, const uint8_t* pcm, size_t n)>;
    using MicOpen = std::function<void(bool open)>;
    using MicData = std::function<void(uint64_t ts_us, const uint8_t* pcm, size_t n)>;

    static bool phoneToTruckControl(uint16_t id) { return id == 18 || id == 23 || id == 24; }
    static bool truckToPhoneControl(uint16_t id) { return id == 17 || id == 19; }

    // ---- vehicle side registrations (set when the vehicle session is up, {} when it is gone)
    // A new vehicle session (e.g. after a USB re-attach) is told what the phone is already doing: active audio streams and an open microphone are replayed to it.
    void setTruck(Start s, Stop e, Data d, MicOpen m, Raw ctl) {
        bool act[kStreams]; int sess[kStreams]; bool mic; Start fs; MicOpen fm;
        { std::lock_guard<std::mutex> l(m_); t_start_ = std::move(s); t_stop_ = std::move(e); t_data_ = std::move(d); t_mic_ = std::move(m); t_ctl_ = std::move(ctl);
          for (int i = 0; i < kStreams; ++i) { act[i] = active_[i]; sess[i] = session_[i]; } mic = mic_active_; fs = t_start_; fm = t_mic_; }
        for (int i = 0; i < kStreams; ++i) if (act[i] && fs) { log("replaying active audio stream " + std::to_string(i) + " to the new vehicle session"); fs(i, sess[i]); }
        if ((mic || call_mic_) && fm) { log("replaying open microphone to the new vehicle session"); fm(true); }
        // The phone's audio focus request is state too: without it the vehicle never grants focus to the relayed stream (ui51a: Sync acked every
        // audio chunk of a whole drive and played none, and showed no now-playing, because the request had gone to a vehicle session that died).
        Raw fc; std::string focus; { std::lock_guard<std::mutex> l(m_); fc = t_ctl_; focus = focus_req_; }
        if (fc && !focus.empty()) { log("replaying the phone's audio focus request (" + focusName(focus) + ") to the new vehicle session"); fc(18, focus); } }
    void clearTruck() { std::lock_guard<std::mutex> l(m_); t_start_ = nullptr; t_stop_ = nullptr; t_data_ = nullptr; t_mic_ = nullptr; t_ctl_ = nullptr; }
    // ---- phone side registrations
    void setPhone(MicData mic, Raw ctl) { std::lock_guard<std::mutex> l(m_); p_mic_ = std::move(mic); p_ctl_ = std::move(ctl); }
    void clearPhone() { std::lock_guard<std::mutex> l(m_); p_mic_ = nullptr; p_ctl_ = nullptr; }

    // ---- phone -> vehicle
    void phoneAudioStart(int s, int session) { Start f; { std::lock_guard<std::mutex> l(m_); f = t_start_; if (s >= 0 && s < kStreams) { active_[s] = true; session_[s] = session; } } log("phone audio START stream " + std::to_string(s) + (f ? "" : " (no vehicle session: dropped)")); if (f) f(s, session); }
    void phoneAudioStop(int s) { Stop f; { std::lock_guard<std::mutex> l(m_); f = t_stop_; if (s >= 0 && s < kStreams) active_[s] = false; } log("phone audio STOP stream " + std::to_string(s)); if (f) f(s); }
    void phoneAudioData(int s, uint64_t ts, const uint8_t* p, size_t n) {
        Data f; { std::lock_guard<std::mutex> l(m_); f = t_data_; if (s >= 0 && s < kStreams) { bytes_in_[s] += n; if (!f) ++no_truck_[s]; } }
        if (f) f(s, ts, p, n); }
    void phoneMicRequest(bool open) { MicOpen f; bool want; { std::lock_guard<std::mutex> l(m_); f = t_mic_; mic_active_ = open; want = open || call_mic_; } log(std::string("phone microphone ") + (open ? "OPEN" : "CLOSE") + (f ? "" : " (no vehicle session: dropped)")); if (f) f(want); }
    // a phone call carried over this link (bt_call_aa): the call bridge needs the vehicle microphone too; while it does, the microphone goes to the call, not to the phone
    void setCallMic(bool open, MicData tap) { MicOpen f; bool want; { std::lock_guard<std::mutex> l(m_); call_mic_ = open; call_tap_ = open ? std::move(tap) : MicData(); f = t_mic_; want = open || mic_active_; } log(std::string("call microphone ") + (open ? "OPEN" : "CLOSE") + (f ? "" : " (no vehicle session: dropped)")); if (f) f(want); }
    // AudioFocusRequest body: field 1 varint = audio_focus_type (1 GAIN, 2 GAIN_TRANSIENT, 3 GAIN_TRANSIENT_MAY_DUCK, 4 RELEASE)
    static int focusType(const std::string& b) { if (b.size() >= 2 && (uint8_t)b[0] == 0x08) return (uint8_t)b[1] & 0x7f; return -1; }
    static std::string focusName(const std::string& b) { const int t = focusType(b); return t == 1 ? "GAIN" : t == 2 ? "GAIN_TRANSIENT" : t == 3 ? "GAIN_TRANSIENT_MAY_DUCK" : t == 4 ? "RELEASE" : "type " + std::to_string(t); }
    std::string focusRequest() const { std::lock_guard<std::mutex> l(m_); return focus_req_; }
    void controlFromPhone(uint16_t id, const std::string& b) {
        Raw f; { std::lock_guard<std::mutex> l(m_); f = t_ctl_; ++ctl_to_truck_;
          if (id == 18) focus_req_ = focusType(b) == 4 ? std::string() : b; }          // remembered until released: replayed to a new vehicle session
        if (!phoneToTruckControl(id)) return;
        log("control phone->vehicle id=" + std::to_string(id) + " len=" + std::to_string(b.size()) + (f ? "" : " (no vehicle session: dropped)")); if (f) f(id, b); }
    // ---- vehicle -> phone
    void micDataFromTruck(uint64_t ts, const uint8_t* p, size_t n) { MicData f, tap; { std::lock_guard<std::mutex> l(m_); f = p_mic_; tap = call_tap_; mic_bytes_ += n; } if (tap) tap(ts, p, n); else if (f) f(ts, p, n); }
    void controlFromTruck(uint16_t id, const std::string& b) {
        Raw f; { std::lock_guard<std::mutex> l(m_); f = p_ctl_; ++ctl_to_phone_; } if (!truckToPhoneControl(id)) return;
        log("control vehicle->phone id=" + std::to_string(id) + " len=" + std::to_string(b.size()) + (f ? "" : " (no phone session: dropped)")); if (f) f(id, b); }

    long bytesIn(int s) const { std::lock_guard<std::mutex> l(m_); return bytes_in_[s]; }
    long micBytes() const { std::lock_guard<std::mutex> l(m_); return mic_bytes_; }
private:
    void log(const std::string& s) { std::lock_guard<std::mutex> l(lm_); if (++logged_ <= 600) std::cout << "[Av] " << s << std::endl; }
    mutable std::mutex m_, lm_;
    std::string focus_req_;                      // the phone's current audio focus request (empty = none / released)
    Start t_start_; Stop t_stop_; Data t_data_; MicOpen t_mic_; Raw t_ctl_; MicData p_mic_; Raw p_ctl_; MicData call_tap_; bool call_mic_ = false;
    bool active_[kStreams] = {false, false}; int session_[kStreams] = {0, 0}; bool mic_active_ = false;
    long bytes_in_[kStreams] = {0, 0}, no_truck_[kStreams] = {0, 0}, mic_bytes_ = 0, ctl_to_truck_ = 0, ctl_to_phone_ = 0, logged_ = 0;
};
