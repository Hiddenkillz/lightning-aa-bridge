#pragma once
// Bluetooth Hands-Free (HFP 1.7, CVSD only) call-control logic for the call bridge. Pure text/state code: no sockets, no BlueZ, fully unit-testable.
//
//   Pixel (Audio Gateway) --RFCOMM--> HfClient     (we are the "car kit" toward the phone)
//   Sync 4 (Hands-Free)   --RFCOMM--> AgServer     (we are the "phone" toward the vehicle)
//   CallCore joins the two: indicator changes, caller id and ringing flow phone -> vehicle, answer / hang up / dial flow vehicle -> phone.
//
// Deliberately NOT claimed anywhere: codec negotiation (so audio is always 8 kHz CVSD and can be copied between the two SCO links untouched),
// 3-way calling, in-band ring tone (the vehicle plays its own ring).
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hfp {

inline std::vector<std::string> takeLines(std::string& buf, const char* d, size_t n) {   // AT lines end in CR and/or LF; empty lines are dropped
    buf.append(d, n); std::vector<std::string> out; std::string cur;
    size_t i = 0;
    for (; i < buf.size(); ++i) { char c = buf[i]; if (c == '\r' || c == '\n') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    buf = cur;                       // unfinished tail stays buffered
    if (buf.size() > 4096) buf.clear();   // garbage guard
    return out;
}
inline std::string trim(const std::string& s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
inline bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
inline std::vector<std::string> splitCsv(const std::string& s) {          // splits on commas outside quotes and parentheses
    std::vector<std::string> out; std::string cur; int depth = 0; bool q = false;
    for (char c : s) { if (c == '"') q = !q; if (!q) { if (c == '(') ++depth; if (c == ')') --depth; }
        if (c == ',' && !q && depth == 0) { out.push_back(trim(cur)); cur.clear(); } else cur += c; }
    out.push_back(trim(cur)); return out;
}
inline std::string unquote(const std::string& s) { return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s; }
inline int toInt(const std::string& s, int dflt = 0) { try { return std::stoi(s); } catch (...) { return dflt; } }

// ---------------------------------------------------------------------------------------------------------------------------------------
// toward the PHONE: we are the Hands-Free unit
// ---------------------------------------------------------------------------------------------------------------------------------------
class HfClient {
public:
    std::function<void(const std::string&)> send;                              // raw bytes to the phone
    std::function<void(const std::string&)> log;
    std::function<void()> onReady;                                             // service level connection established
    std::function<void(const std::string& name, int value)> onIndicator;       // initial values and every +CIEV
    std::function<void(const std::string& number, int type)> onClip;
    std::function<void()> onRing;
    std::function<void(const std::string& name)> onNetworkName;                // +COPS operator
    std::function<void(const std::string& number, int type, int dir, int status)> onClcc;   // one +CLCC line (dir 0 = outgoing, 1 = incoming)

    bool ready() const { return ready_; }
    void start() {                                                              // phone side already connected: begin the SLC handshake
        ready_ = false; script_ = {"AT+BRSF=36", "AT+CIND=?", "AT+CIND?", "AT+CMER=3,0,0,1", "AT+CLIP=1", "AT+CCWA=1", "AT+CMEE=1", "AT+COPS=3,0", "AT+COPS?"};
        crit_ = 4;                                                              // the first four must succeed
        step_ = 0; q_.clear(); busy_ = false; names_.clear(); sendNextScript();
    }
    void command(const std::string& c) { if (!ready_) { note("command '" + c + "' ignored: phone link not ready"); return; } q_.push_back(c); pump(); }
    void answer() { command("ATA"); }
    void hangup() { command("AT+CHUP"); }
    void dial(const std::string& n) { command("ATD" + n + ";"); }
    void redial() { command("AT+BLDN"); }
    void dtmf(char c) { command(std::string("AT+VTS=") + c); }
    void voice(bool on) { command(on ? "AT+BVRA=1" : "AT+BVRA=0"); }
    void queryCalls() { command("AT+CLCC"); }
    void feed(const char* d, size_t n) { for (auto& l : takeLines(rx_, d, n)) line(l); }
    int agFeatures() const { return ag_brsf_; }

private:
    void note(const std::string& s) { if (log) log(s); }
    void emit(const std::string& c) { cur_ = c; busy_ = true; note("-> phone: " + c); if (send) send(c + "\r"); }
    void sendNextScript() { if (step_ < (int)script_.size()) { emit(script_[step_]); } }
    void pump() { if (busy_ || !ready_ || q_.empty()) return; std::string c = q_.front(); q_.pop_front(); emit(c); }
    void indicator(int idx1, int v) { if (idx1 >= 1 && idx1 <= (int)names_.size() && onIndicator) onIndicator(names_[idx1 - 1], v); }
    void done(bool ok) {                                                         // the outstanding command was answered
        busy_ = false;
        if (step_ < (int)script_.size()) {                                       // still in the handshake
            if (!ok && step_ < crit_) { note("handshake failed at '" + script_[step_] + "'"); script_.clear(); return; }
            ++step_;
            if (step_ == crit_ && !ready_) { ready_ = true; note("service level connection up"); if (onReady) onReady(); }
            sendNextScript();
            if (step_ < (int)script_.size()) return;                           // script still running: user commands wait their turn
        }
        pump();                                                                // script just finished (or idle): send what was queued meanwhile (a dial during the handshake tail was lost before)
    }
    void line(const std::string& l0) {
        const std::string l = trim(l0);
        note("<- phone: " + l);
        if (l == "OK") { done(true); return; }
        if (l == "ERROR" || startsWith(l.c_str(), "+CME ERROR") || l == "NO CARRIER" || l == "BUSY" || l == "NO ANSWER") { done(false); return; }
        if (l == "RING") { if (onRing) onRing(); return; }
        if (startsWith(l.c_str(), "+BRSF:")) { ag_brsf_ = toInt(trim(l.substr(6))); return; }
        if (startsWith(l.c_str(), "+CIEV:")) { auto p = splitCsv(l.substr(6)); if (p.size() >= 2) indicator(toInt(p[0]), toInt(p[1])); return; }
        if (startsWith(l.c_str(), "+CLIP:") || startsWith(l.c_str(), "+CCWA:")) { auto p = splitCsv(l.substr(6)); if (!p.empty() && onClip) onClip(unquote(p[0]), p.size() > 1 ? toInt(p[1], 129) : 129); return; }
        if (startsWith(l.c_str(), "+COPS:")) { auto p = splitCsv(l.substr(6)); if (p.size() >= 3 && onNetworkName) onNetworkName(unquote(p[2])); return; }
        if (startsWith(l.c_str(), "+CLCC:")) { auto p = splitCsv(l.substr(6)); if (p.size() >= 6 && onClcc) onClcc(unquote(p[5]), p.size() > 6 ? toInt(p[6], 129) : 129, toInt(p[1]), toInt(p[2])); return; }
        if (startsWith(l.c_str(), "+CIND:")) {
            const std::string body = trim(l.substr(6));
            if (cur_ == "AT+CIND=?") {                                          // ("service",(0,1)),("call",(0,1)),...
                names_.clear();
                for (auto& item : splitCsv(body)) { if (item.empty() || item[0] != '(') continue; auto in = splitCsv(item.substr(1, item.find(',') == std::string::npos ? 0 : item.size() - 2));
                    if (!in.empty()) { std::string nm = unquote(in[0]); if (nm == "call_setup") nm = "callsetup"; if (nm == "call_held") nm = "callheld"; names_.push_back(nm); } }
                note("phone indicators: " + std::to_string(names_.size()));
            } else if (cur_ == "AT+CIND?") {
                auto v = splitCsv(body); for (size_t i = 0; i < v.size(); ++i) indicator((int)i + 1, toInt(v[i]));
            }
            return;
        }
    }
    std::string rx_, cur_; std::vector<std::string> script_, names_; std::deque<std::string> q_; int step_ = 0, crit_ = 4, ag_brsf_ = 0; bool busy_ = false, ready_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// toward the VEHICLE: we are the Audio Gateway
// ---------------------------------------------------------------------------------------------------------------------------------------
struct AgIndicators { int service = 0, call = 0, callsetup = 0, callheld = 0, signal = 0, roam = 0, battchg = 0; };

class AgServer {
public:
    std::function<void(const std::string&)> send;
    std::function<void(const std::string&)> log;
    std::function<void()> onReady;                                  // vehicle finished its handshake (AT+CMER)
    std::function<void()> onAnswer, onHangup, onRedial;
    std::function<void(const std::string& number)> onDial;
    std::function<void(char)> onDtmf;
    std::function<void(bool)> onVoice;
    std::function<void(const std::string&)> onOther;                // anything else worth knowing about (logged by the owner)

    AgIndicators ind;                                                // current state, read by AT+CIND?
    std::string operatorName = "AA-Bridge", ownNumber, number; int numberType = 129;   // number/numberType = current caller id
    std::string manufacturer = "Android", model = "Android Phone", revision = "1.0";  // AT+CGMI/CGMM/CGMR (Sync 4 asks)
    static constexpr int kBrsf = 0x60;                               // reject call (5) + enhanced call status (6)

    void feed(const char* d, size_t n) { for (auto& l : takeLines(rx_, d, n)) line(l); }
    bool ready() const { return events_ && slc_; }
    bool clipEnabled() const { return clip_; }
    int hfFeatures() const { return hf_brsf_; }

    void setIndicator(const std::string& name, int v) {
        int* p = ptr(name); if (!p) return; if (*p == v) return; *p = v;
        if (events_) { const int idx = indexOf(name); if (idx > 0) resp("+CIEV: " + std::to_string(idx) + "," + std::to_string(v)); }
    }
    void sendRing() { resp("RING"); if (clip_ && !number.empty()) resp("+CLIP: \"" + number + "\"," + std::to_string(numberType)); }
    void reset() { slc_ = false; events_ = false; clip_ = false; rx_.clear(); }

private:
    void note(const std::string& s) { if (log) log(s); }
    void raw(const std::string& s) { if (send) send(s); }
    void resp(const std::string& s) { note("-> vehicle: " + s); raw("\r\n" + s + "\r\n"); }
    void ok() { note("-> vehicle: OK"); raw("\r\nOK\r\n"); }
    void err() { note("-> vehicle: ERROR"); raw("\r\nERROR\r\n"); }
    int* ptr(const std::string& n) { if (n == "service") return &ind.service; if (n == "call") return &ind.call; if (n == "callsetup") return &ind.callsetup; if (n == "callheld") return &ind.callheld;
        if (n == "signal") return &ind.signal; if (n == "roam") return &ind.roam; if (n == "battchg") return &ind.battchg; return nullptr; }
    static int indexOf(const std::string& n) { static const char* o[] = {"service", "call", "callsetup", "callheld", "signal", "roam", "battchg"}; for (int i = 0; i < 7; ++i) if (n == o[i]) return i + 1; return 0; }
    void clcc() {
        // synthesize the one call we know about from the indicators
        const char* dir = ind.callsetup == 1 ? "1" : "0"; int st = -1;
        if (ind.callsetup == 1) st = 4; else if (ind.callsetup == 2) st = 2; else if (ind.callsetup == 3) st = 3; else if (ind.call == 1) st = 0;
        if (st >= 0) resp(std::string("+CLCC: 1,") + dir + "," + std::to_string(st) + ",0,0,\"" + number + "\"," + std::to_string(numberType));
    }
    void line(const std::string& l0) {
        const std::string l = trim(l0);
        note("<- vehicle: " + l);
        if (!startsWith(l.c_str(), "AT") && !startsWith(l.c_str(), "at")) { note("ignoring non-AT line"); return; }
        std::string u; for (char c : l) u += (char)toupper((unsigned char)c);
        if (startsWith(u.c_str(), "AT+BRSF=")) { hf_brsf_ = toInt(u.substr(8)); resp("+BRSF: " + std::to_string(kBrsf)); ok(); return; }
        if (u == "AT+CIND=?") { resp("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0,3)),(\"callheld\",(0,2)),(\"signal\",(0,5)),(\"roam\",(0,1)),(\"battchg\",(0,5))"); ok(); return; }
        if (u == "AT+CIND?") { resp("+CIND: " + std::to_string(ind.service) + "," + std::to_string(ind.call) + "," + std::to_string(ind.callsetup) + "," + std::to_string(ind.callheld) + ","
                                + std::to_string(ind.signal) + "," + std::to_string(ind.roam) + "," + std::to_string(ind.battchg)); ok(); return; }
        if (startsWith(u.c_str(), "AT+CMER=")) { auto p = splitCsv(u.substr(8)); events_ = p.size() >= 4 && toInt(p[0]) == 3 && toInt(p[3]) == 1; ok();
            if (events_ && !slc_) { slc_ = true; note("service level connection up"); if (onReady) onReady(); } return; }
        if (u == "AT+CHLD=?") { err(); return; }                                     // no 3-way calling claimed
        if (startsWith(u.c_str(), "AT+CHLD=")) { err(); return; }
        if (startsWith(u.c_str(), "AT+CLIP=")) { clip_ = toInt(u.substr(8)) == 1; ok(); return; }
        if (startsWith(u.c_str(), "AT+CCWA=") || startsWith(u.c_str(), "AT+CMEE=") || startsWith(u.c_str(), "AT+NREC=") || startsWith(u.c_str(), "AT+VGS=") || startsWith(u.c_str(), "AT+VGM=")
            || startsWith(u.c_str(), "AT+BIA=") || startsWith(u.c_str(), "AT+BIND=") || startsWith(u.c_str(), "AT+COPS=") || startsWith(u.c_str(), "AT+BTRH")) { ok(); return; }
        if (u == "AT+COPS?") { resp("+COPS: 0,0,\"" + operatorName + "\""); ok(); return; }
        if (u == "AT+CGMI") { resp("+CGMI: " + manufacturer); ok(); return; }
        if (u == "AT+CGMM") { resp("+CGMM: " + model); ok(); return; }
        if (u == "AT+CGMR") { resp("+CGMR: " + revision); ok(); return; }
        if (u == "AT+CSCS?") { resp("+CSCS: \"UTF-8\""); ok(); return; }
        if (u == "AT+CSCS=?") { resp("+CSCS: (\"UTF-8\",\"IRA\")"); ok(); return; }
        if (startsWith(u.c_str(), "AT+CSCS=")) { ok(); return; }
        if (u == "AT+CNUM") { if (!ownNumber.empty()) resp("+CNUM: ,\"" + ownNumber + "\",129,,4"); ok(); return; }
        if (u == "AT+CLCC") { clcc(); ok(); return; }
        if (u == "ATA") { ok(); if (onAnswer) onAnswer(); return; }
        if (u == "AT+CHUP") { ok(); if (onHangup) onHangup(); return; }
        if (u == "AT+BLDN") { ok(); if (onRedial) onRedial(); return; }
        if (startsWith(u.c_str(), "ATD")) { std::string n = trim(l.substr(3)); while (!n.empty() && (n.back() == ';' || n.back() == ' ')) n.pop_back(); ok(); if (onDial) onDial(n); return; }
        if (startsWith(u.c_str(), "AT+VTS=")) { std::string t = trim(u.substr(7)); ok(); if (!t.empty() && onDtmf) onDtmf(t[0]); return; }
        if (startsWith(u.c_str(), "AT+BVRA=")) { ok(); if (onVoice) onVoice(toInt(u.substr(8)) == 1); return; }
        if (onOther) onOther(l);
        err();                                                                         // unknown (Apple XAPL / IPHONEACCEV / codec commands ...): refuse politely
    }
    std::string rx_; bool slc_ = false, events_ = false, clip_ = false; int hf_brsf_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// the join
// ---------------------------------------------------------------------------------------------------------------------------------------
class CallCore {
public:
    HfClient phone; AgServer vehicle;
    std::function<void(bool active)> onCallAudio;           // a call needs audio (incoming answered / outgoing / active) or ended
    std::function<void(const std::string&)> log;

    CallCore() {
        phone.log = [this](const std::string& s) { note("[phone] " + s); };
        vehicle.log = [this](const std::string& s) { note("[vehicle] " + s); };
        phone.onIndicator = [this](const std::string& n, int v) {
            vehicle.setIndicator(n, v);
            // a call dialled on the phone (or already active when we linked) carries no +CLIP: ask the phone's call list for the number
            if ((n == "callsetup" && (v == 2 || v == 3)) || (n == "call" && v == 1)) { if (vehicle.number.empty() && !clcc_asked_) { clcc_asked_ = true; phone.queryCalls(); } }
            if ((n == "callsetup" && v == 0 && vehicle.ind.call == 0) || (n == "call" && v == 0 && vehicle.ind.callsetup == 0)) clcc_asked_ = false;
            if (n == "callsetup") { ringing_ = (v == 1); if (ringing_) next_ring_ms_ = 0; }
            if (n == "call" || n == "callsetup") { const bool a = vehicle.ind.call == 1 || vehicle.ind.callsetup == 2 || vehicle.ind.callsetup == 3; if (a != audio_) { audio_ = a; if (onCallAudio) onCallAudio(a); } }
            if (vehicle.ind.callsetup == 0 && vehicle.ind.call == 0) { vehicle.number.clear(); }
        };
        phone.onClip = [this](const std::string& n, int t) { vehicle.number = n; vehicle.numberType = t; };
        phone.onClcc = [this](const std::string& n, int t, int, int) { if (!n.empty() && vehicle.number.empty()) { vehicle.number = n; vehicle.numberType = t; note("caller id from the phone's call list: " + n); } };
        phone.onNetworkName = [this](const std::string& n) { vehicle.operatorName = n; };
        phone.onReady = [this]() { note("phone link ready"); };
        vehicle.onAnswer = [this]() { phone.answer(); };
        vehicle.onHangup = [this]() { phone.hangup(); };
        vehicle.onRedial = [this]() { phone.redial(); };
        vehicle.onDial = [this](const std::string& n) { vehicle.number = n; vehicle.numberType = 129; phone.dial(n); };   // the vehicle already knows the number it dialled: show it at once
        vehicle.onDtmf = [this](char c) { phone.dtmf(c); };
        vehicle.onVoice = [this](bool on) { phone.voice(on); };
    }
    // called regularly (~every 100 ms) with a monotonic millisecond clock: repeats RING (+CLIP) like a real phone while a call is incoming
    void tick(int64_t now_ms) {
        if (ringing_ && vehicle.ready() && now_ms >= next_ring_ms_) { vehicle.sendRing(); next_ring_ms_ = now_ms + 2500; }
    }
    void phoneDown() {                                      // phone link lost: the vehicle must not show a stale call
        phone.onIndicator("service", 0); phone.onIndicator("call", 0); phone.onIndicator("callsetup", 0); phone.onIndicator("callheld", 0); phone.onIndicator("signal", 0);
    }
    bool audioWanted() const { return audio_; }
private:
    void note(const std::string& s) { if (log) log(s); }
    bool ringing_ = false, audio_ = false, clcc_asked_ = false; int64_t next_ring_ms_ = 0;
};

}  // namespace hfp
