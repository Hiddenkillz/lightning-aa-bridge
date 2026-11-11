// Offline end-to-end test of the AUDIO / MICROPHONE / CONTROL relay through four REAL sessions (same harness as nav_e2e_test):
//   fake vehicle (HuSession as Sync 4) <-A-> Pi MdSession == AvRelay == Pi HuSession <-B-> fake phone (MdSession)
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include "../src/AvRelay.h"
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
int main(int argc, char** argv) {
    Config cfg; cfg.relay_audio = true; cfg.relay_control = true; cfg.relay_nav = false;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2], B[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A) || socketpair(AF_UNIX, SOCK_STREAM, 0, B)) { perror("socketpair"); return 2; }
    HuSession truck(cfg, A[1]); MdSession piTruck(cfg, A[0], A[0]); HuSession piPhone(cfg, B[1]); MdSession phone(cfg, B[0], B[0]);

    std::mutex m; std::string media, guide, micAtPhone; std::vector<std::pair<uint16_t, std::string>> truckCtl, phoneCtl; std::atomic<int> tStart[2] = {0, 0}, tStop[2] = {0, 0}; std::atomic<int> micOpenAtTruck{-1};
    truck.onAudioStart = [&](int s, int) { ++tStart[s]; };
    truck.onAudioStop = [&](int s) { ++tStop[s]; };
    truck.onAudioData = [&](int s, uint64_t, const uint8_t* p, size_t n) { std::lock_guard<std::mutex> l(m); (s ? guide : media).append((const char*)p, n); };
    truck.onMicRequest = [&](bool o) { micOpenAtTruck = o ? 1 : 0; };
    truck.onControlRaw = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); truckCtl.push_back({id, b}); };
    phone.onMicData = [&](uint64_t, const uint8_t* p, size_t n) { std::lock_guard<std::mutex> l(m); micAtPhone.append((const char*)p, n); };
    phone.onControlRaw = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); phoneCtl.push_back({id, b}); };

    AvRelay relay;   // same wiring as Device.cpp / HeadUnit.cpp
    piTruck.onMicData = [&](uint64_t ts, const uint8_t* p, size_t n) { relay.micDataFromTruck(ts, p, n); };
    piTruck.onControlRaw = [&](uint16_t id, const std::string& b) { relay.controlFromTruck(id, b); };
    relay.setTruck([&](int s, int se) { piTruck.startAudio(s, se); }, [&](int s) { piTruck.stopAudio(s); }, [&](int s, uint64_t ts, const uint8_t* p, size_t n) { piTruck.sendAudio(s, ts, p, n); },
                   [&](bool o) { piTruck.micRequest(o); }, [&](uint16_t id, const std::string& b) { piTruck.sendControl(id, b); });
    piPhone.onAudioStart = [&](int s, int se) { relay.phoneAudioStart(s, se); }; piPhone.onAudioStop = [&](int s) { relay.phoneAudioStop(s); };
    piPhone.onAudioData = [&](int s, uint64_t ts, const uint8_t* p, size_t n) { relay.phoneAudioData(s, ts, p, n); };
    piPhone.onMicRequest = [&](bool o) { relay.phoneMicRequest(o); };
    piPhone.onControlRaw = [&](uint16_t id, const std::string& b) { relay.controlFromPhone(id, b); };
    relay.setPhone([&](uint64_t ts, const uint8_t* p, size_t n) { piPhone.sendMic(ts, p, n); }, [&](uint16_t id, const std::string& b) { piPhone.sendControl(id, b); });

    // ui38a: the phone's stream can start BEFORE the vehicle's audio channels are discovered (key cycle: phone faster than Sync). The wish must survive.
    relay.controlFromPhone(18, std::string("\x08\x01", 2));      // the phone's AudioFocusRequest GAIN, before the vehicle exists (start-up USB flap case)
    relay.phoneAudioStart(0, 7);
    std::thread t1([&] { truck.run(); }), t2([&] { piTruck.run(); }), t3([&] { piPhone.run(); }), t4([&] { phone.run(); });
    CHECK(waitFor([&] { return piTruck.audioReady(0) && piTruck.audioReady(1); }), "Pi opened and set up the vehicle's MEDIA and GUIDANCE sinks (SETUP -> CONFIG ready)");
    CHECK(waitFor([&] { return tStart[0] == 1; }, 4000), "a phone MEDIA start issued before the vehicle channel existed is replayed once the channel is ready");
    { std::lock_guard<std::mutex> l(m); bool focus = false; for (auto& kv : truckCtl) if (kv.first == 18 && kv.second == std::string("\x08\x01", 2)) focus = true;
      CHECK(focus, "the phone's audio focus request issued before the vehicle existed reaches the vehicle (held until its MEDIA channel was ready)"); }
    relay.phoneAudioStop(0); CHECK(waitFor([&] { return tStop[0] == 1; }, 4000), "and stopped again"); tStart[0] = 0; tStop[0] = 0;
    CHECK(waitFor([&] { return phone.audioReady(0) && phone.audioReady(1); }), "the phone side got its MEDIA and GUIDANCE sinks ready from the Pi");

    phone.startAudio(0, 11); phone.startAudio(1, 12);
    CHECK(waitFor([&] { return tStart[0] == 1 && tStart[1] == 1; }), "phone START on both streams reaches the vehicle");
    std::string tone0, tone1;
    for (int i = 0; i < 40; ++i) {   // 40 chunks per stream, paced like a phone (the vehicle's window is 4: the test must also prove nothing is lost while acks flow)
        std::string a(1920, 0), g(960, 0); for (size_t k = 0; k < a.size(); ++k) a[k] = (char)(i * 3 + k); for (size_t k = 0; k < g.size(); ++k) g[k] = (char)(i * 5 + k * 2);
        tone0 += a; tone1 += g; phone.sendAudio(0, 1000 * i, (const uint8_t*)a.data(), a.size()); phone.sendAudio(1, 1000 * i, (const uint8_t*)g.data(), g.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return media.size() >= tone0.size() && guide.size() >= tone1.size(); }), "all PCM chunks arrive at the vehicle");
    { std::lock_guard<std::mutex> l(m);
      CHECK(media == tone0, "MEDIA PCM is byte-exact and in order (1920-byte chunks, 40 per stream)");
      CHECK(guide == tone1, "GUIDANCE PCM is byte-exact and in order"); }
    phone.stopAudio(0); phone.stopAudio(1);
    CHECK(waitFor([&] { return tStop[0] == 1 && tStop[1] == 1; }), "phone STOP reaches the vehicle");

    phone.micRequest(true);
    CHECK(waitFor([&] { return micOpenAtTruck == 1; }), "the phone's microphone OPEN request reaches the vehicle");
    std::string mic; for (int i = 0; i < 20; ++i) { std::string c(640, 0); for (size_t k = 0; k < c.size(); ++k) c[k] = (char)(i + k * 11); mic += c; truck.sendMic(500 * i, (const uint8_t*)c.data(), c.size()); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return micAtPhone.size() >= mic.size(); }), "vehicle microphone PCM reaches the phone");
    { std::lock_guard<std::mutex> l(m); CHECK(micAtPhone == mic, "...byte-exact and in order"); }

    phone.sendControl(23, std::string("\x08\x50", 2)); phone.sendControl(24, std::string("\x08\x01", 2));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); int b = 0, c = 0; for (auto& x : truckCtl) { if (x.first == 23) ++b; if (x.first == 24) ++c; } return b == 1 && c == 1; }), "battery status and call availability from the phone reach the vehicle");
    truck.sendControl(17, std::string("\x08\x01", 2));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); for (auto& x : phoneCtl) if (x.first == 17) return true; return false; }), "the vehicle's voice-session button reaches the phone");

    truck.stop(); piTruck.stop(); piPhone.stop(); phone.stop(); t1.join(); t2.join(); t3.join(); t4.join();
    printf(fails ? "AV E2E FAILED\n" : "AV E2E ALL PASSED\n"); return fails ? 1 : 0;
}
