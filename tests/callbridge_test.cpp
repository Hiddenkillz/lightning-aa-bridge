// CallBridge with its real threads over socketpairs standing in for the two RFCOMM links (SCO needs real Bluetooth and is not exercised here).
#include "../src/btcall/CallBridge.h"
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { std::cout << "FAIL: " << m << std::endl; ++fails; } else std::cout << "ok:   " << m << std::endl; } while (0)
static std::string drain(int fd, int ms) {
    std::string out; auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) { char b[512]; ssize_t n = ::read(fd, b, sizeof b); if (n <= 0) break; out.append(b, (size_t)n); } }
    return out;
}
static bool waitFor(int fd, std::string& acc, const char* needle, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (acc.find(needle) != std::string::npos) return true; acc += drain(fd, 100); }
    return acc.find(needle) != std::string::npos;
}
static void put(int fd, const std::string& s) { (void)!::write(fd, s.data(), s.size()); }
int main() {
    Config cfg; btcall::CallBridge cb(cfg);
    int P[2], V[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, P); socketpair(AF_UNIX, SOCK_STREAM, 0, V);
    cb.onPhoneLink(P[1], "11:22:33:44:55:66"); cb.onVehicleLink(V[1], "AA:BB:CC:DD:EE:FF");
    // fake phone: answer the handshake
    std::string pr; bool ok = true;
    const char* steps[][2] = {{"AT+BRSF=", "\r\n+BRSF: 4063\r\n\r\nOK\r\n"}, {"AT+CIND=?", "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0,3)),(\"callheld\",(0,2)),(\"signal\",(0,5)),(\"roam\",(0,1)),(\"battchg\",(0,5))\r\n\r\nOK\r\n"},
        {"AT+CIND?", "\r\n+CIND: 1,0,0,0,5,0,4\r\n\r\nOK\r\n"}, {"AT+CMER=", "\r\nOK\r\n"}};
    for (auto& s : steps) { if (!waitFor(P[0], pr, s[0])) { ok = false; break; } pr.clear(); put(P[0], s[1]); }
    CHK(ok, "the bridge runs the hands-free handshake with the phone");
    // remaining scripted commands just get OK
    // okr is the ONLY reader of P[0] while it runs: a second poll+read on the same blocking socket (the old waitFor(P[0], "ATA") on the main thread)
    // raced it and one of the two blocked in read() forever (the "known qemu hang" of this test in-suite).
    std::atomic<bool> saw_ata{false};
    std::thread okr([&] { std::string acc; for (int i = 0; i < 40; ++i) { std::string d = drain(P[0], 100); acc += d; size_t pos; while ((pos = acc.find('\r')) != std::string::npos) { std::string l = acc.substr(0, pos); acc.erase(0, pos + 1);
        if (l.find("ATA") != std::string::npos) saw_ata = true;
        if (l == "AT+COPS?") put(P[0], "\r\n+COPS: 0,0,\"Test Net\"\r\n\r\nOK\r\n"); else if (!l.empty()) put(P[0], "\r\nOK\r\n"); } } });
    // fake vehicle handshake
    std::string vr; put(V[0], "AT+BRSF=1015\r"); CHK(waitFor(V[0], vr, "+BRSF: 96"), "vehicle gets the bridge's features");
    vr.clear(); put(V[0], "AT+CIND?\r"); CHK(waitFor(V[0], vr, "+CIND: 1,0,0,0,5,0,4"), "vehicle sees the phone's signal/battery through the bridge");
    vr.clear(); put(V[0], "AT+CLIP=1\r"); waitFor(V[0], vr, "OK"); vr.clear(); put(V[0], "AT+CMER=3,0,0,1\r"); CHK(waitFor(V[0], vr, "OK"), "vehicle enables events");
    // incoming call from the phone
    vr.clear(); put(P[0], "\r\n+CIEV: 3,1\r\n\r\nRING\r\n\r\n+CLIP: \"2565550142\",129\r\n");
    CHK(waitFor(V[0], vr, "+CIEV: 3,1"), "incoming call state reaches the vehicle");
    CHK(waitFor(V[0], vr, "+CLIP: \"2565550142\",129", 3000), "vehicle is rung with the caller number");
    // vehicle answers
    put(V[0], "ATA\r"); for (int i = 0; i < 30 && !saw_ata; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHK(saw_ata.load(), "the vehicle's answer (ATA) reaches the phone");
    okr.join();
    // links drop
    ::close(P[0]); std::this_thread::sleep_for(std::chrono::milliseconds(500));
    vr.clear(); vr += drain(V[0], 600); CHK(vr.find("+CIEV: 1,0") != std::string::npos, "phone link loss tells the vehicle the service is gone");
    ::close(V[0]); cb.stop();
    std::cout << (fails ? "CALLBRIDGE TEST FAILED" : "CALLBRIDGE ALL PASSED") << std::endl; return fails ? 1 : 0;
}
