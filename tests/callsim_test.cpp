// Staged test call (bt_call_sim): the vehicle side of the bridge over a socketpair, no phone, no Bluetooth. Checks the vehicle sees a phone with
// service/signal/battery, is rung with caller id after the delay, that its answer/hang-up drive the call state, that dialling runs an outgoing call,
// and that an unanswered call is picked up by the bridge itself.
#include "../src/btcall/CallBridge.h"
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
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
static int handshake(btcall::CallBridge& cb, std::string& vr) {
    int V[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, V); cb.onVehicleLink(V[1], "AA:BB:CC:DD:EE:FF");
    put(V[0], "AT+BRSF=1015\r"); waitFor(V[0], vr, "+BRSF: 96"); vr.clear();
    put(V[0], "AT+CIND=?\r"); waitFor(V[0], vr, "OK"); vr.clear();
    put(V[0], "AT+CIND?\r"); waitFor(V[0], vr, "OK");
    CHK(vr.find("+CIND: 1,0,0,0,4,0,4") != std::string::npos, "vehicle sees a phone: in service, 4 bars, battery 4/5");
    vr.clear(); put(V[0], "AT+CLIP=1\r"); waitFor(V[0], vr, "OK"); vr.clear(); put(V[0], "AT+CMER=3,0,0,1\r"); waitFor(V[0], vr, "OK"); vr.clear();
    return V[0];
}
int main() {
    { Config cfg; cfg.bt_call_sim = true; cfg.bt_call_sim_delay_s = 1; cfg.bt_call_sim_ring_s = 20; cfg.bt_call_sim_len_s = 20;
      btcall::CallBridge cb(cfg); std::string vr; int v = handshake(cb, vr);
      CHK(waitFor(v, vr, "+CIEV: 3,1", 3000), "incoming call state after the delay");
      CHK(waitFor(v, vr, "RING", 3000), "vehicle is rung");
      CHK(waitFor(v, vr, "+CLIP: \"2055550123\",129", 3000), "caller id reaches the vehicle");
      vr.clear(); put(v, "ATA\r");
      CHK(waitFor(v, vr, "+CIEV: 2,1", 2000), "vehicle answers -> call active");
      CHK(waitFor(v, vr, "+CIEV: 3,0", 2000), "call setup cleared after the answer");
      vr.clear(); put(v, "AT+CLCC\r"); CHK(waitFor(v, vr, "+CLCC: 1,0,0,0,0,\"2055550123\",129", 2000), "call list shows the active call");
      vr.clear(); put(v, "AT+CHUP\r");
      CHK(waitFor(v, vr, "+CIEV: 2,0", 2000), "vehicle hangs up -> call ended");
      vr.clear(); put(v, "ATD2055551212;\r");
      CHK(waitFor(v, vr, "+CIEV: 3,2", 2000), "vehicle dials -> outgoing call set up");
      CHK(waitFor(v, vr, "+CIEV: 3,3", 3000), "far end ringing");
      CHK(waitFor(v, vr, "+CIEV: 2,1", 4000), "outgoing call goes active");
      vr.clear(); put(v, "AT+CHUP\r"); CHK(waitFor(v, vr, "+CIEV: 2,0", 2000), "outgoing call hung up on the vehicle");
      ::close(v); cb.stop(); }
    { Config cfg; cfg.bt_call_sim = true; cfg.bt_call_sim_delay_s = 1; cfg.bt_call_sim_ring_s = 1; cfg.bt_call_sim_len_s = 1;
      btcall::CallBridge cb(cfg); std::string vr; int v = handshake(cb, vr);
      CHK(waitFor(v, vr, "RING", 3000), "second bridge: rings");
      CHK(waitFor(v, vr, "+CIEV: 2,1", 3000), "unanswered call is picked up by the bridge itself");
      CHK(waitFor(v, vr, "+CIEV: 2,0", 3000), "and ends by itself after the call length");
      ::close(v); cb.stop(); }
    { Config cfg; btcall::CallBridge cb(cfg); int V[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, V); cb.onVehicleLink(V[1], "AA:BB:CC:DD:EE:FF"); std::string vr;
      put(V[0], "AT+BRSF=1015\r"); waitFor(V[0], vr, "OK"); vr.clear(); put(V[0], "AT+CIND?\r"); waitFor(V[0], vr, "OK");
      CHK(vr.find("+CIND: 0,0,0,0,0,0,0") != std::string::npos, "bt_call_sim off: no fake service/signal/battery");
      vr.clear(); put(V[0], "AT+CMER=3,0,0,1\r"); waitFor(V[0], vr, "OK"); vr.clear(); vr += drain(V[0], 1600);
      CHK(vr.find("RING") == std::string::npos && vr.find("+CIEV") == std::string::npos, "bt_call_sim off: nothing staged");
      ::close(V[0]); cb.stop(); }
    { // a real phone link takes precedence over the staged call: vehicle dial goes to the phone, no fake call
      Config cfg; cfg.bt_call_sim = true; cfg.bt_call_sim_delay_s = 30; btcall::CallBridge cb(cfg); std::string vr; int v = handshake(cb, vr);
      int P[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, P); cb.onPhoneLink(P[1], "11:22:33:44:55:66");
      { auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3); while (!cb.phoneLinked() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
      CHK(cb.phoneLinked(), "bridge reports the phone link");
      std::string pr; const char* steps[][2] = {{"AT+BRSF=", "\r\n+BRSF: 4063\r\n\r\nOK\r\n"}, {"AT+CIND=?", "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0,3)),(\"callheld\",(0,2)),(\"signal\",(0,5)),(\"roam\",(0,1)),(\"battchg\",(0,5))\r\n\r\nOK\r\n"},
          {"AT+CIND?", "\r\n+CIND: 1,0,0,0,5,0,4\r\n\r\nOK\r\n"}, {"AT+CMER=", "\r\nOK\r\n"}};
      bool ok = true; for (auto& s : steps) { if (!waitFor(P[0], pr, s[0])) { ok = false; break; } pr.clear(); put(P[0], s[1]); }
      CHK(ok, "phone handshake runs with test mode on");
      std::atomic<bool> dialed{false}, stop_okr{false};
      std::thread okr([&] { std::string acc; while (!stop_okr) { std::string d = drain(P[0], 100); acc += d; size_t pos; while ((pos = acc.find('\r')) != std::string::npos) { std::string l = acc.substr(0, pos); acc.erase(0, pos + 1);
          if (l == "AT+COPS?") put(P[0], "\r\n+COPS: 0,0,\"Test Net\"\r\n\r\nOK\r\n"); else if (!l.empty()) put(P[0], "\r\nOK\r\n"); if (l == "ATD2055551212;") dialed = true; } } });
      CHK(waitFor(v, vr, "+CIEV: 5,5", 5000), "real phone signal (5 bars) replaces the fake 4 on the vehicle");
      vr.clear(); put(v, "ATD2055551212;\r");
      { auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8); while (!dialed && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
      CHK(dialed.load(), "vehicle dial is forwarded to the real phone");
      stop_okr = true; okr.join();
      vr += drain(v, 300); CHK(vr.find("+CIEV: 3,2") == std::string::npos, "no fake outgoing call staged while the phone is linked");
      vr.clear(); put(v, "AT+CGMI\r"); CHK(waitFor(v, vr, "+CGMI: Android", 2000), "manufacturer answered (Sync asks)");
      vr.clear(); put(v, "AT+CSCS=?\r"); CHK(waitFor(v, vr, "+CSCS: (\"UTF-8\",\"IRA\")", 2000), "character sets answered");
      ::close(P[0]); std::this_thread::sleep_for(std::chrono::milliseconds(400)); CHK(!cb.phoneLinked(), "phone link loss is reported");
      ::close(v); cb.stop(); }
    std::cout << (fails ? "CALLSIM TEST FAILED" : "CALLSIM ALL PASSED") << std::endl; return fails ? 1 : 0;
}
