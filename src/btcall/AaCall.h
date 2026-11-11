#pragma once
// Phone call audio over Android Auto (config bt_call_aa): the Pi is the phone's hands-free unit, takes the call's SCO audio and plays it into a sound channel the
// vehicle already has (GUIDANCE), and sends the vehicle's microphone back into the call. No Bluetooth link to the vehicle is involved.
//
//   phone --SCO 8 kHz mono s16le--> [down8kTo48k] --48 kHz mono s16le--> vehicle GUIDANCE sink (hooks.down)
//   vehicle mic (16 kHz mono s16le) --> [pushMic16k] --8 kHz--> ring --> SCO packets back to the phone (one per packet received)
//
// Design follows the public description of the same idea in aa-proxy-rs (SCO bridged into an existing AA PCM sink + the HU microphone); the code is our own.
// Pure DSP + a small blocking loop on any datagram fd (a SOCK_SEQPACKET socketpair in tests), so it is fully unit-testable.
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace btcall {

class AaCall {
public:
    struct Hooks {
        std::function<void()> start;                                        // call audio begins (open the vehicle sink stream)
        std::function<void()> stop;                                         // call audio ended
        std::function<void(const uint8_t* pcm, size_t n)> down;             // 48 kHz mono s16le toward the vehicle
        std::function<void(bool open)> mic;                                 // the vehicle microphone is wanted (true) / no longer (false)
        std::function<void(const std::string&)> log;
    };
    Hooks hooks;
    int gain_pct = 100;            // downlink gain. The Pixel's call audio already peaks near 60% of full scale (log): a boost only clips it. A soft limiter catches the rest.
    int mic_gain_pct = 100;        // uplink gain on top of the automatic gain
    bool agc = true;               // automatic mic gain: the vehicle microphone arrives ~35 dB below full scale
    int agc_target = 7000, agc_max = 16;
    bool duck = true;              // lower the mic while the far end is talking (the vehicle microphone also hears its own speakers)
    int duck_pct = 40, duck_hold_ms = 250, peak_thr = 2500;   // far end counts as 'talking' only above a speech-level peak, not at the noise floor
    size_t prime_samples = 320;    // uplink buffer fill (8 kHz samples, 40 ms) before sending starts, and again after every underrun: smooths bursty microphone delivery
    size_t chunk_ms = 20;          // downlink chunk toward the vehicle

    // ---- DSP ------------------------------------------------------------------------------------------------------------------
    // 8 kHz -> 48 kHz mono by linear interpolation (6x), with gain and a hard clip. Keeps the previous sample between calls. Returns the peak |sample| of the INPUT.
    int down8kTo48k(const int16_t* in, size_t n, std::vector<int16_t>& out) {
        int peak = 0;
        for (size_t i = 0; i < n; ++i) {
            const int cur = in[i]; peak = std::max(peak, std::abs(cur));
            for (int k = 1; k <= 6; ++k) {
                const int v = prev_ + (cur - prev_) * k / 6;
                out.push_back(softLimit((long)v * gain_pct / 100));
            }
            prev_ = cur;
        }
        return peak;
    }
    // below 24000 untouched; above it the level is compressed smoothly towards full scale, so a loud caller is never hard-clipped
    static int16_t softLimit(long x) {
        const long T = 24000; const long ax = x < 0 ? -x : x;
        if (ax <= T) return (int16_t)x;
        const double room = 32767.0 - T; const double y = T + room * std::tanh((double)(ax - T) / room);
        return (int16_t)(x < 0 ? -y : y);
    }
    // vehicle microphone: 16 kHz mono s16le bytes -> 8 kHz samples (average of pairs), gain, ducking, into the uplink ring (bounded to ~200 ms)
    void pushMic16k(const uint8_t* p, size_t nbytes, int64_t now_ms) {
        const size_t ns = nbytes / 2; std::vector<int> raw; raw.reserve(ns / 2 + 1); int rpeak = 0;
        for (size_t i = 0; i + 1 < ns; i += 2) {
            int16_t a, b; std::memcpy(&a, p + i * 2, 2); std::memcpy(&b, p + (i + 1) * 2, 2);
            const int m = ((int)a + (int)b) / 2; rpeak = std::max(rpeak, std::abs(m)); raw.push_back(m);
        }
        std::lock_guard<std::mutex> l(mic_m_);
        if (agc) {                                                     // fast down, slow up; frames at the noise floor leave the gain alone
            if (rpeak >= 80) {
                const float want = std::min((float)agc_max, std::max(1.0f, (float)agc_target / (float)rpeak));
                if (want < agc_gain_) agc_gain_ = want; else agc_gain_ += (want - agc_gain_) * 0.08f;
            }
        } else agc_gain_ = 1.0f;
        float g = agc_gain_ * (float)mic_gain_pct / 100.0f;
        if (duck && now_ms - far_active_ms_.load() < duck_hold_ms) g *= (float)duck_pct / 100.0f;
        int peak = 0;
        for (int m : raw) { const long v = std::max<long>(-32768, std::min<long>(32767, (long)(m * g))); peak = std::max(peak, (int)std::labs(v)); ring_.push_back((int16_t)v); }
        while (ring_.size() > 1600) { ring_.pop_front(); ++mic_overrun_; }
        mic_peak_ = std::max(mic_peak_, peak); mic_samples_ += raw.size();
    }
    // `n` samples for one SCO packet: from the ring if enough, otherwise silence (never blocks)
    bool popUplink(int16_t* out, size_t n) {
        std::lock_guard<std::mutex> l(mic_m_);
        if (!primed_ && ring_.size() >= std::max(prime_samples, n)) primed_ = true;       // start (or restart) only once a little audio is buffered
        if (!primed_) { std::memset(out, 0, n * 2); return false; }
        if (ring_.size() < n) { std::memset(out, 0, n * 2); ++mic_underrun_; primed_ = false; return false; }
        for (size_t i = 0; i < n; ++i) { out[i] = ring_.front(); ring_.pop_front(); }
        return true;
    }
    void reset() { prev_ = 0; std::lock_guard<std::mutex> l(mic_m_); ring_.clear(); primed_ = false; agc_gain_ = 1.0f; mic_peak_ = 0; mic_samples_ = 0; mic_underrun_ = 0; mic_overrun_ = 0; far_active_ms_ = -100000; }
    void markFarEnd(int64_t now_ms) { far_active_ms_ = now_ms; }
    struct Stats { long pkts_in = 0, pkts_out = 0, bytes_down = 0, dropped_out = 0; int down_peak = 0, mic_peak = 0; long mic_samples = 0, mic_underrun = 0, mic_overrun = 0; };
    Stats snapshotMic(Stats s) { std::lock_guard<std::mutex> l(mic_m_); s.mic_peak = mic_peak_; s.mic_samples = (long)mic_samples_; s.mic_underrun = mic_underrun_; s.mic_overrun = mic_overrun_; return s; }

    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // ---- the audio loop ---------------------------------------------------------------------------------------------------------
    // Runs on an accepted SCO socket (or a test socketpair) until it closes or `stop` is set. Not closed here. Returns a reason.
    const char* run(int sco, std::atomic<bool>& stop, int packet_bytes_hint = 60) {
        reset();
        auto say = [&](const std::string& s) { if (hooks.log) hooks.log(s); };
        if (hooks.start) hooks.start();
        if (hooks.mic) hooks.mic(true);
        Stats st; std::vector<int16_t> chunk; chunk.reserve(2048);
        const size_t chunk_samples = 48 * chunk_ms;
        auto last_log = std::chrono::steady_clock::now(); const char* why = "stopped"; int pkt = packet_bytes_hint;
        uint8_t buf[1024];
        while (!stop) {
            pollfd p{sco, POLLIN, 0}; const int r = ::poll(&p, 1, 100);
            if (r < 0) { if (errno == EINTR) continue; why = "poll error"; break; }
            if (r > 0) {
                if (p.revents & (POLLERR | POLLNVAL)) { why = "socket error"; break; }
                if (p.revents & POLLIN) {
                    const ssize_t n = ::read(sco, buf, sizeof buf);
                    if (n == 0) { why = "closed by the phone"; break; }
                    if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; why = "read error"; break; }
                    pkt = (int)n; ++st.pkts_in;
                    const size_t ns = (size_t)n / 2; std::vector<int16_t> in(ns); std::memcpy(in.data(), buf, ns * 2);
                    const int peak = down8kTo48k(in.data(), ns, chunk); st.down_peak = std::max(st.down_peak, peak);
                    if (peak >= peak_thr) markFarEnd(nowMs());
                    if (chunk.size() >= chunk_samples) { if (hooks.down) hooks.down((const uint8_t*)chunk.data(), chunk.size() * 2); st.bytes_down += (long)chunk.size() * 2; chunk.clear(); }
                    // one packet back for every packet received (SCO is full duplex and isochronous)
                    std::vector<int16_t> up(ns); popUplink(up.data(), ns);
                    const ssize_t w = ::send(sco, up.data(), ns * 2, MSG_DONTWAIT | MSG_NOSIGNAL);
                    if (w == (ssize_t)(ns * 2)) ++st.pkts_out; else ++st.dropped_out;
                } else if (p.revents & POLLHUP) { why = "hangup"; break; }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_log > std::chrono::seconds(5)) {
                last_log = now; Stats m = snapshotMic(st);
                say("call audio: packets in " + std::to_string(m.pkts_in) + " out " + std::to_string(m.pkts_out) + " (dropped " + std::to_string(m.dropped_out) + "), packet " + std::to_string(pkt)
                    + " B, downlink peak " + std::to_string(m.down_peak) + ", mic samples " + std::to_string(m.mic_samples) + " peak " + std::to_string(m.mic_peak)
                    + " agc " + std::to_string((int)(agc_gain_ * 100)) + "% (underruns " + std::to_string(m.mic_underrun) + ", overruns " + std::to_string(m.mic_overrun) + ")");
            }
        }
        if (!chunk.empty() && hooks.down) hooks.down((const uint8_t*)chunk.data(), chunk.size() * 2);
        Stats m = snapshotMic(st);
        say(std::string("call audio ended (") + why + "): packets in " + std::to_string(m.pkts_in) + " out " + std::to_string(m.pkts_out) + " dropped " + std::to_string(m.dropped_out) + ", downlink bytes "
            + std::to_string(m.bytes_down) + " peak " + std::to_string(m.down_peak) + ", mic peak " + std::to_string(m.mic_peak) + " samples " + std::to_string(m.mic_samples));
        if (hooks.mic) hooks.mic(false);
        if (hooks.stop) hooks.stop();
        return why;
    }

private:
    int prev_ = 0; bool primed_ = false; float agc_gain_ = 1.0f;
    std::mutex mic_m_; std::deque<int16_t> ring_; int mic_peak_ = 0; size_t mic_samples_ = 0; long mic_underrun_ = 0, mic_overrun_ = 0;
    std::atomic<int64_t> far_active_ms_{-100000};
};

}  // namespace btcall
