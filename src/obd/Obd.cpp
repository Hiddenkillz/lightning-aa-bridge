// ELM327 (vLinker FS) serial driver + UDS 0x22 scheduler for the Ford F-150 Lightning. One thread, one USB device.
// Design: a request table drives a simple round-based scheduler (group by module to minimise ATSH/ATCRA/ATFC switches,
// one request outstanding at a time, <=~10 req/s). decode() is a free function so it is testable without a serial port.
#include "obd/Obd.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace obd {

using clk = std::chrono::steady_clock;
static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count(); }

const char* chargingStateName(int v) {
    switch (v) {
        case CHG_NOT_READY: return "Not ready"; case CHG_READY: return "Ready"; case CHG_FAULT: return "Fault"; case CHG_WAIT: return "Wait";
        case CHG_PRE_COND: return "Pre-condition"; case CHG_CHARGING: return "Charging"; case CHG_DONE: return "Done"; case CHG_EXT: return "External";
        case CHG_INIT: return "Init"; default: return "Unknown";
    }
}
const char* thermalModeName(int v) {
    switch (v) {
        case THM_OFF: return "Off"; case THM_EQUALIZE: return "Equalize"; case THM_COOLANT_FLOW: return "Coolant flow"; case THM_COOLANT_FAN: return "Coolant fan";
        case THM_COOLANT_MODERATE: return "Coolant moderate"; case THM_COOLANT_MAX: return "Coolant max"; case THM_HEAT: return "Heat"; default: return "Unknown";
    }
}
const char* heaterModeName(int v) {
    switch (v) {
        case 0: return "Off"; case 1: return "On"; case 2: return "Degraded"; case 3: return "Shutdown"; case 4: return "Short"; case 5: return "No response";
        case 6: return "Unknown"; case 7: return "Stop"; default: return "Unknown";
    }
}

bool Snapshot::plugged() const {
    const int64_t n = now_ms ? now_ms : nowMs();
    if (chg_state.fresh(n, 30000)) { const int c = (int)chg_state.v; if (c == CHG_READY || c == CHG_WAIT || c == CHG_PRE_COND || c == CHG_CHARGING || c == CHG_DONE || c == CHG_EXT) return true; }
    return prox_status.fresh(n, 120000) && prox_status.v != 0;
}
bool Snapshot::charging() const {
    const int64_t n = now_ms ? now_ms : nowMs();
    if (chg_state.fresh(n, 30000) && ((int)chg_state.v == CHG_CHARGING || (int)chg_state.v == CHG_EXT)) return true;
    return plugged() && pack_a.fresh(n, 5000) && pack_a.v < -2.0;
}
double Snapshot::evse_offer_a() const {
    const int64_t n = now_ms ? now_ms : nowMs();
    if (!pilot_duty.fresh(n, 30000)) return 0;
    const double d = pilot_duty.v;                                   // SAE J1772 pilot duty -> amps
    if (d >= 10 && d <= 85) return d * 0.6;
    if (d > 85 && d <= 96) return (d - 64) * 2.5;
    return 0;
}
bool Snapshot::preconditioning() const {
    int cs = (int)chg_state.v, tm = (int)thermal_mode.v;
    if (chg_state.ok && cs == CHG_PRE_COND) return true;
    if (!thermal_mode.ok) return false;
    return tm == THM_HEAT || tm == THM_COOLANT_FLOW || tm == THM_COOLANT_FAN || tm == THM_COOLANT_MODERATE || tm == THM_COOLANT_MAX;
}

// ---- decode(): pure function, payload = bytes AFTER the 0x62 + 2-byte DID echo -------------------------------------
bool decode(uint16_t module, uint16_t did, const std::vector<uint8_t>& payload, Snapshot& s, int64_t now_ms) {
    auto set = [&](Field& f, double v) { f.v = v; f.at_ms = now_ms; f.ok = true; };
    auto u16 = [&](size_t i) -> int { return (payload[i] << 8) | payload[i + 1]; };
    if (module == 0x7E4) {
        switch (did) {
            case 0x480D: if (payload.size() < 2) return false; set(s.pack_v, u16(0) / 100.0); return true;
            case 0x48F9: if (payload.size() < 2) return false; set(s.pack_a, (int16_t)u16(0) / 10.0); return true;
            case 0x480A: if (payload.size() < 2) return false; set(s.pack_a, (int16_t)u16(0) / 50.0); return true;
            case 0x4801: if (payload.size() < 2) return false; set(s.soc, u16(0) / 500.0); return true;
            case 0x4845: if (payload.size() < 1) return false; set(s.soc_disp, payload[0] / 2.0); return true;
            case 0x4848: if (payload.size() < 2) return false; set(s.energy_kwh, u16(0) / 500.0); return true;
            case 0x490C: if (payload.size() < 1) return false; set(s.soh, payload[0] / 2.0); return true;
            case 0x485C: if (payload.size() < 2) return false; set(s.cap_ah, u16(0) / 10.0); return true;
            case 0x485D: if (payload.size() < 2) return false; set(s.throughput_ah, u16(0) * 10.0); return true;
            case 0x4840: if (payload.size() < 2) return false; set(s.mod_v_min, u16(0) / 1000.0); return true;
            case 0x4841: if (payload.size() < 2) return false; set(s.mod_v_avg, u16(0) / 1000.0); return true;
            case 0x483E: if (payload.size() < 2) return false; set(s.soc_spread, u16(0) / 1000.0); return true;
            case 0x4800: if (payload.size() < 1) return false; set(s.t_pack, (double)payload[0] - 50); return true;
            case 0x4808: if (payload.size() < 4) return false;
                set(s.t_max, (double)payload[0] - 50); set(s.t_min, (double)payload[1] - 50);
                set(s.t_range, (double)payload[2] - 50); set(s.t_avg, (double)payload[3] - 50); return true;
            case 0x4846: if (payload.size() < 1) return false; set(s.t_coolant_in, (double)payload[0] - 50); return true;
            case 0x484F: if (payload.size() < 1) return false; set(s.chg_state, payload[0]); return true;
            case 0x484D: if (payload.size() < 1) return false; set(s.chg_status, payload[0]); return true;
            case 0x48FB: if (payload.size() < 2) return false; set(s.chg_power_limit_kw, u16(0) / 10.0); return true;
            case 0x05A1: if (payload.size() < 1) return false; set(s.soc_target, (double)payload[0]); return true;
            case 0x4842: if (payload.size() < 2) return false; set(s.chg_a_req, u16(0) / 100.0); return true;
            case 0x4844: if (payload.size() < 1) return false; set(s.chg_v_req, payload[0] * 2.0); return true;
            case 0x4850: if (payload.size() < 2) return false; set(s.chg_out_a, u16(0) / 100.0); return true;
            default: return false;
        }
    } else if (module == 0x7E6) {
        switch (did) {
            case 0x48E0: if (payload.size() < 1) return false; set(s.thermal_mode, payload[0]); return true;
            case 0x48DF: if (payload.size() < 1) return false; set(s.heater_mode, payload[0]); return true;
            case 0x48DE: if (payload.size() < 2) return false; set(s.heater_kw, u16(0) / 1000.0); return true;
            case 0x481F: if (payload.size() < 2) return false; set(s.motor_coil_c, (double)u16(0)); return true;
            case 0x4824: if (payload.size() < 2) return false; set(s.inverter_c, (double)u16(0)); return true;
            default: return false;
        }
    } else if (module == 0x726) {
        switch (did) {
            case 0x402A: if (payload.size() < 1) return false; set(s.lv_v, payload[0] / 20.0 + 6); return true;
            case 0x402B: if (payload.size() < 1) return false; set(s.lv_a, (double)payload[0] - 127); return true;
            case 0x4028: if (payload.size() < 1) return false; set(s.lv_soc, (double)payload[0]); return true;
            case 0x4027: if (payload.size() < 2) return false; set(s.lv_age_h, u16(0) * 24.0); return true;
            case 0x4025: if (payload.size() < 2) return false; set(s.lv_quiescent_ma, u16(0) / 2.0 - 1000); return true;
            case 0x420A: if (payload.size() < 1) return false; set(s.lv_state, payload[0]); return true;
            default: return false;
        }
    } else if (module == 0x746) {
        if (did == 0x4836) { if (payload.size() < 1) return false; set(s.dcdc_out_a, (double)payload[0]); return true; }
        return false;
    } else if (module == 0x7E2) {
        if (did == 0xDD05) { if (payload.size() < 1) return false; set(s.t_ext, (double)payload[0] - 40); return true; }
        if (did == 0xDD04) { if (payload.size() < 1) return false; set(s.t_int, (double)payload[0] - 40); return true; }
        switch (did) {
            case 0x485E: if (payload.size() < 2) return false; set(s.evse_v, u16(0) / 100.0); return true;
            case 0x485F: if (payload.size() < 1) return false; set(s.evse_a, (double)payload[0]); return true;
            case 0x4860: if (payload.size() < 1) return false; set(s.evse_hz, payload[0] / 2.0); return true;
            case 0x4861: if (payload.size() < 1) return false; set(s.pilot_duty, payload[0] / 2.0); return true;
            case 0x484A: if (payload.size() < 2) return false; set(s.chg_out_v, u16(0) / 100.0); return true;
            case 0x48C4: if (payload.size() < 2) return false; set(s.chg_out_kw_max, u16(0) / 20.0); return true;
            case 0x484E: if (payload.size() < 2) return false; set(s.chg_in_kw_avail, u16(0) / 200.0); return true;
            case 0x4888: if (payload.size() < 1) return false; set(s.coupler_ac_c, (double)payload[0] - 40); return true;
            case 0x4897: if (payload.size() < 1) return false; set(s.coupler_dc1_c, (double)payload[0] - 40); return true;
            case 0x48A4: if (payload.size() < 1) return false; set(s.coupler_dc3_c, (double)payload[0] - 40); return true;
            case 0x489E: if (payload.size() < 1) return false; set(s.dc_status, payload[0]); return true;
            case 0x4862: if (payload.size() < 1) return false; set(s.prox_status, payload[0]); return true;
            case 0x484D: if (payload.size() < 1) return false; set(s.charger_status, payload[0]); return true;
            default: return false;
        }
    } else if (module == 0x7E0) {
        if (did == 0xDD01) { if (payload.size() < 3) return false; set(s.odo_km, (double)((payload[0] << 16) | (payload[1] << 8) | payload[2])); return true; }
        if (did == 0x1505) { if (payload.size() < 2) return false; set(s.speed_obd_mps, (double)((payload[0] << 8) | payload[1]) / 128.0 / 3.6); return true; }
        return false;
    }
    return false;
}

// ---- request table ---------------------------------------------------------------------------------------------
struct ReqDef { uint16_t module; uint16_t did; double interval_s; };
static const ReqDef kTable[] = {
    {0x7E4, 0x480D, 1},    {0x7E4, 0x48F9, 1},    {0x7E4, 0x4801, 1},    {0x7E4, 0x4845, 5},    {0x7E4, 0x4848, 5},
    {0x7E4, 0x490C, 3600}, {0x7E4, 0x485C, 3600}, {0x7E4, 0x485D, 3600}, {0x7E4, 0x4840, 10},   {0x7E4, 0x4841, 10},
    {0x7E4, 0x483E, 60},   {0x7E4, 0x4800, 10},   {0x7E4, 0x4808, 10},   {0x7E4, 0x4846, 10},   {0x7E4, 0x484F, 5},
    {0x7E4, 0x484D, 5},
    {0x7E6, 0x48E0, 5},    {0x7E6, 0x48DF, 5},    {0x7E6, 0x48DE, 5},    {0x7E6, 0x481F, 10},   {0x7E6, 0x4824, 10},
    {0x726, 0x402A, 1},    {0x726, 0x402B, 5},    {0x726, 0x4028, 5},    {0x726, 0x4027, 3600}, {0x726, 0x4025, 60},
    {0x726, 0x420A, 60},
    {0x746, 0x4836, 5},
    {0x7E2, 0xDD05, 30},   {0x7E2, 0xDD04, 30},
    // charging group: interval_s is the PLUGGED-IN rate; unplugged they are polled at kUnpluggedS (see schedule)
    {0x7E4, 0x48FB, 5},    {0x7E4, 0x05A1, 5},    {0x7E4, 0x4842, 5},    {0x7E4, 0x4844, 5},    {0x7E4, 0x4850, 5},
    {0x7E2, 0x485E, 5},    {0x7E2, 0x485F, 5},    {0x7E2, 0x4860, 30},   {0x7E2, 0x4861, 5},    {0x7E2, 0x484A, 5},
    {0x7E2, 0x48C4, 30},   {0x7E2, 0x484E, 5},    {0x7E2, 0x4888, 10},   {0x7E2, 0x4897, 10},   {0x7E2, 0x48A4, 10},
    {0x7E2, 0x489E, 5},    {0x7E2, 0x4862, 5},    {0x7E2, 0x484D, 5},
    {0x7E0, 0xDD01, 10},
    {0x7E0, 0x1505, 1},
};
static constexpr size_t kTableN = sizeof(kTable) / sizeof(kTable[0]);
static constexpr uint16_t kAltPackACurrentDid = 0x480A;   // fallback for 48F9

struct ReqState {
    ReqDef def;
    int64_t next_due_ms = 0;
    int64_t unsupported_until_ms = 0;     // negative-responded (7F): skip until this time
    int64_t first_tried_ms = 0;           // for the 48F9->480A fallback timeout
    bool ever_answered = false;
};

// ---- Elm327 serial transport ------------------------------------------------------------------------------------
class Elm327 {
public:
    bool open(const std::string& port, int baud) {
        close();
        fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) return false;
        termios tio{};
        if (tcgetattr(fd_, &tio) != 0) { close(); return false; }
        cfmakeraw(&tio);
        speed_t sp = baudConst(baud);
        cfsetispeed(&tio, sp); cfsetospeed(&tio, sp);
        tio.c_cflag |= (CLOCAL | CREAD); tio.c_cflag &= ~CSTOPB; tio.c_cflag &= ~PARENB; tio.c_cflag &= ~CSIZE; tio.c_cflag |= CS8;
        tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd_, TCSANOW, &tio) != 0) { close(); return false; }
        tcflush(fd_, TCIOFLUSH);
        port_ = port;
        return true;
    }
    bool isOpen() const { return fd_ >= 0; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    // Sends a line (adds '\r'), reads until the '>' prompt (or timeout_ms). Returns the collected reply text
    // (echo, SEARCHING..., \r stripped) in `out`; returns false on a hard I/O error (caller should close+rediscover).
    bool command(const std::string& cmd, std::string& out, int timeout_ms, bool log_raw, const std::string& log_prefix) {
        out.clear();
        if (log_raw) std::cout << (log_prefix + " > " + cmd + "\n") << std::flush;
        std::string line = cmd + "\r";
        ssize_t n = ::write(fd_, line.data(), line.size());
        if (n < 0 && errno != EAGAIN) return false;
        std::string raw;
        auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
        pollfd pfd{fd_, POLLIN, 0};
        char buf[256];
        for (;;) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clk::now()).count();
            if (remain <= 0) break;
            int pr = ::poll(&pfd, 1, (int)remain);
            if (pr < 0) { if (errno == EINTR) continue; return false; }
            if (pr == 0) break;                                  // timeout: whatever we have
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
            ssize_t r = ::read(fd_, buf, sizeof buf);
            if (r < 0) { if (errno == EAGAIN || errno == EINTR) continue; return false; }
            if (r == 0) return false;                             // ENODEV-ish: device gone
            raw.append(buf, buf + r);
            if (raw.find('>') != std::string::npos) break;
        }
        // strip echo of the command itself, CR/LF, and the trailing prompt
        std::string cleaned;
        for (char c : raw) if (c != '\r' && c != '>') cleaned += (c == '\n' ? ' ' : c);
        size_t p = cleaned.find(cmd);
        if (p != std::string::npos) cleaned.erase(p, cmd.size());
        // trim
        size_t a = cleaned.find_first_not_of(" \t\n");
        size_t b = cleaned.find_last_not_of(" \t\n");
        out = (a == std::string::npos) ? "" : cleaned.substr(a, b - a + 1);
        if (log_raw && !out.empty()) std::cout << (log_prefix + " < " + out + "\n") << std::flush;
        return true;
    }
    int fd() const { return fd_; }
private:
    static speed_t baudConst(int b) {
        switch (b) {
            case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
            case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
            default: return B115200;
        }
    }
    int fd_ = -1;
    std::string port_;
};

// ---- Poller::Impl -------------------------------------------------------------------------------------------------
struct Poller::Impl {
    Options opt;
    std::thread th;
    std::atomic<bool> running{false};
    mutable std::mutex mtx;
    Snapshot snap;
    std::atomic<double> speed_mps{0}; std::atomic<int64_t> speed_set_ms{0};
    Elm327 elm;
    std::string cur_module;         // currently selected module's 3-hex header, "" = none selected
    std::vector<ReqState> table;
    int64_t last_err_log_ms = 0;
    int64_t last_any_reply_ms = 0;
    bool awake = false;
    // efficiency sliding window
    struct Sample { int64_t t_ms; double dist_m; double energy_kwh; bool has_energy; };
    std::deque<Sample> window;
    int64_t last_eff_update_ms = 0;
    int64_t last_speed_at_ms = 0;

    explicit Impl(Options o) : opt(std::move(o)) {
        table.reserve(kTableN);
        for (auto& d : kTable) table.push_back(ReqState{d, 0, 0, 0, false});
    }

    void log(const std::string& msg) { std::cout << (opt.log_prefix + " " + msg + "\n") << std::flush; }   // one write per line: other threads' lines were spliced into ours (ui62a)
    void logErrRate(const std::string& msg) {
        int64_t n = nowMs();
        if (n - last_err_log_ms < 10000) return;
        last_err_log_ms = n;
        std::cout << (opt.log_prefix + " " + msg + "\n") << std::flush;
    }

    // ---- port discovery ----
    std::string discoverPort() {
        if (!opt.port.empty()) return opt.port;
        const char* dir = "/dev/serial/by-id";
        DIR* d = opendir(dir);
        if (d) {
            std::vector<std::string> names;
            dirent* e;
            while ((e = readdir(d)) != nullptr) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
            closedir(d);
            std::sort(names.begin(), names.end());
            if (!names.empty()) return std::string(dir) + "/" + names.front();
        }
        struct stat st;
        if (::stat("/dev/ttyUSB0", &st) == 0) return "/dev/ttyUSB0";
        if (::stat("/dev/ttyACM0", &st) == 0) return "/dev/ttyACM0";
        return "";
    }

    bool initAdapter() {
        std::string r;
        if (!elm.command("ATZ", r, 1600, opt.log_raw, opt.log_prefix)) return false;
        const char* seq[] = {"ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF1", "ATAT1", "ATST32"};
        for (const char* c : seq) if (!elm.command(c, r, 500, opt.log_raw, opt.log_prefix)) return false;
        cur_module.clear();
        return true;
    }

    bool selectModule(const std::string& mod3) {
        if (mod3 == cur_module) return true;
        std::string r;
        char cra[4]; int modv = (int)strtol(mod3.c_str(), nullptr, 16);
        snprintf(cra, sizeof cra, "%03X", modv + 8);
        std::ostringstream shc; shc << "ATSH " << mod3;
        std::ostringstream crac; crac << "ATCRA " << cra;
        std::ostringstream fcshc; fcshc << "ATFCSH " << mod3;
        const std::string cmds[] = {shc.str(), crac.str(), fcshc.str(), "ATFCSD300000", "ATFCSM1"};
        for (auto& c : cmds) if (!elm.command(c, r, 400, opt.log_raw, opt.log_prefix)) return false;
        cur_module = mod3;
        return true;
    }

    // Parse an ELM reply (headers+ CAF1 reassembled) looking for 0x62 + did-echo, or 0x7F 0x22 NRC.
    // Returns 1 = positive (payload filled), -1 = negative (NRC), 0 = no match / NO DATA / error text.
    int parseReply(const std::string& text, uint16_t did, std::vector<uint8_t>& payload) {
        if (text.find("NO DATA") != std::string::npos) return 0;
        if (text.find("SEARCHING") != std::string::npos) return 0;
        if (text.find("STOPPED") != std::string::npos) return 0;
        if (text.find("UNABLE TO CONNECT") != std::string::npos) return 0;
        if (text.find("CAN ERROR") != std::string::npos) return 0;
        if (text.find("BUS INIT") != std::string::npos) return 0;
        if (text == "?" ) return 0;
        // collect hex bytes from all "lines" (space-separated tokens, possibly concatenated per line with a 3-hex id prefix)
        // Strategy: split on whitespace; tokens may be 3-hex CAN id (odd length won't happen) or hex byte pairs or
        // a long concatenated hex string. Normalize: strip any token that looks like a 3-hex CAN header at start of
        // each physical reply line. Since lines were joined with spaces already, just scan the whole string for hex
        // digits, drop 3-hex-id looking groups heuristically is unreliable; instead locate the 0x62/0x7F markers directly.
        std::vector<uint8_t> bytes;
        {
            std::string hex;
            for (char c : text) if (isxdigit((unsigned char)c)) hex += c;
            // tokens of 2 hex chars; if odd, drop first spurious nibble (defensive)
            if (hex.size() % 2 != 0) hex.erase(hex.begin());
            for (size_t i = 0; i + 1 < hex.size() + 1 && i + 2 <= hex.size(); i += 2)
                bytes.push_back((uint8_t)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        }
        uint8_t dh = (uint8_t)(did >> 8), dl = (uint8_t)(did & 0xFF);
        for (size_t i = 0; i + 1 < bytes.size(); ++i) {
            if (bytes[i] == 0x62 && i + 2 < bytes.size() && bytes[i + 1] == dh && bytes[i + 2] == dl) {
                payload.assign(bytes.begin() + i + 3, bytes.end());
                return 1;
            }
            if (bytes[i] == 0x7F && i + 1 < bytes.size() && bytes[i + 1] == 0x22) return -1;
        }
        return 0;
    }

    void applyDecode(uint16_t module, uint16_t did, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(mtx);
        decode(module, did, payload, snap, nowMs());
    }

    void run() {
        running = true;
        {
            std::lock_guard<std::mutex> lk(mtx);
            snap = Snapshot{};
        }
        std::string port_in_use;
        for (;;) {
            if (!running) return;
            if (!elm.isOpen()) {
                std::string p = discoverPort();
                if (p.empty()) { logErrRate("no serial adapter found (checked Options.port, /dev/serial/by-id, ttyUSB0/ttyACM0)"); sleepPoll(2000); continue; }
                log("opening " + p);
                if (!elm.open(p, opt.baud)) { logErrRate("open failed: " + p); sleepPoll(2000); continue; }
                log("initializing ELM327 on " + p);
                if (!initAdapter()) { logErrRate("ELM327 init failed on " + p); elm.close(); sleepPoll(2000); continue; }
                port_in_use = p;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    snap.port = p; snap.port_present = true; snap.adapter_ok = true;
                }
                log("adapter ready on " + p);
                last_any_reply_ms = nowMs();
            }
            if (!running) return;
            if (!pollOnce()) {
                log("adapter lost (" + port_in_use + "), closing and rediscovering");
                elm.close();
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    snap.port_present = false; snap.adapter_ok = false;
                }
                sleepPoll(2000);
                continue;
            }
            updateEfficiency();
            sleepPoll(20);
        }
    }

    void sleepPoll(int ms) {
        int waited = 0;
        while (waited < ms && running) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); waited += 20; }
    }

    // Pick due entries, group by module, send one request, update state. Returns false on hard I/O error.
    bool pollOnce() {
        int64_t n = nowMs();
        // vehicle awake/asleep transition + slow-poll mode
        bool was_awake = awake;
        awake = (last_any_reply_ms != 0) && (n - last_any_reply_ms < 10000);
        if (was_awake != awake) {
            log(awake ? "vehicle awake" : "vehicle asleep (no module answered for 10 s)");
            if (awake) for (auto& rs : table) { rs.first_tried_ms = 0; if (rs.def.interval_s > 10) rs.next_due_ms = 0; }   // re-ask the slow DIDs now; restart the 48F9 fallback clock
            std::lock_guard<std::mutex> lk(mtx);
            snap.vehicle_awake = awake;
        }
        if (!awake) {
            // slow single probe: 22 402A @ 726 every 10s
            static int64_t next_probe = 0;
            if (n < next_probe) return true;
            next_probe = n + 10000;
            return doOneRequest(0x726, 0x402A);
        }
        // normal scheduling: find all due entries, pick the module with the most due work, send one request from it
        ReqState* best = nullptr;
        for (auto& rs : table) {
            if (n < rs.next_due_ms) continue;
            if (rs.unsupported_until_ms > n) continue;
            if (!best) { best = &rs; continue; }
            // prefer same module as cur_module to avoid header switch; else prefer earliest due
            bool bestSame = sameModule(best->def.module);
            bool rsSame = sameModule(rs.def.module);
            if (rsSame && !bestSame) best = &rs;
            else if (rsSame == bestSame && rs.next_due_ms < best->next_due_ms) best = &rs;
        }
        if (!best) return true;
        uint16_t did = best->def.did;
        if (did == 0x48F9) {
            // fallback: if 48F9 never answered within 30 s of its first try, use 480A instead
            ReqState* alt = nullptr;
            for (auto& rs : table) if (rs.def.did == 0x48F9) { alt = &rs; break; }
            // ui62a boot 3: 48F9 got no answer in the first 30 s (truck still asleep when the Pi booted) and the scheduler then skipped it FOREVER while
            // never sending 480A either -> no pack current, no kW, for the whole drive. Now: after 30 s without an answer, alternate 48F9 / 480A; the
            // first one that answers wins (ever_answered), and a sleep/wake cycle starts the clock again.
            if (alt && !alt->ever_answered) {
                if (alt->first_tried_ms == 0) alt->first_tried_ms = n;
                if (n - alt->first_tried_ms > 30000 && (++alt_toggle & 1)) did = kAltPackACurrentDid;
            }
            if (alt && alt->unsupported_until_ms > n) did = kAltPackACurrentDid;
        }
        { double iv = best->def.interval_s; if (isChargingDid(best->def) && !pluggedNow()) iv = std::max(iv, 60.0); best->next_due_ms = n + (int64_t)(iv * 1000); }
        return doOneRequest(best->def.module, did, best);
    }

    unsigned alt_toggle = 0;
    static bool isChargingDid(const ReqDef& d) {
        if (d.module == 0x7E2) return d.did != 0xDD05 && d.did != 0xDD04;
        return d.did == 0x48FB || d.did == 0x05A1 || d.did == 0x4842 || d.did == 0x4844 || d.did == 0x4850;
    }
    bool pluggedNow() { std::lock_guard<std::mutex> lk(mtx); return snap.plugged(); }
    bool sameModule(uint16_t mod) const {
        if (cur_module.empty()) return false;
        char buf[8]; snprintf(buf, sizeof buf, "%03X", mod);
        return cur_module == buf;
    }

    bool doOneRequest(uint16_t module, uint16_t did, ReqState* rs = nullptr) {
        char mod3[8]; snprintf(mod3, sizeof mod3, "%03X", module);
        {
            std::lock_guard<std::mutex> lk(mtx);
            snap.requests++;
        }
        if (!selectModule(mod3)) return false;
        char reqbuf[16]; snprintf(reqbuf, sizeof reqbuf, "22%04X", did);
        std::string reply;
        if (!elm.command(reqbuf, reply, 1000, opt.log_raw, opt.log_prefix)) return false;
        std::vector<uint8_t> payload;
        int r = parseReply(reply, did, payload);
        int64_t n = nowMs();
        if (r == 1) {
            last_any_reply_ms = n;
            applyDecode(module, did, payload);
            {
                std::lock_guard<std::mutex> lk(mtx);
                snap.replies++;
            }
            if (rs) rs->ever_answered = true;
            else { for (auto& x : table) if (x.def.module == module && x.def.did == did) { x.ever_answered = true; break; } }
        } else if (r == -1) {
            log("DID unsupported (negative response): " + std::string(mod3) + " " + std::to_string(did));
            int64_t until = n + 10 * 60 * 1000;
            if (rs) rs->unsupported_until_ms = until;
            else for (auto& x : table) if (x.def.module == module && x.def.did == did) { x.unsupported_until_ms = until; break; }
            std::lock_guard<std::mutex> lk(mtx);
            snap.errors++;
        } else {
            // NO DATA / timeout: retry soon instead of waiting the whole interval (the hourly health DIDs asked once while the truck was asleep stayed
            // blank for the entire drive in ui62a).
            if (rs && rs->def.interval_s > 10) rs->next_due_ms = n + 10000;
        }
        return true;
    }

    void updateEfficiency() {
        int64_t n = nowMs();
        if (n - last_eff_update_ms < 1000) return;
        int64_t dt_ms = last_eff_update_ms == 0 ? 1000 : (n - last_eff_update_ms);
        last_eff_update_ms = n;
        double speed = speed_mps.load(); bool from_obd = false;
        { std::lock_guard<std::mutex> lk(mtx);                                                   // Sync's speed goes stale when its USB session is down (start-up flap): use the PCM's own
          if (n - speed_set_ms.load() > 3000 && snap.speed_obd_mps.fresh(n, 3000)) { speed = snap.speed_obd_mps.v; from_obd = true; } }
        double dist_m = speed * (dt_ms / 1000.0);
        bool pack_kw_fresh = false;
        double pack_kw_val = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            pack_kw_fresh = snap.pack_v.fresh(n, 3000) && snap.pack_a.fresh(n, 3000);
            if (pack_kw_fresh) {
                pack_kw_val = snap.pack_v.v * snap.pack_a.v / 1000.0;
                snap.pack_kw.v = pack_kw_val; snap.pack_kw.at_ms = n; snap.pack_kw.ok = true;
            }
            snap.speed_mps.v = speed; snap.speed_mps.at_ms = n; snap.speed_mps.ok = true; snap.speed_from_obd = from_obd;
        }
        double energy_kwh = pack_kw_fresh ? (pack_kw_val * (dt_ms / 1000.0) / 3600.0) : 0;
        window.push_back(Sample{n, dist_m, energy_kwh, pack_kw_fresh});
        while (!window.empty() && n - window.front().t_ms > 30000) window.pop_front();
        double sum_dist_m = 0, sum_energy_kwh = 0;
        for (auto& s : window) { sum_dist_m += s.dist_m; if (s.has_energy) sum_energy_kwh += s.energy_kwh; }
        double v; bool ok = true;
        const double M_PER_MILE = 1609.344;
        if (sum_dist_m < 5.0) v = 0;                                                           // stopped
        else if (sum_energy_kwh <= 0) v = -1;                                                  // net regen over the window
        else v = std::min(99.9, (sum_dist_m / M_PER_MILE) / sum_energy_kwh);                   // tiny positive energy (coasting) -> capped, still a number
        { std::lock_guard<std::mutex> lk(mtx);
          snap.mi_per_kwh_30s.v = v; snap.mi_per_kwh_30s.at_ms = n; snap.mi_per_kwh_30s.ok = ok; }
        if (n - last_summary_ms >= 60000) {                                                     // one decoded line a minute (the raw log is ~80k lines a drive)
            last_summary_ms = n; Snapshot s; { std::lock_guard<std::mutex> lk(mtx); s = snap; }
            char b[320]; snprintf(b, sizeof b, "summary: kW %.1f (V %.0f A %.1f%s) soc %.1f%% kWh %.1f eff30 %.1f tpack %.0fC lv %.2fV %.0fA %.0f%% chg %d thm %d speed %.1f m/s%s replies %u",
                s.pack_kw.v, s.pack_v.v, s.pack_a.v, s.pack_a.fresh(n, 3000) ? "" : " STALE", s.soc.v, s.energy_kwh.v, s.mi_per_kwh_30s.v, s.t_pack.v, s.lv_v.v, s.lv_a.v, s.lv_soc.v,
                (int)s.chg_state.v, (int)s.thermal_mode.v, s.speed_mps.v, s.speed_from_obd ? " (obd)" : "", s.replies);
            log(b);
        }
    }
    int64_t last_summary_ms = 0;
};

Poller::Poller(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}
Poller::~Poller() { stop(); }

void Poller::start() {
    if (impl_->running) return;
    impl_->running = true;
    impl_->th = std::thread([this] { impl_->run(); });
}
void Poller::stop() {
    impl_->running = false;
    if (impl_->th.joinable()) impl_->th.join();
    impl_->elm.close();
}
Snapshot Poller::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    Snapshot s = impl_->snap;
    s.now_ms = nowMs();
    return s;
}
void Poller::setSpeedMps(double mps) { impl_->speed_mps = mps; impl_->speed_set_ms = nowMs(); }
std::string Poller::debugLine() const {
    Snapshot s = snapshot();
    std::ostringstream o;
    o << "port=" << (s.port.empty() ? "-" : s.port) << " present=" << s.port_present << " adapter_ok=" << s.adapter_ok
      << " awake=" << s.vehicle_awake << " req=" << s.requests << " rep=" << s.replies << " err=" << s.errors;
    return o.str();
}

}  // namespace obd
