// Media playback status relay through four real sessions: fake phone (MdSession) -> Pi HuSession -> RawRelay -> Pi MdSession -> fake vehicle (HuSession).
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include "../src/RawRelay.h"
#include <sys/socket.h>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
int main(int argc, char** argv) {
    Config cfg; cfg.relay_nav = false; cfg.relay_media_status = true;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2], B[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A) || socketpair(AF_UNIX, SOCK_STREAM, 0, B)) { perror("socketpair"); return 2; }
    HuSession truck(cfg, A[1]); MdSession piTruck(cfg, A[0], A[0]); HuSession piPhone(cfg, B[1]); MdSession phone(cfg, B[0], B[0]);
    RawRelay relay("Media");
    piPhone.onMediaMessage = [&](uint16_t id, const std::string& b) { relay.fromPhone(id, b); };
    piPhone.onMediaOpen = [&]() { relay.phoneChannelOpen(); };
    relay.setPhoneSend([&](uint16_t id, const std::string& b) { piPhone.sendMedia(id, b); });
    piTruck.onMedia = [&](uint16_t id, const std::string& b) { relay.fromTruck(id, b); };
    piTruck.onMediaChannel = [&](bool open) { if (open) relay.setTruckSend([&](uint16_t id, const std::string& b) { piTruck.sendMedia(id, b); }); };
    std::mutex m; std::vector<std::pair<uint16_t, std::string>> atTruck, atPhone;
    truck.onMediaMessage = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); atTruck.emplace_back(id, b); };
    phone.onMedia = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); atPhone.emplace_back(id, b); };
    std::thread t1([&] { truck.run(); }), t2([&] { piTruck.run(); }), t3([&] { piPhone.run(); }), t4([&] { phone.run(); });
    CHECK(waitFor([&] { return piTruck.videoReady() && phone.videoReady(); }), "both sessions are up");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const std::string meta = "\x0a\x09Song Name\x12\x06" "Artist";                 // any bytes: carried raw
    phone.sendMedia(0x8003, meta); phone.sendMedia(0x8001, "\x08\x02");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return atTruck.size() >= 2; }), "phone's now-playing messages reach the vehicle");
    { std::lock_guard<std::mutex> l(m); CHECK(atTruck.size() >= 2 && atTruck[0].first == 0x8003 && atTruck[0].second == meta && atTruck[1].first == 0x8001, "ids and payloads untouched, in order"); }
    truck.sendMedia(0x8002, "\x08\x01");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return !atPhone.empty(); }, 4000), "a vehicle message on the channel reaches the phone");
    CHECK(relay.droppedCount() == 0, "nothing dropped once both channels were open");
    truck.stop(); piTruck.stop(); piPhone.stop(); phone.stop(); t1.join(); t2.join(); t3.join(); t4.join();
    printf(fails ? "MEDIA E2E FAILED\n" : "MEDIA E2E ALL PASSED\n"); return fails ? 1 : 0;
}
