// Call audio over Android Auto: DSP (8k->48k, mic 16k->8k, gain, clip, ducking) and the SCO loop on a SEQPACKET socketpair.
#include "../src/btcall/AaCall.h"
#include "../src/AvRelay.h"
#include <sys/socket.h>
#include <cstdio>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
using btcall::AaCall;

int main() {
    // ---- downlink conversion
    { AaCall a; a.gain_pct = 100; std::vector<int16_t> in(160, 1200), out; a.down8kTo48k(in.data(), in.size(), out);
      CHECK(out.size() == 960, "160 samples at 8 kHz become 960 at 48 kHz (6x)");
      CHECK(out.front() == 200 && out.back() == 1200, "linear ramp from the previous sample (0 -> 1200 in six steps)");
      out.clear(); a.down8kTo48k(in.data(), in.size(), out); CHECK(out[0] == 1200 && out[959] == 1200, "constant input stays constant after the first sample (state is kept between packets)"); }
    { AaCall a; a.gain_pct = 300; std::vector<int16_t> in(8, 1000), out; a.down8kTo48k(in.data(), in.size(), out); a.down8kTo48k(in.data(), in.size(), out);
      CHECK(out.back() == 3000, "300% gain: 1000 -> 3000"); }
    { AaCall a; a.gain_pct = 400; std::vector<int16_t> in(8, 20000), out; a.down8kTo48k(in.data(), in.size(), out);
      CHECK(*std::max_element(out.begin(), out.end()) <= 32767 && *std::max_element(out.begin(), out.end()) > 32000, "a very loud signal is limited just under +32767, never wraps around"); }
    { AaCall a; std::vector<int16_t> in(8, 20000), out; a.down8kTo48k(in.data(), in.size(), out); a.down8kTo48k(in.data(), in.size(), out);
      CHECK(out.back() == 20000, "default gain is 100%: a signal at 61% of full scale passes through untouched (no clipping)"); }
    { AaCall a; a.gain_pct = 300; std::vector<int16_t> in(8, 30000), out; a.down8kTo48k(in.data(), in.size(), out); a.down8kTo48k(in.data(), in.size(), out);
      const int v = out.back(); CHECK(v > 24000 && v < 32767, "soft limiter: 300% on a loud signal lands between the knee and full scale (smooth, not flat-topped)"); }
    // ---- microphone conversion
    { AaCall a; a.duck = false; a.agc = false; a.prime_samples = 0; std::vector<int16_t> m16(320, 2000); a.pushMic16k((const uint8_t*)m16.data(), m16.size() * 2, 1000);
      int16_t o[160]; CHECK(a.popUplink(o, 160), "320 mic samples at 16 kHz fill 160 at 8 kHz"); CHECK(o[0] == 2000 && o[159] == 2000, "mic level preserved at 100% gain");
      int16_t z[10]; CHECK(!a.popUplink(z, 10) && z[0] == 0 && z[9] == 0, "empty ring gives silence, not stale data"); }
    { AaCall a; a.agc = false; a.prime_samples = 0; a.duck = true; a.duck_pct = 20; a.duck_hold_ms = 300; a.markFarEnd(1000);
      std::vector<int16_t> m16(32, 5000); a.pushMic16k((const uint8_t*)m16.data(), 64, 1100); int16_t o[16]; a.popUplink(o, 16);
      CHECK(o[0] == 1000, "far end talking: mic ducked to 20%");
      a.pushMic16k((const uint8_t*)m16.data(), 64, 1500); a.popUplink(o, 16); CHECK(o[0] == 5000, "after the hold time the mic is back at full level"); }
    { AaCall a; a.duck = false; a.agc = false; a.prime_samples = 0; std::vector<int16_t> big(8000, 1); a.pushMic16k((const uint8_t*)big.data(), big.size() * 2, 0);
      int16_t o[1600]; CHECK(a.popUplink(o, 1600) && !a.popUplink(o, 1), "the uplink buffer is bounded (about 200 ms): old audio is dropped, latency cannot grow"); }

    // ---- microphone: automatic gain, speech-level ducking, prefill buffer
    { AaCall a; a.duck = false; a.prime_samples = 0; std::vector<int16_t> quiet(320, 400);                       // the real truck mic: peaks of a few hundred
      for (int i = 0; i < 60; ++i) a.pushMic16k((const uint8_t*)quiet.data(), quiet.size() * 2, 0);
      int16_t o[160]; int16_t last = 0; while (a.popUplink(o, 160)) last = o[159];
      CHECK(last > 4000 && last <= 7000, "the AGC brings a very quiet microphone (peak 400) up to a usable level (~7000)"); }
    { AaCall a; a.duck = false; a.prime_samples = 0; std::vector<int16_t> noise(320, 30);
      for (int i = 0; i < 20; ++i) a.pushMic16k((const uint8_t*)noise.data(), noise.size() * 2, 0);
      int16_t o[160]; a.popUplink(o, 160); CHECK(o[0] == 30, "the AGC leaves the noise floor alone (no hiss amplified)"); }
    { AaCall a; a.agc = false; a.prime_samples = 0; a.duck = true; a.peak_thr = 2500;
      std::vector<int16_t> m(32, 5000); a.markFarEnd(1000);                                                    // (markFarEnd is what the loop calls above the threshold)
      a.pushMic16k((const uint8_t*)m.data(), 64, 1100); int16_t o[16]; a.popUplink(o, 16); CHECK(o[0] == 2000, "ducking lowers the mic to 40% while the caller talks"); }
    { AaCall a; a.agc = false; a.duck = false; a.prime_samples = 320;
      std::vector<int16_t> m(100, 1000); a.pushMic16k((const uint8_t*)m.data(), 200, 0); int16_t o[30];
      CHECK(!a.popUplink(o, 30) && o[0] == 0, "uplink waits for a 40 ms cushion before sending (silence until then)");
      std::vector<int16_t> more(800, 1000); a.pushMic16k((const uint8_t*)more.data(), 1600, 0);
      CHECK(a.popUplink(o, 30) && o[0] == 1000, "once the cushion is there, audio flows");
      while (a.popUplink(o, 30)) {} CHECK(!a.popUplink(o, 30), "after running dry it refills before sending again (no stuttering)"); }
    // ---- microphone sharing in the audio hub: a call and the phone's own mic request (voice assistant) can both want the vehicle microphone
    { AvRelay r; std::vector<bool> opens; long toPhone = 0, toCall = 0;
      r.setTruck(nullptr, nullptr, nullptr, [&](bool o) { opens.push_back(o); }, nullptr);
      r.setPhone([&](uint64_t, const uint8_t*, size_t n) { toPhone += (long)n; }, nullptr);
      uint8_t d[10] = {0};
      r.phoneMicRequest(true); r.micDataFromTruck(1, d, 10);
      CHECK(opens.size() == 1 && opens[0] && toPhone == 10, "phone opens the mic: vehicle mic opens, audio goes to the phone");
      r.setCallMic(true, [&](uint64_t, const uint8_t*, size_t n) { toCall += (long)n; });
      r.micDataFromTruck(2, d, 10);
      CHECK(toCall == 10 && toPhone == 10, "during a call the microphone audio goes to the call, not to the phone");
      r.phoneMicRequest(false);
      CHECK(opens.back() == true, "the phone closing its mic does not close the vehicle mic while the call needs it");
      r.setCallMic(false, nullptr);
      CHECK(opens.back() == false, "call over and phone mic closed: the vehicle mic closes");
      r.setCallMic(true, [&](uint64_t, const uint8_t*, size_t n) { toCall += (long)n; }); r.setCallMic(false, nullptr);
      r.micDataFromTruck(3, d, 10); CHECK(toCall == 10, "after the call no audio is delivered to the call tap"); }

    // ---- the loop on a socketpair
    int sp[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp)) { perror("socketpair"); return 2; }
    AaCall a; a.gain_pct = 100; a.duck = false; a.agc = false; a.prime_samples = 0;
    std::atomic<int> started{0}, stopped{0}; std::atomic<bool> micOn{false}, micSeen{false}; std::atomic<long> downBytes{0}; std::mutex lm; std::string lastLog;
    a.hooks.start = [&] { ++started; }; a.hooks.stop = [&] { ++stopped; };
    a.hooks.down = [&](const uint8_t*, size_t n) { downBytes += (long)n; };
    a.hooks.mic = [&](bool o) { micOn = o; if (o) micSeen = true; };
    a.hooks.log = [&](const std::string& s) { std::lock_guard<std::mutex> l(lm); lastLog = s; };
    std::atomic<bool> stop{false}; const char* why = "";
    std::thread t([&] { why = a.run(sp[0], stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(started == 1 && micOn, "call start opens the vehicle stream and asks for the microphone");
    std::vector<int16_t> mic16(3200, 3000); a.pushMic16k((const uint8_t*)mic16.data(), mic16.size() * 2, AaCall::nowMs());      // 200 ms of mic at 16 kHz
    int back = 0, backMic = 0;
    for (int i = 0; i < 100; ++i) {
        int16_t pkt[30]; for (int k = 0; k < 30; ++k) pkt[k] = (int16_t)(8000 * std::sin((i * 30 + k) * 0.3));        // 60-byte SCO packets
        if (::send(sp[1], pkt, sizeof pkt, 0) != (ssize_t)sizeof pkt) break;
        pollfd p{sp[1], POLLIN, 0};
        if (::poll(&p, 1, 500) > 0) { int16_t rb[64]; ssize_t n = ::recv(sp[1], rb, sizeof rb, 0); if (n == 60) { ++back; if (rb[0] == 3000 && rb[29] == 3000) ++backMic; } }
    }
    CHECK(back == 100, "every SCO packet in is answered by one packet of the same size");
    CHECK(backMic >= 50 && backMic <= 55, "the vehicle microphone audio arrives in those packets (200 ms = 1600 samples = 53 packets of 30), then silence");
    CHECK(downBytes.load() >= 100 * 30 * 6 * 2 - 3840 - 1, "downlink audio reaches the vehicle hook in full (minus at most one unfinished 20 ms chunk)");
    ::close(sp[1]); t.join();
    CHECK(std::string(why) == "closed by the phone", "the loop ends when the phone closes the call audio");
    CHECK(stopped == 1 && !micOn, "call end stops the vehicle stream and releases the microphone");
    { std::lock_guard<std::mutex> l(lm); CHECK(lastLog.find("call audio ended") == 0, "an end-of-call summary is logged"); }
    ::close(sp[0]);
    printf(fails ? "AACALL FAILED\n" : "AACALL ALL PASSED\n"); return fails ? 1 : 0;
}
