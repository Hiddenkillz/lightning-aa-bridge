// Vehicle Bluetooth-service answer: a fake vehicle (HuSession, AA_TEST_VEHICLE_BT set) offers channel 10; the Pi's MdSession must announce our Bluetooth address
// (field 1 string, field 2 = numeric comparison) and acknowledge the vehicle's authentication data. With bt_pair_proxy off it must stay silent.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <sys/socket.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
static std::string g_car;
static bool run(bool proxy, const char* const* a, std::string& pairing, bool& gotAuthResult) {
    Config cfg; cfg.bt_pair_proxy = proxy; cfg.relay_nav = false;
    cfg.md_cert = a[1]; cfg.md_key = a[2]; cfg.hu_cert = a[3]; cfg.hu_key = a[4];
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int S[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, S)) return false;
    HuSession veh(cfg, S[1]); MdSession pi(cfg, S[0], S[0]);
    pi.btAddress = [] { return std::string("AA:BB:CC:DD:EE:01"); };
    pi.onCarAddress = [](const std::string& a) { g_car = a; };
    std::mutex m; pairing.clear(); gotAuthResult = false;
    veh.onOtherMessage = [&](int ch, uint16_t id, const std::string& p) {
        std::lock_guard<std::mutex> l(m);
        if (ch == 10 && id == 0x8001) { pairing = p; veh.sendRaw(10, 0x8002, std::string("\x08\x00\x10\x00", 4)); veh.sendRaw(10, 0x8003, std::string("\x0a\x06" "123456\x10\x02", 10)); }
        if (ch == 10 && id == 0x8004) gotAuthResult = true;
    };
    std::thread t1([&] { veh.run(); }), t2([&] { pi.run(); });
    bool up = waitFor([&] { return pi.videoReady(); });
    if (proxy) waitFor([&] { std::lock_guard<std::mutex> l(m); return gotAuthResult; }, 6000); else std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    veh.stop(); pi.stop(); t1.join(); t2.join();
    return up;
}
int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: btproxy_test md_cert md_key hu_cert hu_key\n"); return 2; }
    setenv("AA_TEST_VEHICLE_BT", "11:22:33:44:55:66", 1);
    std::string pairing; bool auth = false;
    CHECK(run(true, argv, pairing, auth), "session up (proxy on)");
    CHECK(pairing == std::string("\x0a\x11" "AA:BB:CC:DD:EE:01\x10\x02", 21), "vehicle received our address + numeric comparison");
    CHECK(auth, "vehicle's authentication data was answered");
    CHECK(g_car == "11:22:33:44:55:66", "the vehicle's own Bluetooth address is handed on (for the Pi-initiated pairing)");
    CHECK(run(false, argv, pairing, auth), "session up (proxy off)");
    CHECK(pairing.empty() && !auth, "proxy off: nothing announced");
    printf(fails ? "BTPROXY FAILED\n" : "BTPROXY ALL PASSED\n"); return fails ? 1 : 0;
}
