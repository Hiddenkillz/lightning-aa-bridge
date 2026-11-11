// g++ -std=c++17 -pthread -Isrc tests/navrelay_test.cpp -o navrelay_test && ./navrelay_test
#include <thread>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include "NavRelay.h"
#include <vector>
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)
struct Got { uint16_t id; std::string body; };
int main() {
    NavRelay r(true); std::vector<Got> toPhone, toTruck;      // strict mode: the original ordering rules
    r.setDecoder([](uint16_t id, const std::string&) { return id == 0x8003 ? std::string("NavigationStatus{decoded}") : std::string(); });
    r.setPhoneSend([&](uint16_t id, const std::string& b) { toPhone.push_back({id, b}); });
    // 1. phone data before the vehicle asked is NOT forwarded
    r.fromPhone(0x8003, "x"); CHK(toTruck.empty() && r.droppedCount() == 1, "phone data before the vehicle's Start is dropped, not sent");
    // 2. vehicle Start before the phone opened its channel: nothing sent yet, replayed at open
    r.setTruckSend([&](uint16_t id, const std::string& b) { toTruck.push_back({id, b}); });
    r.fromTruck(0x8001, "S"); CHK(toPhone.empty(), "vehicle Start before the phone opened the channel is held back");
    r.phoneChannelOpen(); CHK(toPhone.size() == 1 && toPhone[0].id == 0x8001 && toPhone[0].body == "S", "...and replayed when the phone opens the channel (body intact)");
    // 3. now phone data flows raw to the vehicle
    r.fromPhone(0x8008, std::string("\x01\x02\x03", 3)); r.fromPhone(0x8003, "n");
    CHK(toTruck.size() == 2 && toTruck[0].id == 0x8008 && toTruck[0].body == std::string("\x01\x02\x03", 3), "phone messages reach the vehicle with id and payload untouched");
    // 4. vehicle Stop reaches the phone and gates further phone data
    r.fromTruck(0x8002, ""); CHK(toPhone.back().id == 0x8002, "vehicle Stop is forwarded to the phone");
    r.fromPhone(0x8003, "late"); CHK(toTruck.size() == 2, "after Stop the phone's data is not forwarded");
    // 5. vehicle restarts, then vanishes: phone gets a Stop
    r.fromTruck(0x8001, "S2"); size_t before = toPhone.size(); r.setTruckSend({}); CHK(toPhone.size() == before + 1 && toPhone.back().id == 0x8002, "vehicle session gone: phone is told Stop");
    r.fromPhone(0x8003, "y"); CHK(toTruck.size() == 2, "no vehicle session: nothing forwarded (and no crash)");
    // 6. phone gone: no sends to a dead phone
    r.setPhoneSend({}); size_t b2 = toPhone.size(); r.setTruckSend([&](uint16_t id, const std::string& b) { toTruck.push_back({id, b}); }); r.fromTruck(0x8001, "S3"); CHK(toPhone.size() == b2, "no phone session: vehicle messages are held, not sent");
    // 7. focus
    std::string fb; r.setTruckFocusSend([&](const std::string& b) { fb = b; }); r.focusFromPhone("F"); CHK(fb == "F", "phone's nav-focus request is carried to the vehicle");
    {   // tolerant mode (default): the vehicle never sends Start (what we suspect from the truck) and everything must still flow
        NavRelay q; std::vector<Got> tp, tt; q.setPhoneSend([&](uint16_t id, const std::string& b) { tp.push_back({id, b}); }); q.setTruckSend([&](uint16_t id, const std::string& b) { tt.push_back({id, b}); });
        q.phoneChannelOpen(); CHK(tp.size() == 1 && tp[0].id == 0x8001, "tolerant: the phone is sent a Start by us when the vehicle never sent one");
        q.fromPhone(0x8003, "n"); q.fromPhone(0x8008, "forecast"); CHK(tt.size() == 2 && tt[1].body == "forecast", "tolerant: phone data reaches the vehicle without waiting for the vehicle's Start");
        q.fromTruck(0x8001, "real"); CHK(tp.size() == 2 && tp[1].body == "real", "tolerant: a Start the vehicle sends later is still passed to the phone");
        NavRelay w; std::vector<Got> t2; w.setTruckSend([&](uint16_t id, const std::string& b) { t2.push_back({id, b}); }); w.fromPhone(0x8003, "x"); CHK(t2.size() == 1, "tolerant: phone data goes to the vehicle even before the phone's own channel-open event");
    }
    {   // battery preconditioning request: a synthetic charging stop on the vehicle's navigation channel
        NavRelay p; std::vector<Got> tt; std::vector<bool> states;
        p.setTruckSend([&](uint16_t id, const std::string& b) { tt.push_back({id, b}); });
        p.setPrecondStateCallback([&](bool on) { states.push_back(on); });
        NavRelay::PrecondCfg pc; pc.power_w = 325000; pc.eta_min = 15; pc.tz_min = -300; pc.period_s = 1; pc.name = "Battery preconditioning";
        p.setPrecondition(true, pc);
        CHK(tt.size() == 2 && tt[0].id == 0x8003 && tt[0].body == std::string("\x08\x01", 2) && tt[1].id == 0x8006, "ON: NavigationStatus ACTIVE then a NavigationState");
        // decode: field 2 (destination) -> field 1 name, field 2 block {1: eta, 2: 325000, 3: -1}
        const std::string& st = tt[1].body;
        CHK(st.size() > 20 && (uint8_t)st[0] == 0x12 && st.find("Battery preconditioning") != std::string::npos, "...the destination carries the name");
        const std::string blk_tag = std::string("\x12\x13", 2);     // field 2, 19 bytes: 08 <eta varint up to 3> 10 88 eb 13 18 ff..ff 01
        const size_t bp = st.find("\x10\x88\xeb\x13\x18\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01");
        CHK(bp != std::string::npos, "...and the charging-stop block: power 325000 W and -1, exactly as Google Maps sends it");
        { size_t i = bp; while (i > 0 && (uint8_t)st[i - 1] != 0x08) --i; uint64_t eta = 0; int sh = 0; for (size_t k = i; k < bp; ++k) { eta |= (uint64_t)((uint8_t)st[k] & 0x7f) << sh; sh += 7; }
          const std::time_t now = std::time(nullptr) - 300 * 60 + 15 * 60; const long want = (long)(now % 86400);
          CHK(std::labs((long)eta - want) <= 2 || std::labs((long)eta - want) >= 86398, "...ETA = local now + 15 min as seconds of day"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        CHK(tt.size() >= 3 && tt.back().id == 0x8006, "re-sent periodically while on");
        const size_t n_before = tt.size();
        p.fromPhone(0x8003, std::string("\x08\x01", 2));            // the phone starts navigating
        CHK(states.size() == 1 && !states[0] && !p.preconditionOn(), "phone navigation ACTIVE: the request is withdrawn and the UI is told OFF");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        CHK(tt.size() == n_before + 1 && tt.back().body == std::string("\x08\x01", 2), "...and nothing synthetic is sent any more (only the phone's own status went through)");
        p.setPrecondition(true, pc); CHK(!p.preconditionOn(), "ON while the phone navigates is refused");
        p.fromPhone(0x8003, std::string("\x08\x02", 2)); p.setPrecondition(true, pc); CHK(p.preconditionOn(), "after the phone stops navigating it can be turned on again");
        const size_t n2 = tt.size(); p.setPrecondition(false, pc);
        CHK(tt.size() == n2 + 2 && tt[n2].id == 0x8006 && tt[n2].body.empty() && tt[n2 + 1].id == 0x8003 && tt[n2 + 1].body == std::string("\x08\x02", 2), "OFF: empty state (destination gone) then NavigationStatus INACTIVE");
        p.stopPrecond();
        NavRelay r2; std::vector<Got> t3; r2.setTruckSend([&](uint16_t id, const std::string& b) { t3.push_back({id, b}); });
        NavRelay::PrecondCfg rc = pc; rc.mode = "replay"; rc.period_s = 1; r2.setPrecondition(true, rc);
        CHK(t3.size() == 3 && t3[0].id == 0x8003 && t3[1].id == 0x8006 && t3[1].body.size() == 69 && t3[2].id == 0x8007 && t3[2].body.size() == 36, "replay: ACTIVE, Maps' plain state (69 B) and position (36 B)");
        std::this_thread::sleep_for(std::chrono::milliseconds(3300));
        bool charger = false; for (auto& g : t3) if (g.id == 0x8006 && g.body.size() == 90 && g.body.find("\x10\x88\xeb\x13\x18", 0) != std::string::npos) charger = true;
        CHK(charger, "replay: from the 4th cycle the state carries the captured charging stop (90 B, 325000 W)");
        // the parts builder reproduces the capture's structure and sizes with the default variants (69 / 90 bytes)
        CHK(NavRelay::replayState(rc, false).size() == 69, "builder: plain state == 69 bytes like the capture");
        CHK(NavRelay::replayState(rc, true).size() == 90, "builder: charger state == 90 bytes like the capture");
        { NavRelay::PrecondCfg v = rc; v.replay_steps = false; const std::string s = NavRelay::replayState(v, true); CHK(s.size() == 42 && (uint8_t)s[0] == 0x12, "variant: no steps -> only the destination"); }
        { NavRelay::PrecondCfg v = rc; v.replay_name = "Battery preconditioning"; CHK(NavRelay::replayState(v, true).find("Battery preconditioning") != std::string::npos, "variant: renamed destination"); }
        { NavRelay::PrecondCfg v = rc; v.replay_field3 = false; CHK(NavRelay::replayState(v, true).size() == 90 - 11, "variant: no field 3 drops the 11-byte -1"); }
        { NavRelay::PrecondCfg v = rc; v.replay_field1 = 1; CHK(NavRelay::replayState(v, true).size() == 88, "variant: field1=1 (1-byte varint)"); }
        r2.setPrecondition(false, rc); r2.stopPrecond();
    }
    std::cout << (fails ? "NAVRELAY TEST FAILED" : "NAVRELAY TEST PASSED") << std::endl; return fails ? 1 : 0;
}
