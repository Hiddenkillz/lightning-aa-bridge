// Offline protocol test: HuSession (Pi as head unit) <-> MdSession (Pi as phone) over a socketpair.
// Proves version/TLS/auth/service discovery/channel opens/video setup+focus+start/ACK windowing/touch with no hardware.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main(int argc, char** argv) {
    Config cfg;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem";
    cfg.md_key  = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem";
    cfg.hu_key  = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear();
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 2; }

    MdSession phone(cfg, sv[0], sv[0]);        // fake phone: the truck-facing code, reused as a phone
    std::atomic<int> win_x{-1}, win_w{-1}, win_h{-1};
    phone.onVisibleWindow = [&](int x, int, int w, int h) { win_x = x; win_w = w; win_h = h; };
    // truck-sensor callback of the fake phone: records the LAST value of each sensor the head unit (HuSession) sent it
    std::atomic<int> got_drv{-1}, got_night{-1}, drv_msgs{0}; 
    phone.onVehicle = [&](int what, int v) { if (what == 6) { got_drv = v; ++drv_msgs; } else if (what == 7) got_night = v; };
    HuSession hu(cfg, sv[1]);
    hu.setPhoneSensors(0, false);              // start state: parked, daytime (what a session gets by default)
    hu.setVisibleWindow(292, 0, 695, 720);     // Ford-like: 1280x720 codec frame, centred 695x720 UI area
    std::atomic<int> frames{0}, cfgmsgs{0}; std::atomic<size_t> bytes{0};
    hu.setFrameSink([&](const uint8_t*, size_t n, uint64_t pts) { if (pts == 0) ++cfgmsgs; else ++frames; bytes += n; });
    // flow control: pretend the decoder is backed up for the first 700 ms; ACKs are held, the phone must stall (not flood), then everything flows
    const auto fc_t0 = std::chrono::steady_clock::now();
    hu.setQueueDepth([fc_t0] { return std::chrono::steady_clock::now() - fc_t0 < std::chrono::milliseconds(700) ? (size_t)10000000 : (size_t)0; }, 100000);

    std::vector<TouchFrame> got; std::mutex gm;
    phone.setTouchFrameHandler([&](const TouchFrame& f) { std::lock_guard<std::mutex> l(gm); got.push_back(f); });
    std::thread tp([&] { phone.run(); });
    std::thread th([&] { hu.run(); });

    for (int i = 0; i < 100 && !phone.videoReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(phone.videoReady());
    printf("video ready after handshake: %d\n", (int)phone.videoReady());

    // Send Annex-B-looking test pictures (SPS/PPS first like the real encoder does), plenty to exceed the ack window.
    std::vector<uint8_t> sps = {0,0,0,1,0x09,0x10, 0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x01,0x40,0x16,0xe8, 0,0,0,1,0x68,0xce,0x3c,0x80};
    phone.sendVideo(sps.data(), sps.size(), 0);
    for (int i = 0; i < 60; ++i) {
        std::vector<uint8_t> pic(2000 + i * 50, (uint8_t)i); pic[0]=0; pic[1]=0; pic[2]=0; pic[3]=1; pic[4]=0x09; pic[5]=0x10; pic[6]=0; pic[7]=0; pic[8]=0; pic[9]=1; pic[10]= (i==0)?0x65:0x41;   // AUD + slice, as the real encoder emits
        phone.sendVideo(pic.data(), pic.size(), 33000ull * (i + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // one big (multi-fragment) picture
    std::vector<uint8_t> big(60000, 0x5a); big[0]=0; big[1]=0; big[2]=0; big[3]=1; big[4]=0x09; big[5]=0x10; big[6]=0; big[7]=0; big[8]=0; big[9]=1; big[10]=0x41;
    phone.sendVideo(big.data(), big.size(), 33000ull * 100);
    for (int i = 0; i < 40 && frames < 61; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {   // two-finger gesture (map zoom): finger 0 down, finger 1 down (pointer-down, index 1), spread, finger 1 up, finger 0 up
        TouchFrame a; a.action = 0; a.index = 0; a.p = {{0, 300, 300}}; hu.sendTouchFrame(a);
        TouchFrame b; b.action = 5; b.index = 1; b.p = {{0, 300, 300}, {1, 400, 350}}; hu.sendTouchFrame(b);
        TouchFrame m; m.action = 2; m.index = 0; m.p = {{0, 250, 280}, {1, 450, 380}}; hu.sendTouchFrame(m);
        TouchFrame u; u.action = 6; u.index = 1; u.p = {{0, 250, 280}, {1, 450, 380}}; hu.sendTouchFrame(u);
        TouchFrame z; z.action = 1; z.index = 0; z.p = {{0, 250, 280}}; hu.sendTouchFrame(z);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::lock_guard<std::mutex> l(gm);
        CHECK(got.size() == 5);
        if (got.size() == 5) {
            CHECK(got[1].action == 5 && got[1].index == 1 && got[1].p.size() == 2);
            CHECK(got[1].p[1].id == 1 && got[1].p[1].x == 400 && got[1].p[1].y == 350);
            CHECK(got[2].action == 2 && got[2].p.size() == 2 && got[2].p[0].x == 250 && got[2].p[1].x == 450);
            CHECK(got[3].action == 6 && got[3].index == 1 && got[3].p.size() == 2);
            CHECK(got[4].action == 1 && got[4].p.size() == 1);
        }
        printf("two-finger gesture round trip: %zu events\n", got.size());
    }
    {   // a fast drag (a move every 2 ms for 300 ms = 150 moves) must be rate-limited, keep the final position, and keep down/up
        { std::lock_guard<std::mutex> l(gm); got.clear(); }
        TouchFrame d; d.action = 0; d.index = 0; d.p = {{0, 100, 100}}; hu.sendTouchFrame(d);
        for (int i = 1; i <= 150; ++i) { TouchFrame m; m.action = 2; m.index = 0; m.p = {{0, 100 + i, 100 + i}}; hu.sendTouchFrame(m); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        TouchFrame u; u.action = 1; u.index = 0; u.p = {{0, 250, 250}}; hu.sendTouchFrame(u);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::lock_guard<std::mutex> l(gm);
        int moves = 0; for (auto& e : got) if (e.action == 2) ++moves;
        printf("fast drag: 150 moves in -> %d moves out (total events %zu)\n", moves, got.size());
        CHECK(moves >= 8 && moves <= 30);
        CHECK(!got.empty() && got.front().action == 0 && got.back().action == 1);
        bool mono = true; int lastx = 0; for (auto& e : got) { if (e.p[0].x < lastx) mono = false; lastx = e.p[0].x; }
        CHECK(mono);                                      // order preserved, never goes backwards
        CHECK(got.size() >= 3 && got[got.size() - 2].p[0].x >= 249);   // last move (flushed before the up) is at the final position
    }
    {   // sensors toward the phone: initial answer to the subscription, then a live change pushed without being asked again
        for (int i = 0; i < 60 && (got_drv < 0 || got_night < 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        printf("sensors: initial driving_status=%d night=%d\n", got_drv.load(), got_night.load());
        CHECK(got_drv == 0 && got_night == 0);
        const int before = drv_msgs;
        hu.setPhoneSensors(26, true);
        for (int i = 0; i < 60 && !(got_drv == 26 && got_night == 1); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // two separate messages: wait for BOTH
        printf("sensors: after change driving_status=%d night=%d\n", got_drv.load(), got_night.load());
        CHECK(got_drv == 26 && got_night == 1);
        hu.setPhoneSensors(26, true);          // unchanged -> not re-sent
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(drv_msgs == before + 1);
        hu.setPhoneSensors(0, false);
        for (int i = 0; i < 60 && !(got_drv == 0 && got_night == 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(got_drv == 0 && got_night == 0);
    }
    hu.sendTouch(640, 360, 0); hu.sendTouch(660, 380, 2); hu.sendTouch(660, 380, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    printf("sink: %d codec-config msgs, %d pictures, %zu bytes; hu says video started=%d rx=%llu\n",
           cfgmsgs.load(), frames.load(), bytes.load(), (int)hu.videoStarted(), (unsigned long long)hu.framesReceived());
    printf("visible window from margins: x=%d %dx%d\n", win_x.load(), win_w.load(), win_h.load());
    CHECK(win_w == 695 && win_h == 720 && win_x == 292);   // margins l292 r293 -> centred 695x720
    CHECK(cfgmsgs >= 1);
    CHECK(frames >= 10 && frames < 61);   // held ACKs made the (loopback) phone skip frames while the decoder was "backed up": flow control engaged, not a flood
    {   // after the queue drained the stream must flow normally again: 10 more pictures, all must arrive
        int before = frames;
        for (int i = 0; i < 10; ++i) { std::vector<uint8_t> pic(3000, (uint8_t)i); pic[0]=0; pic[1]=0; pic[2]=0; pic[3]=1; pic[4]=0x09; pic[5]=0x10; pic[6]=0; pic[7]=0; pic[8]=0; pic[9]=1; pic[10]=0x41;
            phone.sendVideo(pic.data(), pic.size(), 33000ull * (200 + i)); std::this_thread::sleep_for(std::chrono::milliseconds(15)); }
        for (int i = 0; i < 40 && frames < before + 10; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        printf("after release: +%d of 10 pictures\n", frames.load() - before);
        CHECK(frames >= before + 10);
    }
    CHECK(hu.videoStarted());
    hu.stop(); phone.stop();
    th.join(); tp.join();
    printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
