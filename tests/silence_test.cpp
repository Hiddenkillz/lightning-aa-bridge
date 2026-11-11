// Truck-silence watchdog (truck 29: at engine start Sync 4 sent one ping and then NOTHING for 104 s while the USB link stayed "configured";
// the session never ended, so the Pi never re-attached and the truck never came back).
//   case 1: the peer never says anything            -> session must end by itself after truck_silence_start_s
//   case 2: the peer sends one valid frame, then is silent -> session must end after truck_silence_s, and report WHY
//   case 3: the peer keeps talking (a frame every 300 ms)  -> the session must NOT end
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/device/MdSession.h"

static int bad = 0;
static void CHK(bool ok, const char* what) { std::printf("[silence] %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++bad; }
static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    Config cfg;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem";
    cfg.md_key  = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear();
    cfg.truck_silence_start_s = 3; cfg.truck_silence_s = 2;

    const uint8_t frame[] = {0x00, 0x0b, 0x00, 0x02, 0x00, 0x00};     // channel 0, flags 0x0b, 2 payload bytes (garbage is fine: only "a frame arrived" matters)

    for (int c = 1; c <= 5; ++c) {
        Config cc = cfg; if (c == 5) { cc.truck_silence_s = 0; cc.truck_silence_start_s = 0; cc.truck_silence_early_s = 2; }   // case 5: watchdog DISABLED: the early limit must not switch it back on
        if (c == 4) { cc.truck_silence_s = 30; cc.truck_silence_early_s = 2; cc.truck_silence_early_window_s = 40; }   // case 4: young session, long normal limit, short early limit
        int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 2; }
        MdSession s(cc, sv[0], sv[0]);
        std::atomic<int> dead_calls{0}; s.onDead = [&] { ++dead_calls; };
        std::atomic<bool> done{false};
        const int64_t t0 = nowMs();
        std::thread th([&] { s.run(); done = true; });
        std::atomic<bool> talk{c == 3};
        std::thread peer([&] {
            if (c == 2 || c == 4 || c == 5) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); (void)!::write(sv[1], frame, sizeof frame); }
            while (talk && !done) { (void)!::write(sv[1], frame, sizeof frame); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
        });
        const int64_t until = t0 + (c == 3 ? 6500 : c == 5 ? 7000 : 9000);
        while (!done && nowMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int64_t took = nowMs() - t0;
        if (c == 1) { CHK(done && s.endedBySilence(), "never-heard truck: session ends by itself"); CHK(took >= 2800 && took < 6000, "...after about truck_silence_start_s (3 s)"); }
        if (c == 2) { CHK(done && s.endedBySilence(), "truck that went quiet: session ends by itself"); CHK(took >= 2300 && took < 5000, "...about truck_silence_s (2 s) after its last frame"); }
        if (c == 4) { CHK(done && s.endedBySilence(), "young session: the short early limit ends it long before truck_silence_s (30 s)"); CHK(took >= 2300 && took < 5000, "...about truck_silence_early_s (2 s) after its last frame"); }
        if (c == 5) CHK(!done && !s.endedBySilence(), "watchdog disabled (limits 0): a quiet peer is never cut off, the early limit does not re-enable it");
        if (c == 3) { CHK(!done, "a talking truck is never cut off (6.5 s with a frame every 300 ms)"); }
        CHK((c == 3 || c == 5) ? dead_calls == 0 : dead_calls == 1, (c == 3 || c == 5) ? "onDead (the USB unbind hook) is never called for a talking truck" : "onDead is called exactly once when the truck is declared dead");
        talk = false; s.stop();
        th.join(); peer.join();
        CHK((c == 3 || c == 5) ? !s.endedBySilence() : s.endedBySilence(), (c == 3 || c == 5) ? "stop() is not reported as silence" : "reason is reported as silence");
        ::close(sv[0]); ::close(sv[1]);
    }
    std::printf(bad ? "[silence] FAILED\n" : "[silence] ALL PASSED\n");
    return bad ? 1 : 0;
}
