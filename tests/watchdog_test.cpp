// g++ -std=c++17 -pthread -Isrc tests/watchdog_test.cpp -o watchdog_test && ./watchdog_test
#include "Watchdog.h"
#include <cstdio>
#include <sys/socket.h>
#include <fcntl.h>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)
int main() {
    { LagGuard g(3000, 20000);
      CHK(!g.feed(100, 0), "healthy lag: no trigger");
      CHK(!g.feed(5000, 1000) && !g.feed(5000, 15000), "high lag shorter than the hold: no trigger");
      CHK(!g.feed(-1, 16000), "unknown lag is ignored");
      CHK(g.feed(5000, 21000), "high lag held 20 s: trigger");
      LagGuard h(3000, 20000); h.feed(5000, 0); h.feed(200, 10000); CHK(!h.feed(5000, 25000), "a healthy sample resets the timer");
      LagGuard off(0, 1); CHK(!off.feed(999999, 0) && !off.feed(999999, 99999), "limit 0 = disabled"); }
    // sdNotify + HangWatchdog against a real datagram socket
    const char* path = "/tmp/wd-test.sock"; ::unlink(path);
    int s = ::socket(AF_UNIX, SOCK_DGRAM, 0); sockaddr_un a{}; a.sun_family = AF_UNIX; strcpy(a.sun_path, path); ::bind(s, (sockaddr*)&a, sizeof a);
    fcntl(s, F_SETFL, O_NONBLOCK);
    auto drain = [&](int& ready, int& wd) { char b[64]; ssize_t n; while ((n = ::recv(s, b, sizeof b - 1, 0)) > 0) { b[n] = 0; if (!strcmp(b, "READY=1")) ++ready; else if (!strcmp(b, "WATCHDOG=1")) ++wd; } };
    std::atomic<uint64_t> prog{0}; std::atomic<bool> feed{true};
    std::thread f([&] { while (feed) { ++prog; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });
    HangWatchdog w(2, [&] { return prog.load(); }, path); w.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2600));
    int ready = 0, wd = 0; drain(ready, wd);
    CHK(ready == 1, "READY=1 sent once"); CHK(wd >= 1, "WATCHDOG=1 pings flow while frames flow");
    feed = false; f.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));    // 2 s grace + margin
    drain(ready, wd); int before = wd;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); drain(ready, wd);
    CHK(wd - before == 0 && w.withheld() > 0, "frames stalled: pings are withheld (systemd would restart us)");
    w.stop(); ::close(s); ::unlink(path);
    std::cout << (fails ? "WATCHDOG TEST FAILED" : "WATCHDOG TEST PASSED") << std::endl; return fails ? 1 : 0;
}
