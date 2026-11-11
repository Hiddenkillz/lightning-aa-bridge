// g++ -std=c++17 -O1 -Isrc tests/obd_test.cpp src/obd/Obd.cpp -o obd-test -lpthread -lutil && ./obd-test
// Fake ELM327 on a pty: answers AT init, module selects, and 22xxxx requests with canned replies; no real hardware.
#include "obd/Obd.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <pty.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)

// ---- decode() unit tests (hand-computed expected values) ------------------------------------------------------
static void decodeTests() {
    using namespace obd;
    Snapshot s; int64_t t = 1000;
    CHK(decode(0x7E4, 0x480D, {0x0F, 0xA0}, s, t) && std::abs(s.pack_v.v - 40.00) < 1e-6, "480D pack_v 0x0FA0(4000)/100 -> 40.00 V");
    CHK(decode(0x7E4, 0x48F9, {0xFF, 0x9C}, s, t) && std::abs(s.pack_a.v - (-10.0)) < 1e-6, "48F9 pack_a 0xFF9C (-100/10) -> -10.0 A");
    CHK(decode(0x7E4, 0x480A, {0xFF, 0x9C}, s, t) && std::abs(s.pack_a.v - (-2.0)) < 1e-6, "480A fallback pack_a 0xFF9C (-100/50) -> -2.0 A");
    CHK(decode(0x7E4, 0x4801, {0x4E, 0x20}, s, t) && std::abs(s.soc.v - 40.0) < 1e-6, "4801 soc 0x4E20(20000)/500 -> 40.0 %");
    CHK(decode(0x7E4, 0x4845, {0x64}, s, t) && std::abs(s.soc_disp.v - 50.0) < 1e-6, "4845 soc_disp 0x64(100)/2 -> 50.0 %");
    CHK(decode(0x7E4, 0x4848, {0x27, 0x10}, s, t) && std::abs(s.energy_kwh.v - 20.0) < 1e-6, "4848 energy_kwh 0x2710(10000)/500 -> 20.0 kWh");
    CHK(decode(0x7E4, 0x490C, {0xC7}, s, t) && std::abs(s.soh.v - 99.5) < 1e-6, "490C soh 0xC7(199)/2 -> 99.5 %");
    CHK(decode(0x7E4, 0x485C, {0x03, 0xE8}, s, t) && std::abs(s.cap_ah.v - 100.0) < 1e-6, "485C cap_ah 0x03E8(1000)/10 -> 100.0 Ah");
    CHK(decode(0x7E4, 0x485D, {0x00, 0x05}, s, t) && std::abs(s.throughput_ah.v - 50.0) < 1e-6, "485D throughput_ah 0x0005(5)*10 -> 50.0 Ah");
    CHK(decode(0x7E4, 0x4840, {0x0F, 0xA0}, s, t) && std::abs(s.mod_v_min.v - 4.00) < 1e-6, "4840 mod_v_min 0x0FA0(4000)/1000 -> 4.00 V");
    CHK(decode(0x7E4, 0x4841, {0x0F, 0xA0}, s, t) && std::abs(s.mod_v_avg.v - 4.00) < 1e-6, "4841 mod_v_avg -> 4.00 V");
    CHK(decode(0x7E4, 0x483E, {0x00, 0x32}, s, t) && std::abs(s.soc_spread.v - 0.050) < 1e-6, "483E soc_spread 0x0032(50)/1000 -> 0.050 %");
    CHK(decode(0x7E4, 0x4800, {0x46}, s, t) && std::abs(s.t_pack.v - 20.0) < 1e-6, "4800 t_pack 0x46(70)-50 -> 20.0 C");
    {
        std::vector<uint8_t> p = {0x28, 0x1E, 0x0A, 0x23};
        CHK(decode(0x7E4, 0x4808, p, s, t) && std::abs(s.t_max.v - (-10)) < 1e-6 && std::abs(s.t_min.v - (-20)) < 1e-6 &&
                std::abs(s.t_range.v - (-40)) < 1e-6 && std::abs(s.t_avg.v - (-15)) < 1e-6,
            "4808 28 1E 0A 23 -> max=-10 min=-20 range=-40 avg=-15");
    }
    CHK(decode(0x7E4, 0x4846, {0x3C}, s, t) && std::abs(s.t_coolant_in.v - 10.0) < 1e-6, "4846 t_coolant_in 0x3C(60)-50 -> 10.0 C");
    CHK(decode(0x7E4, 0x484F, {0x05}, s, t) && (int)s.chg_state.v == 5 && std::string(obd::chargingStateName((int)s.chg_state.v)) == "Charging",
        "484F chg_state 5 -> Charging");
    CHK(decode(0x7E4, 0x484D, {0x02}, s, t) && (int)s.chg_status.v == 2, "484D chg_status raw 2");
    CHK(decode(0x7E6, 0x48E0, {0x0F}, s, t) && (int)s.thermal_mode.v == 15 && std::string(obd::thermalModeName(15)) == "Heat", "48E0 thermal_mode 15 -> Heat");
    CHK(decode(0x7E6, 0x48DF, {0x01}, s, t) && (int)s.heater_mode.v == 1 && std::string(obd::heaterModeName(1)) == "On", "48DF heater_mode 1 -> On");
    CHK(decode(0x7E6, 0x48DE, {0x0B, 0xB8}, s, t) && std::abs(s.heater_kw.v - 3.0) < 1e-6, "48DE heater_kw 0x0BB8(3000)/1000 -> 3.0 kW");
    CHK(decode(0x7E6, 0x481F, {0x00, 0x32}, s, t) && std::abs(s.motor_coil_c.v - 50.0) < 1e-6, "481F motor_coil_c -> 50.0 C");
    CHK(decode(0x7E6, 0x4824, {0x00, 0x28}, s, t) && std::abs(s.inverter_c.v - 40.0) < 1e-6, "4824 inverter_c -> 40.0 C");
    CHK(decode(0x726, 0x402A, {0x61}, s, t) && std::abs(s.lv_v.v - 10.85) < 1e-6, "402A lv_v 0x61(97)/20+6 -> 10.85 V");
    CHK(decode(0x7E4, 0x48FB, {0x02, 0x58}, s, t) && std::abs(s.chg_power_limit_kw.v - 60.0) < 1e-6, "48FB charge power limit 0x0258/10 -> 60 kW");
    CHK(decode(0x7E4, 0x05A1, {0x5A}, s, t) && s.soc_target.v == 90, "05A1 target SoC 0x5A -> 90 %");
    s.now_ms = t; CHK(decode(0x7E2, 0x4861, {0x64}, s, t) && std::abs(s.pilot_duty.v - 50.0) < 1e-6 && std::abs(s.evse_offer_a() - 30.0) < 1e-6, "4861 pilot 0x64/2 -> 50 % -> 30 A");
    CHK(decode(0x7E2, 0x485E, {0x5D, 0xC0}, s, t) && std::abs(s.evse_v.v - 240.0) < 1e-6, "485E EVSE voltage 0x5DC0/100 -> 240 V");
    s.now_ms = t; CHK(decode(0x7E4, 0x484F, {0x05}, s, t) && s.plugged() && s.charging(), "484F CHARGING -> plugged + charging");
    CHK(decode(0x7E4, 0x484F, {0x00}, s, t) && !s.plugged(), "484F NOT_READY -> not plugged");
    CHK(decode(0x726, 0x402B, {0x7F}, s, t) && std::abs(s.lv_a.v - 0.0) < 1e-6, "402B lv_a 0x7F(127)-127 -> 0.0 A");
    CHK(decode(0x726, 0x4028, {0x50}, s, t) && std::abs(s.lv_soc.v - 80.0) < 1e-6, "4028 lv_soc 0x50(80) -> 80 %");
    CHK(decode(0x726, 0x4027, {0x00, 0x02}, s, t) && std::abs(s.lv_age_h.v - 48.0) < 1e-6, "4027 lv_age_h 0x0002(2)*24 -> 48 h");
    CHK(decode(0x726, 0x4025, {0x07, 0xD0}, s, t) && std::abs(s.lv_quiescent_ma.v - 0.0) < 1e-6, "4025 lv_quiescent_ma 0x07D0(2000)/2-1000 -> 0 mA");
    CHK(decode(0x726, 0x420A, {0x03}, s, t) && (int)s.lv_state.v == 3, "420A lv_state raw 3");
    CHK(decode(0x746, 0x4836, {0x0A}, s, t) && std::abs(s.dcdc_out_a.v - 10.0) < 1e-6, "4836 dcdc_out_a 0x0A -> 10 A");
    CHK(decode(0x7E2, 0xDD05, {0x46}, s, t) && std::abs(s.t_ext.v - 30.0) < 1e-6, "DD05 t_ext 0x46(70)-40 -> 30 C");
    CHK(decode(0x7E2, 0xDD04, {0x3C}, s, t) && std::abs(s.t_int.v - 20.0) < 1e-6, "DD04 t_int 0x3C(60)-40 -> 20 C");
    CHK(decode(0x7E0, 0xDD01, {0x00, 0x01, 0x00}, s, t) && std::abs(s.odo_km.v - 256.0) < 1e-6, "DD01 odo_km 0x000100(256) -> 256 km");
    CHK(!decode(0x7E4, 0x480D, {0x01}, s, t), "decode returns false on short payload");
    s.chg_state.ok = true; s.chg_state.v = obd::CHG_PRE_COND;
    CHK(s.preconditioning(), "preconditioning() true when chg_state==PRE_COND");
}

// ---- fake ELM327 over a pty -------------------------------------------------------------------------------------
struct FakeElm {
    int master_fd = -1;
    std::string slave_path;
    std::thread th;
    std::atomic<bool> running{false};
    std::string cur_module = "none";
    std::map<std::string, int> unsupported_hits;   // count how many times a DID was asked (for logging/inspection)

    bool open() {
        int m, s; char name[256];
        if (openpty(&m, &s, name, nullptr, nullptr) == 0) { master_fd = m; slave_path = name; ::close(s); return true; }   // the Poller opens the slave path itself
        // Under qemu-user with a sysroot (-L) glibc's /dev/ptmx resolves into the sysroot's device node, which cannot be opened there (regress host):
        // /dev/pts/ptmx is not shadowed by the sysroot and works the same.
        m = ::open("/dev/pts/ptmx", O_RDWR | O_NOCTTY); if (m < 0) return false;
        if (grantpt(m) != 0 || unlockpt(m) != 0) { ::close(m); return false; }
        const char* pn = ptsname(m); if (!pn) { ::close(m); return false; }
        master_fd = m; slave_path = pn; return true;
    }
    void start() { running = true; th = std::thread([this] { loop(); }); }
    void stop() { running = false; if (th.joinable()) th.join(); if (master_fd >= 0) ::close(master_fd); }

    static std::string readLine(int fd, std::string& buf) {
        char c;
        for (;;) {
            size_t p = buf.find('\r');
            if (p != std::string::npos) { std::string line = buf.substr(0, p); buf.erase(0, p + 1); return line; }
            ssize_t n = ::read(fd, &c, 1);
            if (n <= 0) { if (errno == EAGAIN || errno == EINTR) { usleep(5000); continue; } return ""; }
            buf += c;
        }
    }
    void reply(const std::string& text) {
        std::string out = text + "\r>";
        ::write(master_fd, out.data(), out.size());
    }
    void loop() {
        std::string buf;
        while (running) {
            fd_set rf; FD_ZERO(&rf); FD_SET(master_fd, &rf);
            timeval tv{0, 50000};
            int r = select(master_fd + 1, &rf, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            std::string line = readLine(master_fd, buf);
            if (line.empty()) continue;
            handle(line);
        }
    }
    void handle(const std::string& cmdIn) {
        std::string cmd = cmdIn;
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == ' ')) cmd.pop_back();
        if (cmd == "ATZ") { reply("ELM327 v1.5"); return; }
        if (cmd.rfind("AT", 0) == 0) { reply("OK"); if (cmd.rfind("ATSH", 0) == 0) cur_module = cmd.substr(5); return; }
        if (cmd.rfind("22", 0) == 0) {
            std::string did = cmd.substr(2);
            handleRequest(did);
            return;
        }
        reply("?");
    }
    void handleRequest(const std::string& did) {
        if (cur_module == "7E4") {
            if (did == "480D") { reply("7EC0562480D0FA0"); return; }       // 40.00 V
            if (did == "48F9") { reply("7EC056248F9FF9C"); return; }  // -10.0 A
            if (did == "4801") { reply("7EC056248014E20"); return; }  // 40.0%
            if (did == "490C") { reply("7EC0562490CC7"); return; }    // 99.5%
            if (did == "485D") { reply("7F2231"); return; }               // negative response -> unsupported
            if (did == "4808") {
                // multi-frame reply split across two lines (the ELM, with ATCAF1, reassembles but we still
                // simulate two physical lines joined by the poller's reader loop reading until '>')
                reply("7EC106208" "48" "28" "1E\r7EC210A" "23");
                return;
            }
        }
        if (cur_module == "726") {
            if (did == "402A") { reply("7260562402A61"); return; }        // 10.85 V
        }
        reply("NO DATA");
    }
};

int main() {
    decodeTests();

    FakeElm fake;
    CHK(fake.open(), "fake ELM327 pty opened");
    fake.start();

    obd::Poller::Options opt;
    opt.port = fake.slave_path;
    opt.log_raw = false;
    obd::Poller poller(opt);
    poller.start();

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8); ++i) {
        poller.setSpeedMps(20.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        obd::Snapshot s = poller.snapshot();
        if (s.pack_v.ok && s.lv_v.ok && s.soh.ok && s.replies > 0) break;
    }
    // give a little extra time for throughput_ah's negative-response + the mi/kWh window to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    obd::Snapshot s = poller.snapshot();
    CHK(s.pack_v.fresh(s.now_ms, 5000) && std::abs(s.pack_v.v - 40.0) < 1e-6, "snapshot pack_v fresh and == 40.0 V");
    CHK(s.lv_v.fresh(s.now_ms, 5000) && std::abs(s.lv_v.v - 10.85) < 1e-6, "snapshot lv_v fresh and == 10.85 V");
    CHK(s.soh.fresh(s.now_ms, 5000) && std::abs(s.soh.v - 99.5) < 1e-6, "snapshot soh fresh and == 99.5 %");
    CHK(!s.throughput_ah.ok, "throughput_ah (485D) stayed unsupported (negative response), never ok");
    CHK(s.replies > 0, "replies counter > 0");
    CHK(s.vehicle_awake, "vehicle_awake == true");
    CHK(s.mi_per_kwh_30s.ok, "mi_per_kwh_30s.ok == true");

    poller.stop();
    fake.stop();

    // "ALL PASSED" suffix on success matches tools/regress.sh's generic `run` grep ('ALL PASSED|FAIL') for CMake-built tests.
    std::cout << (fails ? "OBD TEST FAILED" : "OBD TEST PASSED (ALL PASSED)") << std::endl;
    return fails ? 1 : 0;
}
