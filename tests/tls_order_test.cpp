// Regression test for the TLS record ordering race: enqueueRaw used to encrypt (taking the next TLS sequence number) and queue the frames in TWO separate steps, so two threads
// sending at the same moment could put records on the wire out of order -> the peer fails with "bad record mac" and the session dies (seen as a flaky e2e test; on the truck it
// looks like "the vehicle/phone suddenly stops talking"). Here 6 threads per direction hammer a real MdSession <-> HuSession TLS pair with big and small messages.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
struct Tally { std::mutex m; std::map<int, int> next; std::atomic<long> got{0}; std::atomic<long> bad{0};
    void on(const std::string& b) { if (b.size() < 8) { ++bad; return; } int t, n; memcpy(&t, b.data(), 4); memcpy(&n, b.data() + 4, 4);
        for (size_t i = 8; i < b.size(); ++i) if ((uint8_t)b[i] != (uint8_t)(t * 31 + n + i)) { ++bad; return; }     // payload integrity
        std::lock_guard<std::mutex> l(m); if (next[t] != n) ++bad; next[t] = n + 1; ++got; } };      // per-thread order preserved
int main(int argc, char** argv) {
    Config cfg; cfg.relay_nav = true; cfg.relay_audio = false;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 2; }
    MdSession phone(cfg, sv[0], sv[0]); HuSession hu(cfg, sv[1]);
    Tally atHu, atPhone; std::atomic<bool> open{false};
    hu.onNavMessage = [&](uint16_t, const std::string& b) { atHu.on(b); };
    phone.onNav = [&](uint16_t, const std::string& b) { atPhone.on(b); };
    phone.onNavChannel = [&](bool o) { open = o; };
    std::thread t1([&] { hu.run(); }), t2([&] { phone.run(); });
    CHECK(waitFor([&] { return open.load(); }, 10000), "TLS session up, navigation channel open");
    const int kThreads = 6, kPer = 250; std::vector<std::thread> w;
    auto make = [&](int t, int n, std::mt19937& g) { size_t sz = 8 + (g() % 5 == 0 ? 20000 + g() % 20000 : g() % 200); std::string b(sz, 0); memcpy(&b[0], &t, 4); memcpy(&b[4], &n, 4); for (size_t i = 8; i < sz; ++i) b[i] = (char)(uint8_t)(t * 31 + n + i); return b; };
    for (int t = 0; t < kThreads; ++t) {
        w.emplace_back([&, t] { std::mt19937 g(t + 1); for (int n = 0; n < kPer; ++n) phone.sendNav(0x8003, make(t, n, g)); });      // phone -> HU
        w.emplace_back([&, t] { std::mt19937 g(100 + t); for (int n = 0; n < kPer; ++n) hu.sendNav(0x8003, make(t, n, g)); });          // HU -> phone
    }
    for (auto& x : w) x.join();
    const long want = (long)kThreads * kPer;
    CHECK(waitFor([&] { return atHu.got >= want && atPhone.got >= want; }, 40000), "every message arrives in BOTH directions (the TLS session survived the concurrent senders)");
    CHECK(atHu.bad == 0 && atPhone.bad == 0, "payloads intact and each sender's messages in order");
    hu.stop(); phone.stop(); t1.join(); t2.join();
    printf(fails ? "TLS ORDER FAILED\n" : "TLS ORDER ALL PASSED\n"); return fails ? 1 : 0;
}
