// Call-control logic: HfClient (toward the phone), AgServer (toward the vehicle) and CallCore joining them, driven by a scripted fake phone and a fake vehicle.
// Also the SCO audio pump over socketpairs.
#include "../src/btcall/Hfp.h"
#include "../src/btcall/Pump.h"
#include <sys/socket.h>
#include <algorithm>
#include <iostream>
#include <cstring>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { std::cout << "FAIL: " << m << std::endl; ++fails; } else std::cout << "ok:   " << m << std::endl; } while (0)
using namespace hfp;
static bool has(const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; }

// A phone (Audio Gateway) that answers the way an Android phone does.
struct FakePhone {
    std::string rx, out; HfClient* hf = nullptr; std::vector<std::string> got;
    void onBytes(const std::string& s) {
        for (auto& l : takeLines(rx, s.data(), s.size())) {
            got.push_back(l); std::string r;
            if (startsWith(l.c_str(), "AT+BRSF")) r = "\r\n+BRSF: 4063\r\n\r\nOK\r\n";
            else if (l == "AT+CIND=?") r = "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"call_setup\",(0,3)),(\"callheld\",(0,2)),(\"signal\",(0,5)),(\"roam\",(0,1)),(\"battchg\",(0,5))\r\n\r\nOK\r\n";
            else if (l == "AT+CIND?") r = "\r\n+CIND: 1,0,0,0,4,0,5\r\n\r\nOK\r\n";
            else if (l == "AT+COPS?") r = "\r\n+COPS: 0,0,\"T-Mobile\"\r\n\r\nOK\r\n";
            else if (l == "AT+CHLD=?") r = "\r\nERROR\r\n";
            else r = "\r\nOK\r\n";
            if (hf) hf->feed(r.data(), r.size());
        }
    }
    void unsolicited(const std::string& s) { if (hf) hf->feed(s.data(), s.size()); }
};
struct FakeVehicle {                       // a head unit (Hands-Free) talking to AgServer
    std::string all, rx; AgServer* ag = nullptr;
    void onBytes(const std::string& s) { all += s; }
    void say(const std::string& l) { std::string s = l + "\r"; if (ag) ag->feed(s.data(), s.size()); }
    void clear() { all.clear(); }
};

int main() {
    CallCore core; FakePhone ph; FakeVehicle veh; ph.hf = &core.phone; veh.ag = &core.vehicle;
    core.phone.send = [&](const std::string& s) { ph.onBytes(s); };
    core.vehicle.send = [&](const std::string& s) { veh.onBytes(s); };
    int audio_events = 0; bool audio_now = false;
    core.onCallAudio = [&](bool a) { ++audio_events; audio_now = a; };

    // ---- phone side handshake
    core.phone.start();
    CHK(core.phone.ready(), "phone service level connection comes up");
    CHK(core.vehicle.ind.service == 1 && core.vehicle.ind.signal == 4 && core.vehicle.ind.battchg == 5, "phone indicators (service/signal/battery) land in the vehicle-facing state");
    CHK(core.vehicle.operatorName == "T-Mobile", "operator name taken from the phone");
    bool sentCmer = false, sentBrsf = false; for (auto& g : ph.got) { if (g == "AT+CMER=3,0,0,1") sentCmer = true; if (g == "AT+BRSF=36") sentBrsf = true; }
    CHK(sentCmer && sentBrsf, "HF advertises features 36 (CLI + enhanced call status; no codec negotiation) and enables indicator events");

    // ---- vehicle side handshake (what a Ford HF does)
    veh.say("AT+BRSF=1015"); CHK(has(veh.all, "+BRSF: 96") && has(veh.all, "OK"), "AG answers BRSF without codec negotiation (96)");
    veh.clear(); veh.say("AT+CIND=?"); CHK(has(veh.all, "\"callsetup\",(0,3)") && has(veh.all, "\"battchg\",(0,5)"), "AG lists its 7 indicators");
    veh.clear(); veh.say("AT+CIND?"); CHK(has(veh.all, "+CIND: 1,0,0,0,4,0,5"), "AG reports the phone's current state");
    bool vready = false; core.vehicle.onReady = [&] { vready = true; };
    veh.clear(); veh.say("AT+CMER=3,0,0,1"); CHK(vready && core.vehicle.ready(), "AT+CMER completes the vehicle's service level connection");
    veh.clear(); veh.say("AT+CHLD=?"); CHK(has(veh.all, "ERROR"), "no 3-way calling claimed");
    veh.clear(); veh.say("AT+CLIP=1"); veh.say("AT+CCWA=1"); veh.say("AT+VGS=8"); veh.say("AT+NREC=0"); CHK(!has(veh.all, "ERROR"), "CLIP / CCWA / VGS / NREC accepted");
    veh.clear(); veh.say("AT+XAPL=iPhone,2"); CHK(has(veh.all, "ERROR"), "unknown vendor commands are refused cleanly");
    veh.clear(); veh.say("AT+COPS?"); CHK(has(veh.all, "+COPS: 0,0,\"T-Mobile\""), "operator query answered");

    // ---- incoming call
    veh.clear();
    ph.unsolicited("\r\n+CIEV: 3,1\r\n"); ph.unsolicited("\r\nRING\r\n"); ph.unsolicited("\r\n+CLIP: \"+12565550123\",145\r\n");
    CHK(has(veh.all, "+CIEV: 3,1"), "callsetup=1 forwarded to the vehicle immediately");
    CHK(core.vehicle.number == "+12565550123" && core.vehicle.numberType == 145, "caller id captured");
    core.tick(1000); CHK(has(veh.all, "RING") && has(veh.all, "+CLIP: \"+12565550123\",145"), "ring generated for the vehicle with caller id");
    size_t n1 = veh.all.size(); core.tick(1500); CHK(veh.all.size() == n1, "no second ring after 0.5 s");
    core.tick(3600); CHK(veh.all.size() > n1, "ring repeats after 2.5 s");
    CHK(!audio_now, "no call audio while it is only ringing");
    veh.clear(); veh.say("AT+CLCC"); CHK(has(veh.all, "+CLCC: 1,1,4,0,0,\"+12565550123\",145"), "AT+CLCC shows the incoming call");

    // ---- vehicle answers
    ph.got.clear(); veh.clear(); veh.say("ATA");
    CHK(!ph.got.empty() && ph.got.back() == "ATA" && has(veh.all, "OK"), "ATA from the vehicle becomes ATA to the phone");
    ph.unsolicited("\r\n+CIEV: 3,0\r\n"); ph.unsolicited("\r\n+CIEV: 2,1\r\n");
    CHK(audio_now && core.audioWanted(), "answered call asks for call audio");
    veh.clear(); core.tick(9000); CHK(!has(veh.all, "RING"), "ringing stops once answered");
    veh.clear(); veh.say("AT+CLCC"); CHK(has(veh.all, "+CLCC: 1,0,0,0,0"), "AT+CLCC shows the active call");

    // ---- vehicle hangs up
    ph.got.clear(); veh.say("AT+CHUP"); CHK(!ph.got.empty() && ph.got.back() == "AT+CHUP", "AT+CHUP forwarded");
    ph.unsolicited("\r\n+CIEV: 2,0\r\n"); CHK(!audio_now && core.vehicle.number.empty(), "call end drops audio and clears the number");

    // ---- outgoing call from the vehicle
    ph.got.clear(); veh.clear(); veh.say("ATD2565550199;"); CHK(!ph.got.empty() && ph.got.back() == "ATD2565550199;", "dial forwarded with the number");
    ph.unsolicited("\r\n+CIEV: 3,2\r\n"); CHK(audio_now, "dialing asks for call audio (ringback / answer)");
    CHK(std::find(ph.got.begin(), ph.got.end(), "AT+CLCC") == ph.got.end(), "number known from the vehicle's dial: no call-list query needed");
    ph.unsolicited("\r\n+CIEV: 3,0\r\n"); ph.unsolicited("\r\n+CIEV: 2,1\r\n"); ph.unsolicited("\r\n+CIEV: 2,0\r\n"); CHK(!audio_now, "outgoing call ended");
    ph.got.clear(); veh.say("AT+BLDN"); CHK(ph.got.back() == "AT+BLDN", "redial forwarded");
    ph.got.clear(); veh.say("AT+VTS=5"); CHK(ph.got.back() == "AT+VTS=5", "DTMF forwarded");
    ph.got.clear(); veh.say("AT+BVRA=1"); CHK(ph.got.back() == "AT+BVRA=1", "voice assistant request forwarded to the phone");

    // ---- a call dialled ON THE PHONE: no +CLIP comes; the bridge must ask the call list and give the vehicle the number (ui36: 'Unknown caller')
    ph.got.clear(); veh.clear(); ph.unsolicited("\r\n+CIEV: 3,2\r\n");
    CHK(std::find(ph.got.begin(), ph.got.end(), "AT+CLCC") != ph.got.end(), "outgoing call with no number -> the phone is asked for its call list");
    ph.unsolicited("\r\n+CLCC: 1,0,2,0,0,\"2055550123\",129\r\n\r\nOK\r\n");
    veh.clear(); veh.say("AT+CLCC"); CHK(has(veh.all, "+CLCC: 1,0,2,0,0,\"2055550123\",129"), "vehicle's call list now carries the dialled number");
    ph.unsolicited("\r\n+CIEV: 2,1\r\n"); ph.unsolicited("\r\n+CIEV: 3,0\r\n");                                   // Android order: call=1, then callsetup=0
    { int n = 0; for (auto& g : ph.got) if (g == "AT+CLCC") ++n; CHK(n == 1, "asked once per call, not on every state change"); }
    ph.unsolicited("\r\n+CIEV: 2,0\r\n"); CHK(core.vehicle.number.empty(), "number cleared when the call ends");
    ph.got.clear(); ph.unsolicited("\r\n+CIEV: 3,1\r\n"); ph.unsolicited("\r\nRING\r\n\r\n+CLIP: \"2565550142\",129\r\n");
    CHK(core.vehicle.number == "2565550142", "incoming call: number from +CLIP as before");
    ph.unsolicited("\r\n+CIEV: 3,0\r\n"); ph.unsolicited("\r\n+CIEV: 2,1\r\n"); ph.unsolicited("\r\n+CIEV: 2,0\r\n");

    // ---- a phone call that started on the phone itself (outgoing), phone drops, battery
    veh.clear(); ph.unsolicited("\r\n+CIEV: 7,3\r\n"); CHK(has(veh.all, "+CIEV: 7,3"), "battery level change forwarded");
    veh.clear(); core.phoneDown(); CHK(has(veh.all, "+CIEV: 1,0"), "phone link loss marks the service as unavailable");

    // ---- robustness
    { CallCore c2; std::string big(10000, 'x'); c2.vehicle.feed(big.data(), big.size()); c2.vehicle.feed("\r\nAT+CIND?\r", 11); CHK(true, "garbage and split lines do not crash");
      c2.phone.feed("\xff\xfe\r+CIEV: 9,9\r", 16); CHK(true, "unknown indicator index ignored"); }
    { CallCore c3; std::string o; c3.vehicle.send = [&](const std::string& s) { o += s; }; const char* a = "AT+CIN"; const char* b = "D?\rAT+CMER=3,0,0,1\r"; c3.vehicle.feed(a, 6); c3.vehicle.feed(b, strlen(b)); CHK(has(o, "+CIND:") && c3.vehicle.ready(), "a command split across two reads is reassembled"); }

    // ---- audio pump
    { int A[2], B[2]; socketpair(AF_UNIX, SOCK_SEQPACKET, 0, A); socketpair(AF_UNIX, SOCK_SEQPACKET, 0, B);
      std::atomic<bool> stop{false}; btcall::PumpStats st; const char* why = nullptr;
      std::thread t([&] { why = btcall::pumpAudio(A[1], B[1], stop, st); });
      uint8_t pkt[60]; for (int i = 0; i < 60; ++i) pkt[i] = (uint8_t)i;
      for (int i = 0; i < 20; ++i) { ::send(A[0], pkt, 60, 0); ::send(B[0], pkt, 48, 0); }
      uint8_t rb[128]; int gotB = 0, gotA = 0; long bytesB = 0, bytesA = 0;
      for (int i = 0; i < 400 && (gotB < 20 || gotA < 20); ++i) { pollfd p[2] = {{B[0], POLLIN, 0}, {A[0], POLLIN, 0}}; if (::poll(p, 2, 10) > 0) {
          if (p[0].revents & POLLIN) { ssize_t n = ::recv(B[0], rb, sizeof rb, 0); if (n > 0) { ++gotB; bytesB += n; } }
          if (p[1].revents & POLLIN) { ssize_t n = ::recv(A[0], rb, sizeof rb, 0); if (n > 0) { ++gotA; bytesA += n; } } } }
      CHK(gotB == 20 && bytesB == 1200, "audio A -> B: 20 packets, 1200 bytes, unchanged");
      CHK(gotA == 20 && bytesA == 960, "audio B -> A: 20 packets, 960 bytes");
      ::close(A[0]); std::this_thread::sleep_for(std::chrono::milliseconds(300)); t.join();
      CHK(why && std::string(why) == "side A closed", "pump ends when one side closes");
      ::close(A[1]); ::close(B[0]); ::close(B[1]); }
    // ---- re-chunking to the destination's SCO packet size (ui36a: 60-byte phone packets written to a 64-byte dongle link = silence)
    { // AUTO sizing: each side is written in the packet size it sends us (ui41a: the USB dongle delivers 48 B and silently drops 64 B packets)
      int A[2], B[2]; CHK(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, A) == 0 && ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, B) == 0, "socketpairs");
      std::atomic<bool> stop{false}; btcall::PumpStats st; const char* why = nullptr;
      std::thread t([&] { why = btcall::pumpAudio(A[1], B[1], stop, st, 0, -1, -1); });
      std::vector<uint8_t> p60(60, 1), p48(48, 2);
      for (int i = 0; i < 4; ++i) ::send(A[0], p60.data(), 60, 0);           // phone side sends 60-byte packets
      for (int i = 0; i < 5; ++i) ::send(B[0], p48.data(), 48, 0);           // vehicle side sends 48-byte packets
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      std::vector<int> toB, toA; uint8_t buf[256]; ssize_t n;
      while ((n = ::recv(B[0], buf, sizeof buf, MSG_DONTWAIT)) > 0) toB.push_back((int)n);
      while ((n = ::recv(A[0], buf, sizeof buf, MSG_DONTWAIT)) > 0) toA.push_back((int)n);
      CHK(st.rx_a == 60 && st.rx_b == 48 && st.mtu_a == 60 && st.mtu_b == 48, "auto: learned 60 from the phone side and 48 from the vehicle side");
      CHK(toB.size() == 5 && toB[0] == 48, "240 phone bytes -> 5 x 48-byte packets to the vehicle");
      CHK(toA.size() == 4 && toA[0] == 60, "240 vehicle bytes -> 4 x 60-byte packets to the phone");
      stop = true; t.join();
      { // SECOND session with the SAME stats object (as CallBridge does call after call): sizes must be learned again, not inherited or skipped
        int C[2], D[2]; CHK(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, C) == 0 && ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, D) == 0, "socketpairs 2");
        std::atomic<bool> stop2{false}; std::thread t2([&] { btcall::pumpAudio(C[1], D[1], stop2, st, 0, -1, -1); });
        std::vector<uint8_t> p60b(60, 3), p48b(48, 4);
        for (int i = 0; i < 4; ++i) ::send(C[0], p60b.data(), 60, 0);
        for (int i = 0; i < 5; ++i) ::send(D[0], p48b.data(), 48, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<int> toD, toC;
        while ((n = ::recv(D[0], buf, sizeof buf, MSG_DONTWAIT)) > 0) toD.push_back((int)n);
        while ((n = ::recv(C[0], buf, sizeof buf, MSG_DONTWAIT)) > 0) toC.push_back((int)n);
        CHK(st.rx_a == 60 && st.rx_b == 48 && st.mtu_a == 60 && st.mtu_b == 48 && toD.size() == 5 && toD[0] == 48 && toC.size() == 4 && toC[0] == 60, "second call with the same stats object: sizes learned again (48 to the vehicle, 60 to the phone)");
        stop2 = true; t2.join(); for (int f : {C[0], C[1], D[0], D[1]}) ::close(f);
      }
      for (int f : {A[0], A[1], B[0], B[1]}) ::close(f);
    }
    { int A[2], B[2]; socketpair(AF_UNIX, SOCK_SEQPACKET, 0, A); socketpair(AF_UNIX, SOCK_SEQPACKET, 0, B);
      std::atomic<bool> stop{false}; btcall::PumpStats st; const char* why = nullptr;
      std::thread t([&] { why = btcall::pumpAudio(A[1], B[1], stop, st, 0, 48, 64); });                   // to A: 48-byte packets, to B: 64-byte packets
      uint8_t pkt[64]; for (int i = 0; i < 64; ++i) pkt[i] = (uint8_t)i;
      for (int i = 0; i < 16; ++i) { ::send(A[0], pkt, 60, 0); ::send(B[0], pkt, 60, 0); }       // 16 x 60 = 960 bytes each way
      std::vector<size_t> sizesB, sizesA; long bytesB = 0, bytesA = 0; uint8_t rb[256];
      for (int i = 0; i < 400 && (bytesB < 960 || bytesA < 960); ++i) { pollfd p[2] = {{B[0], POLLIN, 0}, {A[0], POLLIN, 0}}; if (::poll(p, 2, 10) > 0) {
          if (p[0].revents & POLLIN) { ssize_t n = ::recv(B[0], rb, sizeof rb, 0); if (n > 0) { sizesB.push_back((size_t)n); bytesB += n; } }
          if (p[1].revents & POLLIN) { ssize_t n = ::recv(A[0], rb, sizeof rb, 0); if (n > 0) { sizesA.push_back((size_t)n); bytesA += n; } } } }
      bool allB = true; for (auto s : sizesB) allB &= (s == 64); bool allA = true; for (auto s : sizesA) allA &= (s == 48);
      CHK(sizesB.size() == 15 && allB && bytesB == 960, "A -> B: 16 x 60 bytes arrive as 15 x 64-byte packets (960 bytes, nothing lost)");
      CHK(sizesA.size() == 20 && allA && bytesA == 960, "B -> A: 16 x 60 bytes arrive as 20 x 48-byte packets");
      ::close(A[0]); std::this_thread::sleep_for(std::chrono::milliseconds(300)); stop = true; t.join();
      ::close(A[1]); ::close(B[0]); ::close(B[1]); }
    std::cout << (fails ? "HFP TEST FAILED" : "HFP ALL PASSED") << std::endl; return fails ? 1 : 0;
}
