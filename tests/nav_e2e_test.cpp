// Offline end-to-end test of the NAVIGATION relay through four REAL sessions:
//   fake vehicle (HuSession acting as Sync 4) <-A-> Pi truck-facing MdSession ==NavRelay== Pi phone-facing HuSession <-B-> fake phone (MdSession acting as the phone)
// Proves: the Pi advertises/opens the navigation service on both sides, the vehicle's Start reaches the phone, phone nav messages (incl. an 0x8008-style blob) reach the
// vehicle byte-exact, and the phone's nav-focus request reaches the vehicle.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include "../src/NavRelay.h"
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <mutex>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 6000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
int main(int argc, char** argv) {
    Config cfg; cfg.relay_nav = true;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2], B[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A) || socketpair(AF_UNIX, SOCK_STREAM, 0, B)) { perror("socketpair"); return 2; }

    HuSession truck(cfg, A[1]);                 // pretends to be Sync 4
    MdSession piTruck(cfg, A[0], A[0]);         // the Pi's vehicle-facing half
    HuSession piPhone(cfg, B[1]);               // the Pi's phone-facing half
    MdSession phone(cfg, B[0], B[0]);           // pretends to be the phone

    std::mutex m; std::vector<std::pair<uint16_t, std::string>> truckGot, phoneGot; std::string focusGot; std::atomic<bool> truckOpened{false}, phoneOpened{false};
    truck.onNavOpen = [&] { truckOpened = true; };
    truck.onNavMessage = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); truckGot.push_back({id, b}); };
    truck.onNavFocus = [&](const std::string& b) { std::lock_guard<std::mutex> l(m); focusGot = b; };
    phone.onNav = [&](uint16_t id, const std::string& b) { std::lock_guard<std::mutex> l(m); phoneGot.push_back({id, b}); };
    phone.onNavChannel = [&](bool o) { phoneOpened = o; };

    NavRelay relay;   // the same wiring as Device.cpp / HeadUnit.cpp
    piTruck.onNav = [&](uint16_t id, const std::string& b) { relay.fromTruck(id, b); };
    piTruck.onNavChannel = [&](bool open) { if (!open) return; relay.setTruckSend([&](uint16_t id, const std::string& b) { piTruck.sendNav(id, b); }); relay.setTruckFocusSend([&](const std::string& b) { piTruck.sendNavFocus(b); }); };
    piPhone.onNavMessage = [&](uint16_t id, const std::string& b) { relay.fromPhone(id, b); };
    piPhone.onNavOpen = [&] { relay.phoneChannelOpen(); };
    piPhone.onNavFocus = [&](const std::string& b) { relay.focusFromPhone(b); };
    relay.setPhoneSend([&](uint16_t id, const std::string& b) { piPhone.sendNav(id, b); });

    std::thread t1([&] { truck.run(); }), t2([&] { piTruck.run(); }), t3([&] { piPhone.run(); }), t4([&] { phone.run(); });

    CHECK(waitFor([&] { return truckOpened.load(); }), "the Pi opened the vehicle's navigation channel");
    CHECK(waitFor([&] { return phoneOpened.load(); }), "the fake phone opened the Pi's navigation channel (the Pi advertises the service to the phone)");
    truck.sendNav(0x8001, "");                                       // vehicle: InstrumentClusterStart (please send navigation data)
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return !phoneGot.empty() && phoneGot[0].first == 0x8001; }), "vehicle Start reaches the phone");
    std::string blob(900, 0); for (size_t i = 0; i < blob.size(); ++i) blob[i] = (char)(i * 7 + 3);      // an 0x8008-sized binary payload with every byte value pattern
    phone.sendNav(0x8008, blob); phone.sendNav(0x8003, std::string("\x08\x01", 2));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return truckGot.size() >= 2; }), "phone navigation messages reach the vehicle");
    { std::lock_guard<std::mutex> l(m);
      CHECK(truckGot.size() >= 2 && truckGot[0].first == 0x8008 && truckGot[0].second == blob, "...the 900-byte 0x8008 payload is byte-exact");
      CHECK(truckGot.size() >= 2 && truckGot[1].first == 0x8003 && truckGot[1].second == std::string("\x08\x01", 2), "...and so is the next message, in order"); }
    phone.sendNavFocus(std::string("\x08\x01", 2));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return !focusGot.empty(); }), "the phone's nav-focus request reaches the vehicle");
    truck.sendNav(0x8002, "");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return phoneGot.size() >= 2 && phoneGot.back().first == 0x8002; }), "vehicle Stop reaches the phone");

    truck.stop(); piTruck.stop(); piPhone.stop(); phone.stop(); t1.join(); t2.join(); t3.join(); t4.join();
    printf(fails ? "NAV E2E FAILED\n" : "NAV E2E ALL PASSED\n"); return fails ? 1 : 0;
}
