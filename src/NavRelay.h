// NavRelay: the hub that carries the NAVIGATION channel between the two AA sessions (phone <-> vehicle).
//   vehicle (Sync 4, channel "navigation_status_service") --Start/Stop-->  Pi  --->  phone     (what the HU asks for)
//   phone --NavigationStatus / turn events / distance / state / position / energy forecast-->  Pi  --->  vehicle
// Messages are carried RAW (id + payload untouched), so whatever the phone sends today or in the future (e.g. the VehicleEnergyForecast that makes Ford precondition the
// battery for a DC fast charger) reaches the vehicle exactly as a direct connection would deliver it. Everything is also logged ([Nav] lines) because nobody has captured it.
// Rules: the phone is never sent anything before the vehicle has asked (InstrumentClusterStart 0x8001); the vehicle is never sent anything before it has asked.
// All senders are called OUTSIDE the lock (they take the sessions' own queue locks).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

class NavRelay {
public:
    // require_start=false (default): do not wait for the vehicle's Start. When the phone opens its navigation channel it is sent a Start right away if the vehicle has not sent one,
    // and phone data is forwarded as soon as the vehicle's navigation channel is open. require_start=true restores the strict order (nothing before the vehicle's Start).
    explicit NavRelay(bool require_start = false) : require_start_(require_start) {}
    using Send = std::function<void(uint16_t id, const std::string& body)>;
    using Decode = std::function<std::string(uint16_t id, const std::string& body)>;
    static constexpr uint16_t kStart = 0x8001, kStop = 0x8002;

    void setDecoder(Decode d) { std::lock_guard<std::mutex> l(m_); decode_ = std::move(d); }
    // --- vehicle side
    void setTruckSend(Send s) {                       // set when the vehicle's navigation channel is open; {} when the session is gone
        Send phone; bool tell = false;
        { std::lock_guard<std::mutex> l(m_); const bool had = (bool)truck_send_; truck_send_ = std::move(s);
          if (!truck_send_ && had) { tell = truck_started_ && phone_open_; truck_started_ = false; start_body_.clear(); phone = phone_send_; } }
        if (tell && phone) { log("vehicle gone -> Stop to phone"); phone(kStop, ""); }
    }
    void fromTruck(uint16_t id, const std::string& body) {
        Send phone; bool fwd = false;
        { std::lock_guard<std::mutex> l(m_);
          if (id == kStart) { truck_started_ = true; start_body_ = body; } else if (id == kStop) truck_started_ = false;
          fwd = phone_open_ && phone_send_; phone = phone_send_; ++from_truck_; }
        logMsg("vehicle->phone", id, body, fwd);
        if (fwd) phone(id, body);
    }
    // --- phone side
    void setPhoneSend(Send s) {                       // set while a phone session exists; {} when it ends
        std::lock_guard<std::mutex> l(m_); phone_send_ = std::move(s); if (!phone_send_) phone_open_ = false;
    }
    void phoneChannelOpen() {                         // the phone opened our navigation channel: if the vehicle already asked for data, tell the phone now
        Send phone; bool replay = false; std::string body;
        bool synthetic = false;
        { std::lock_guard<std::mutex> l(m_); phone_open_ = true; phone = phone_send_; body = start_body_; replay = phone_send_ && (truck_started_ || !require_start_); synthetic = replay && !truck_started_; }
        log(std::string("phone opened the navigation channel") + (replay ? (synthetic ? " -> the vehicle has not sent Start: sending the phone a Start ourselves" : " -> replaying the vehicle's Start") : " (waiting for the vehicle's Start)"));
        if (replay) phone(kStart, body);
    }
    void fromPhone(uint16_t id, const std::string& body) {
        Send truck; bool fwd = false; bool withdraw = false; std::function<void(bool)> tell;
        { std::lock_guard<std::mutex> l(m_); fwd = (truck_started_ || !require_start_) && truck_send_; truck = truck_send_; ++from_phone_; if (!fwd) ++dropped_;
          if (id == 0x8003 && body.size() >= 2 && body[0] == 0x08) { phone_nav_active_ = (body[1] == 0x01 || body[1] == 0x03);      // NavigationStatus ACTIVE / REROUTING
              if (phone_nav_active_ && precond_on_) { precond_on_ = false; withdraw = true; tell = precond_state_; } } }
        if (withdraw) { log("phone navigation started: the preconditioning request is withdrawn (the phone's own route takes over)"); if (tell) tell(false); }
        logMsg("phone->vehicle", id, body, fwd);
        if (fwd) truck(id, body);
    }

    // ---- battery preconditioning request (the Debug/strip toggle) ---------------------------------------------------------------------------------
    // Evidence (drive ui46a, 2026-10-20): the Lightning showed its preconditioning icon while Google Maps routed via a DC fast charger; the only thing on the
    // wire was the NavigationState destination list, whose charging-stop entry carries an undocumented sub-message: field 2 { 1: ETA (seconds of day),
    // 2: 325000 (charger power, W), 3: -1 }. Ordinary destinations never carry it. So the request = NavigationStatus ACTIVE + a NavigationState whose single
    // destination carries that block, re-sent every few seconds like Maps does; OFF = a state without destinations + NavigationStatus INACTIVE.
    struct PrecondCfg { int power_w = 325000, eta_min = 15, tz_min = -300, period_s = 2; std::string name = "Battery preconditioning"; std::string mode = "synth";
                        bool replay_pos = true, replay_steps = true, replay_field3 = true; std::string replay_name, replay_road; long replay_field1 = -1; int replay_plain = 3; };
    // Captured on the ui47a drive (2026-10-20, a 4.5 mi Maps route to a DC fast charger): Maps sent 4 states without the charging block, then with it; one
    // NavigationCurrentPosition per state (distance + ETA only, no coordinates). The cluster showed the preconditioning icon on that route. Replayed with the same
    // structure and sizes; the road / destination TEXT comes from config (precond_replay_road / precond_replay_name) with neutral defaults of the captured lengths.
    static constexpr const char* kReplayPosition = "0a070a05120130180612190a0a089d381203342e351805120831313a313220504d18e703";
    static constexpr const char* kDefaultRoad = "toward Charger Rd";      // 17 chars, like the capture
    static constexpr const char* kDefaultName = "DC Fast Charger 1";      // 17 chars, like the capture
    // The message built from parts, so single pieces can be varied for the bisect.
    static std::string replayState(const PrecondCfg& pc, bool charger) {
        std::string st;
        const std::string roadName = pc.replay_road.empty() ? std::string(kDefaultRoad) : pc.replay_road;
        if (pc.replay_steps) {   // steps { maneuver { type: DEPART } road { name: "toward …" } cue { alternate_text: "toward …" } }
            std::string man; putVarint(man, (1 << 3) | 0); putVarint(man, 1);
            std::string road; putField(road, 1, roadName);
            std::string cue; putField(cue, 1, roadName);
            std::string step; putField(step, 1, man); putField(step, 2, road); putField(step, 4, cue);
            putField(st, 1, step);
        }
        std::string dst; putField(dst, 1, pc.replay_name.empty() ? std::string(kDefaultName) : pc.replay_name);
        if (charger) {
            std::string blk; putVarint(blk, (1 << 3) | 0); putVarint(blk, (uint64_t)(pc.replay_field1 < 0 ? 100709 : pc.replay_field1));
            putVarint(blk, (2 << 3) | 0); putVarint(blk, (uint64_t)pc.power_w);
            if (pc.replay_field3) { putVarint(blk, (3 << 3) | 0); putVarint(blk, (uint64_t)-1); }
            putField(dst, 2, blk);
        }
        putField(st, 2, dst);
        return st;
    }
    static std::string unhex(const char* h) { std::string o; for (size_t i = 0; h[i] && h[i + 1]; i += 2) o.push_back((char)std::stoi(std::string(h + i, 2), nullptr, 16)); return o; }
    void setPrecondStateCallback(std::function<void(bool)> f) { std::lock_guard<std::mutex> l(m_); precond_state_ = std::move(f); }
    void setPrecondition(bool on, const PrecondCfg& pc) {
        bool start = false;
        { std::lock_guard<std::mutex> l(m_); pcfg_ = pc;
          if (on && phone_nav_active_) { log("preconditioning request refused: the phone is navigating (its route decides)"); on = false; }
          if (on == precond_on_) { if (!on) return; }
          precond_on_ = on; precond_sent_ = 0; start = on && !precond_thread_.joinable(); }
        if (on) { log(pc.mode == "replay" ? "preconditioning request ON (mode replay): re-sending Google Maps' captured charger route every " + std::to_string(pc.period_s) + " s" : "preconditioning request ON: Sync is told about a charging stop '" + pc.name + "' (" + std::to_string(pc.power_w / 1000) + " kW, ETA now+" + std::to_string(pc.eta_min) + " min) every " + std::to_string(pc.period_s) + " s"); }
        else { log("preconditioning request OFF: destination removed, navigation INACTIVE"); sendPrecond(false); }
        if (start) { precond_run_ = true; precond_thread_ = std::thread([this] { precondLoop(); }); }
        if (on) sendPrecond(true);
    }
    void stopPrecond() { precond_run_ = false; if (precond_thread_.joinable()) precond_thread_.join(); }
    ~NavRelay() { stopPrecond(); }
    bool preconditionOn() const { std::lock_guard<std::mutex> l(m_); return precond_on_; }

public:
    static void putVarint(std::string& o, uint64_t v) { do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; o.push_back((char)b); } while (v); }
    static void putField(std::string& o, int num, const std::string& v) { putVarint(o, (uint64_t)(num << 3) | 2); putVarint(o, v.size()); o += v; }
    std::string precondState(bool on, int& eta) const {              // NavigationState{ destinations { address: name  2 { 1: eta  2: power  3: -1 } } }  (empty when off)
        std::string st;
        if (on) {
            const std::time_t t = std::time(nullptr) + (std::time_t)pcfg_.tz_min * 60 + (std::time_t)pcfg_.eta_min * 60;
            eta = (int)(t % 86400);
            std::string blk; putVarint(blk, (1 << 3) | 0); putVarint(blk, (uint64_t)eta); putVarint(blk, (2 << 3) | 0); putVarint(blk, (uint64_t)pcfg_.power_w); putVarint(blk, (3 << 3) | 0); putVarint(blk, (uint64_t)-1);
            std::string dst; putField(dst, 1, pcfg_.name); putField(dst, 2, blk);
            putField(st, 2, dst);
        }
        return st;
    }
    void sendPrecond(bool on) {
        Send truck; std::string st; int eta = 0; long n; bool replay;
        { std::lock_guard<std::mutex> l(m_); truck = truck_send_; if (!truck) { if (on) log("preconditioning: the vehicle's navigation channel is not open yet; will send when it is"); return; } replay = pcfg_.mode == "replay"; st = replay ? std::string() : precondState(on, eta); n = ++precond_sent_; }
        if (on && replay) {   // Maps' own sequence, byte for byte: status ACTIVE, 3 plain states, then the state with the charging stop; a position message after every state
            if (n == 1) truck(0x8003, std::string("\x08\x01", 2));
            PrecondCfg pc; { std::lock_guard<std::mutex> l(m_); pc = pcfg_; }
            const bool charger = n > pc.replay_plain; const std::string st2 = replayState(pc, charger);
            truck(0x8006, st2); if (pc.replay_pos) truck(0x8007, unhex(kReplayPosition));
            if (n == 1 || n == pc.replay_plain + 1 || n % 30 == 0) log(std::string("preconditioning (replay of the ui47a Maps capture): sent NavigationState ") + (charger ? "WITH the charging stop" : "without the charging stop yet") + (pc.replay_pos ? " + CurrentPosition" : " (no CurrentPosition)") + (pc.replay_steps ? "" : ", no steps") + (pc.replay_name.empty() ? "" : ", renamed '" + pc.replay_name + "'") + (pc.replay_field1 < 0 ? "" : ", field1=" + std::to_string(pc.replay_field1)) + (pc.replay_field3 ? "" : ", no field3") + ", #" + std::to_string(n) + (n == pc.replay_plain + 1 ? " raw=" + hex(st2, 200) : ""));
            return; }
        if (on) { if (n == 1) truck(0x8003, std::string("\x08\x01", 2)); truck(0x8006, st); if (n == 1 || n % 30 == 0) log("preconditioning: sent NavigationState with the charging stop (ETA " + std::to_string(eta / 3600) + ":" + (eta % 3600 / 60 < 10 ? "0" : "") + std::to_string(eta % 3600 / 60) + " local), #" + std::to_string(n) + " raw=" + hex(st, 200)); }
        else { truck(0x8006, st); truck(0x8003, std::string("\x08\x02", 2)); }
    }
    void precondLoop() {
        while (precond_run_) {
            int period; bool on; { std::lock_guard<std::mutex> l(m_); on = precond_on_; period = pcfg_.period_s; }
            for (int i = 0; i < std::max(1, period) * 10 && precond_run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            { std::lock_guard<std::mutex> l(m_); on = precond_on_; }
            if (on && precond_run_) sendPrecond(true);     // setPrecondition() already sent the first one; the loop re-sends every period
        }
    }
    bool phone_nav_active_ = false, precond_on_ = false; long precond_sent_ = 0; PrecondCfg pcfg_; std::function<void(bool)> precond_state_;
    std::atomic<bool> precond_run_{false}; std::thread precond_thread_;
public:
    // navigation FOCUS requests live on the control channel (phone asks the head unit for nav focus): carried raw as well
    using FocusSend = std::function<void(const std::string& body)>;
    void setTruckFocusSend(FocusSend f) { std::lock_guard<std::mutex> l(m_); focus_send_ = std::move(f); }
    void focusFromPhone(const std::string& body) { FocusSend f; { std::lock_guard<std::mutex> l(m_); f = focus_send_; } log("phone nav-focus request -> vehicle" + std::string(f ? "" : " (vehicle session not ready, dropped)")); if (f) f(body); }

    long fromPhoneCount() const { std::lock_guard<std::mutex> l(m_); return from_phone_; }
    long fromTruckCount() const { std::lock_guard<std::mutex> l(m_); return from_truck_; }
    long droppedCount() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
private:
    static std::string hex(const std::string& b, size_t max) {
        static const char* d = "0123456789abcdef"; std::string o; for (size_t i = 0; i < b.size() && i < max; ++i) { o += d[(uint8_t)b[i] >> 4]; o += d[(uint8_t)b[i] & 15]; }
        if (b.size() > max) o += "..."; return o; }
    void log(const std::string& s) { std::cout << "[Nav] " << s << std::endl; }
    void logMsg(const char* dir, uint16_t id, const std::string& body, bool forwarded) {
        // Budget applies to the chatty routine messages only (status / turn / distance / state / position). Everything else - Start/Stop, the EV energy
        // forecast (0x8008) and any id we do not know - is logged unconditionally: those are the rare ones a preconditioning trigger would hide in.
        const bool routine = id >= 0x8003 && id <= 0x8007;
        Decode dec; { std::lock_guard<std::mutex> l(m_); dec = decode_; if (routine) { if (logged_ >= kLogBudget) { if (logged_ == kLogBudget) { ++logged_; log("routine navigation messages: log budget reached (rare messages are still logged)"); } return; } ++logged_; } }
        std::ostringstream o; o << dir << " id=0x" << std::hex << id << std::dec << " len=" << body.size() << (forwarded ? "" : " (NOT forwarded)");
        if (dec) { std::string t = dec(id, body); if (!t.empty()) o << " " << (t.size() > 600 ? t.substr(0, 600) + "..." : t); }
        o << " raw=" << hex(body, routine ? 160 : 4000);                         // rare messages (incl. 0x8008 EV energy forecast) in full: they are what we are after
        log(o.str());
    }
    static constexpr long kLogBudget = 4000;           // lines per process: a long navigation must not fill the card
    mutable std::mutex m_;
    Send truck_send_, phone_send_; FocusSend focus_send_; Decode decode_;
    bool require_start_ = false;
    bool truck_started_ = false, phone_open_ = false; std::string start_body_;
    long from_phone_ = 0, from_truck_ = 0, dropped_ = 0, logged_ = 0;
};
