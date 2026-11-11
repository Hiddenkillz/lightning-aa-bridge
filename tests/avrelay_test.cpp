// g++ -std=c++17 -pthread -Isrc tests/avrelay_test.cpp -o avrelay_test && ./avrelay_test
#include "AvRelay.h"
#include <vector>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)
int main() {
    AvRelay r; int starts = 0, stops = 0, last_stream = -1; std::string pcm; bool micOpen = false; std::vector<std::pair<uint16_t, std::string>> toTruck, toPhone; std::string micGot;
    r.phoneAudioData(0, 1, (const uint8_t*)"x", 1); CHK(r.bytesIn(0) == 1, "audio with no vehicle session is counted and dropped without crashing");
    r.setTruck([&](int s, int) { ++starts; last_stream = s; }, [&](int) { ++stops; }, [&](int, uint64_t, const uint8_t* p, size_t n) { pcm.append((const char*)p, n); }, [&](bool o) { micOpen = o; },
               [&](uint16_t id, const std::string& b) { toTruck.push_back({id, b}); });
    r.setPhone([&](uint64_t, const uint8_t* p, size_t n) { micGot.append((const char*)p, n); }, [&](uint16_t id, const std::string& b) { toPhone.push_back({id, b}); });
    r.phoneAudioStart(1, 7); CHK(starts == 1 && last_stream == 1, "start reaches the vehicle with its stream");
    r.phoneAudioData(1, 5, (const uint8_t*)"abcd", 4); r.phoneAudioData(1, 6, (const uint8_t*)"efgh", 4); CHK(pcm == "abcdefgh", "PCM is carried unchanged and in order");
    r.phoneAudioStop(1); CHK(stops == 1, "stop reaches the vehicle");
    r.phoneMicRequest(true); CHK(micOpen, "mic open request reaches the vehicle"); r.phoneMicRequest(false); CHK(!micOpen, "mic close request reaches the vehicle");
    r.micDataFromTruck(9, (const uint8_t*)"mic1", 4); r.micDataFromTruck(10, (const uint8_t*)"mic2", 4); CHK(micGot == "mic1mic2" && r.micBytes() == 8, "microphone PCM flows vehicle -> phone");
    r.controlFromPhone(18, "F"); r.controlFromPhone(23, "B"); r.controlFromPhone(24, "C"); r.controlFromPhone(7, "no"); CHK(toTruck.size() == 3 && toTruck[0].first == 18 && toTruck[1].first == 23 && toTruck[2].first == 24, "focus request / battery / call availability go to the vehicle, nothing else");
    r.controlFromTruck(17, "V"); r.controlFromTruck(19, "N"); r.controlFromTruck(11, "ping"); CHK(toPhone.size() == 2 && toPhone[0].first == 17 && toPhone[1].first == 19, "voice + audio-focus notifications go to the phone, nothing else");
    // a NEW vehicle session while the phone is streaming MEDIA (stream 0 stopped earlier, 1 stopped) and the mic is closed: nothing to replay
    { int rs = 0; AvRelay q; q.phoneAudioStart(1, 42); q.phoneMicRequest(true); int gs = -1, gsess = -1; bool gm = false;
      q.setTruck([&](int s, int se) { ++rs; gs = s; gsess = se; }, [](int) {}, [](int, uint64_t, const uint8_t*, size_t) {}, [&](bool o) { gm = o; }, [](uint16_t, const std::string&) {});
      CHK(rs == 1 && gs == 1 && gsess == 42 && gm, "a new vehicle session is told about the stream the phone is already playing, and the open microphone"); }
    r.clearTruck(); r.phoneAudioStart(0, 1); r.controlFromPhone(23, "B"); CHK(starts == 1 && toTruck.size() == 3, "vehicle session gone: nothing forwarded, no crash");
    r.clearPhone(); r.micDataFromTruck(1, (const uint8_t*)"z", 1); r.controlFromTruck(17, "V"); CHK(toPhone.size() == 2, "phone session gone: nothing forwarded, no crash");
    {   // audio focus request survives a vehicle session change (ui51a)
        AvRelay q; std::vector<std::pair<uint16_t, std::string>> tt;
        q.controlFromPhone(18, std::string("\x08\x01", 2));                                        // GAIN with no vehicle session: remembered
        CHK(q.focusRequest() == std::string("\x08\x01", 2), "focus GAIN with no vehicle session is remembered");
        q.setTruck([](int, int) {}, [](int) {}, [](int, uint64_t, const uint8_t*, size_t) {}, [](bool) {}, [&](uint16_t id, const std::string& b) { tt.push_back({id, b}); });
        CHK(tt.size() == 1 && tt[0].first == 18 && tt[0].second == std::string("\x08\x01", 2), "...and sent to the vehicle session that appears");
        q.clearTruck(); tt.clear();
        q.setTruck([](int, int) {}, [](int) {}, [](int, uint64_t, const uint8_t*, size_t) {}, [](bool) {}, [&](uint16_t id, const std::string& b) { tt.push_back({id, b}); });
        CHK(tt.size() == 1 && tt[0].first == 18, "replayed again to the next vehicle session (USB re-attach)");
        q.controlFromPhone(18, std::string("\x08\x04", 2)); tt.clear(); q.clearTruck();                 // RELEASE: forgotten
        q.setTruck([](int, int) {}, [](int) {}, [](int, uint64_t, const uint8_t*, size_t) {}, [](bool) {}, [&](uint16_t id, const std::string& b) { tt.push_back({id, b}); });
        CHK(tt.empty() && q.focusRequest().empty(), "after RELEASE nothing is replayed");
    }
    std::cout << (fails ? "AVRELAY TEST FAILED" : "AVRELAY TEST PASSED") << std::endl; return fails ? 1 : 0;
}
