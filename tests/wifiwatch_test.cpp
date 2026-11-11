// g++ -std=c++17 -pthread -Isrc tests/wifiwatch_test.cpp -o wifiwatch_test && ./wifiwatch_test
#include "headunit/WifiWatch.h"
#include <cstdio>
#include <sstream>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)
int main() {
    const std::string p = "/tmp/wifiwatch-test.log";
    { std::ofstream o(p); o << "1.0: wlan0: AP-STA-CONNECTED old:old:old\n"; }   // history before start must NOT be echoed
    std::ostringstream cap; auto* old = std::cout.rdbuf(cap.rdbuf());
    WifiWatch w(p); w.start(); std::this_thread::sleep_for(std::chrono::milliseconds(600));
    { std::ofstream o(p, std::ios::app);
      o << "1777.1: 1777.1: wlan0: STA AA:BB:CC:DD:EE:01 IEEE 802.11: associated\n1777.2: wlan0: AP-STA-CONNECTED AA:BB:CC:DD:EE:01\n1777.3: wlan0: random noise line\n"
        << "1778.0: 1778.0: wlan0: STA AA:BB:CC:DD:EE:01 IEEE 802.11: disassociated\n1778.0: wlan0: AP-STA-DISCONNECTED AA:BB:CC:DD:EE:01\n1779.0: wlan0: interface state HT_SCAN->ENABLED\n1779.1: wlan0: AP-ENABLED \n"; }
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    { std::ofstream o(p, std::ios::app); o << "1780.0: wlan0: AP-STA-CONNECTED partial"; }   // no newline yet: must wait
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const std::string mid = cap.str();
    { std::ofstream o(p, std::ios::app); o << "\n"; }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    w.stop(); std::cout.rdbuf(old); const std::string s = cap.str();
    CHK(s.find("old:old") == std::string::npos, "history before start is not echoed");
    CHK(s.find("[Wifi] STA AA:BB:CC:DD:EE:01 IEEE 802.11: associated") != std::string::npos, "association echoed");
    CHK(s.find("AP-STA-DISCONNECTED AA:BB:CC:DD:EE:01") != std::string::npos, "disconnect echoed");
    CHK(s.find("HT_SCAN->ENABLED") != std::string::npos && s.find("AP-ENABLED") != std::string::npos, "AP state echoed");
    CHK(s.find("random noise") == std::string::npos, "noise filtered");
    CHK(mid.find("partial") == std::string::npos && s.find("partial") != std::string::npos, "half-written line waits for its newline, then appears once");
    CHK(s.find("1777") == std::string::npos, "epoch timestamps stripped");
    std::remove(p.c_str()); std::cout << (fails ? "WIFIWATCH TEST FAILED" : "WIFIWATCH TEST PASSED") << std::endl; return fails ? 1 : 0;
}
