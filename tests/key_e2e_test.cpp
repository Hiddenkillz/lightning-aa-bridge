// Steering-wheel key relay through four real sessions + the binding request the Pi sends the vehicle.
//   fake vehicle (HuSession as Sync 4) -> key -> Pi MdSession -> callback -> Pi HuSession -> fake phone (MdSession) -> onKey
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
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
    Config cfg; cfg.relay_keys = true; cfg.relay_nav = false;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2], B[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A) || socketpair(AF_UNIX, SOCK_STREAM, 0, B)) { perror("socketpair"); return 2; }
    HuSession truck(cfg, A[1]); MdSession piTruck(cfg, A[0], A[0]); HuSession piPhone(cfg, B[1]); MdSession phone(cfg, B[0], B[0]);
    std::mutex m; std::vector<std::tuple<int, bool, bool>> got;
    piTruck.setKeyHandler([&](int kc, bool down, bool lp, int) { piPhone.sendKey(kc, down, lp); });     // same wiring as main.cpp (Device -> HeadUnit)
    phone.setKeyHandler([&](int kc, bool down, bool lp, int) { std::lock_guard<std::mutex> l(m); got.emplace_back(kc, down, lp); });
    std::thread t1([&] { truck.run(); }), t2([&] { piTruck.run(); }), t3([&] { piPhone.run(); }), t4([&] { phone.run(); });
    CHECK(waitFor([&] { return piTruck.videoReady() && phone.videoReady(); }), "both sessions are up");
    // the binding request: field 1 packed varints, containing the media keys
    const std::string b = piTruck.bindingRequestBytes();
    CHECK(b.size() > 2 && (uint8_t)b[0] == 0x0a && (size_t)(uint8_t)b[1] == b.size() - 2, "binding request is a packed list (not empty)");
    bool has87 = false, has88 = false, has85 = false; for (size_t i = 2; i < b.size(); ++i) { if ((uint8_t)b[i] == 87) has87 = true; if ((uint8_t)b[i] == 88) has88 = true; if ((uint8_t)b[i] == 85) has85 = true; }
    CHECK(has87 && has88 && has85, "binding request asks for next (87), previous (88) and play/pause (85)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    truck.sendKey(87, true, false); truck.sendKey(87, false, false);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return got.size() >= 2; }), "key press reaches the phone");
    { std::lock_guard<std::mutex> l(m); CHECK(got.size() >= 2 && std::get<0>(got[0]) == 87 && std::get<1>(got[0]) && !std::get<1>(got[1]), "next-track down then up, in order"); }
    truck.sendKey(88, true, true);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return got.size() >= 3; }) && std::get<0>(got[2]) == 88 && std::get<2>(got[2]), "previous with long-press flag arrives intact");
    truck.stop(); piTruck.stop(); piPhone.stop(); phone.stop(); t1.join(); t2.join(); t3.join(); t4.join();
    printf(fails ? "KEY E2E FAILED\n" : "KEY E2E ALL PASSED\n"); return fails ? 1 : 0;
}
