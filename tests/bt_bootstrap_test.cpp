// Offline test of the wireless-AA Bluetooth bootstrap exchange: Pi (HU side) vs a scripted fake phone over a socketpair.
#include "../src/headunit/BtBootstrap.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool rd(int fd, std::string& body, uint16_t& id) {
    unsigned char h[4]; if (::read(fd, h, 4) != 4) return false; size_t len = (h[0] << 8) | h[1]; id = (h[2] << 8) | h[3];
    body.assign(len, 0); size_t g = 0; while (g < len) { ssize_t k = ::read(fd, &body[g], len - g); if (k <= 0) return false; g += k; } return true;
}
static void wr(int fd, uint16_t id, const std::string& b) { std::string p; p.push_back(b.size() >> 8); p.push_back(b.size()); p.push_back(id >> 8); p.push_back(id); p += b; (void)!::write(fd, p.data(), p.size()); }
int main() {
    btboot::WifiParams p; p.ip = "192.168.50.1"; p.port = 5277; p.ssid = "AABridge"; p.key = "secretkey12"; p.bssid = "dc:a6:32:00:11:22";
    // byte-exact protobuf encodings
    std::string s = btboot::encodeWifiStartRequest(p);
    CHECK(s == std::string("\x0a\x0c" "192.168.50.1" "\x10\x9d\x29", 17) );   // port 5277 = 0x149d -> varint 9d 29
    std::string i = btboot::encodeWifiInfoResponse(p);
    CHECK(i.find(std::string("\x20\x08\x28\x01", 4)) == i.size() - 4);       // security=8 (WPA2 wire value), ap_type=1 DYNAMIC
    CHECK(i.find("AABridge") != std::string::npos && i.find("secretkey12") != std::string::npos && i.find("dc:a6:32:00:11:22") != std::string::npos);

    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    std::string gotInfo; bool phone_ok = true;
    std::thread phone([&] {
        std::string b; uint16_t id;
        phone_ok &= rd(sv[1], b, id) && id == 1 && b == s;     // WifiStartRequest
        wr(sv[1], 2, "");                                      // WifiInfoRequest
        phone_ok &= rd(sv[1], gotInfo, id) && id == 3;         // WifiInfoResponse
        wr(sv[1], 7, std::string("\x08\x00", 2));              // WifiStartResponse
        wr(sv[1], 6, std::string("\x08\x00", 2));              // WifiConnectStatus
    });
    bool ok = btboot::runWifiBootstrap(sv[0], p, [](const std::string& l) { printf("  %s\n", l.c_str()); }, 2000);
    phone.join();
    CHECK(ok); CHECK(phone_ok); CHECK(gotInfo == i);
    // phone that never answers -> clean failure
    int sw[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sw);
    bool ok2 = btboot::runWifiBootstrap(sw[0], p, [](const std::string&) {}, 300);
    CHECK(!ok2);
    printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
