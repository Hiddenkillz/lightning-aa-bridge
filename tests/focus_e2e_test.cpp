// focus_reclaim: the vehicle takes video focus (native popup) and never gives it back while still forwarding touches (ui34a frozen image).
//   fake vehicle (HuSession as Sync 4) -> NATIVE focus -> Pi MdSession pauses -> vehicle keeps touching -> Pi asks for focus -> vehicle answers PROJECTED -> video resumes
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <sys/socket.h>
#include <chrono>
#include <cstdio>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
int main(int argc, char** argv) {
    Config cfg; cfg.relay_nav = false;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A)) { perror("socketpair"); return 2; }
    HuSession truck(cfg, A[1]); MdSession pi(cfg, A[0], A[0]);
    std::thread t1([&] { truck.run(); }), t2([&] { pi.run(); });
    CHECK(waitFor([&] { return pi.videoReady(); }), "session up, video live");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));                      // let the start-up focus exchange (unsolicited PROJECTED + our request's reply) settle first
    std::string nat; nat.push_back(0x08); nat.push_back(0x02); nat.push_back(0x10); nat.push_back(0x01);   // VideoFocusNotification{focus=NATIVE, unsolicited}
    truck.sendRaw(1, 0x8008, nat);
    CHECK(waitFor([&] { return !pi.videoReady(); }, 6000), "vehicle takes the screen -> video paused");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    truck.sendTouch(500, 500, 0); truck.sendTouch(500, 500, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(!pi.videoReady(), "a touch right after the focus loss does not react yet (popups are normal for a second)");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    truck.sendTouch(500, 500, 0); truck.sendTouch(500, 500, 1);
    CHECK(waitFor([&] { return pi.videoReady(); }, 4000), "touches while paused -> focus requested -> vehicle grants it -> video resumes");
    truck.stop(); pi.stop(); t1.join(); t2.join();
    printf(fails ? "FOCUS E2E FAILED\n" : "FOCUS E2E ALL PASSED\n"); return fails ? 1 : 0;
}
