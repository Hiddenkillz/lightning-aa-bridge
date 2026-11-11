#include "CallBridge.h"
#include "Hfp.h"
#include "Pump.h"
#include "AaCall.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <poll.h>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <cstdio>

#define CLOG(x) do { std::cout << "[Call] " << x << std::endl; } while (0)

namespace btcall {

static std::string readText(const std::string& p) { std::ifstream f(p); std::string s; std::getline(f, s); return s; }

std::string adapterAddressByIndex(int dev_id) {
    int s = ::socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI); if (s < 0) return "";
    struct hci_dev_info di; memset(&di, 0, sizeof di); di.dev_id = (uint16_t)dev_id;
    std::string out;
    if (::ioctl(s, HCIGETDEVINFO, (void*)&di) == 0) { char b[18]; ba2str(&di.bdaddr, b); out = b; for (auto& ch : out) ch = (char)toupper((unsigned char)ch); if (out == "00:00:00:00:00:00") out.clear(); }
    ::close(s); return out;
}
std::vector<AdapterInfo> listAdapters() {
    std::vector<AdapterInfo> v;
    if (DIR* d = ::opendir("/sys/class/bluetooth")) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name; if (n.rfind("hci", 0) != 0 || n.find(':') != std::string::npos) continue;
            AdapterInfo a; a.name = n; a.dev_id = atoi(n.c_str() + 3);
            char buf[512]; ssize_t k = ::readlink(("/sys/class/bluetooth/" + n).c_str(), buf, sizeof buf - 1);
            if (k > 0) { buf[k] = 0; a.usb = std::string(buf).find("/usb") != std::string::npos; }
            a.address = adapterAddressByIndex(a.dev_id);            // sysfs has no 'address' attribute (ui60b): ask the HCI socket; empty until the adapter was opened once
            v.push_back(a);
        }
        ::closedir(d);
    }
    std::sort(v.begin(), v.end(), [](const AdapterInfo& x, const AdapterInfo& y) { return x.dev_id < y.dev_id; });
    return v;
}

bool routeScoToHci(int dev_id, std::string* why) {
    int s = hci_open_dev(dev_id);
    if (s < 0) { if (why) *why = "cannot open hci" + std::to_string(dev_id); return false; }
    hci_version ver; memset(&ver, 0, sizeof ver);
    bool ok = false;
    if (hci_read_local_version(s, &ver, 1000) < 0) { if (why) *why = "read version failed"; }
    else if (ver.manufacturer != 15) { if (why) *why = "manufacturer " + std::to_string(ver.manufacturer) + " (not Broadcom): nothing to do"; ok = true; }
    else {
        uint8_t p[5] = {1, 2, 0, 1, 1};                      // Write_SCO_PCM_Int_Param: routing = transport (HCI), PCM rate, frame, sync, clock
        uint8_t r[8] = {0};
        hci_request rq; memset(&rq, 0, sizeof rq); rq.ogf = 0x3f; rq.ocf = 0x1c; rq.cparam = p; rq.clen = 5; rq.rparam = r; rq.rlen = sizeof r;
        if (hci_send_req(s, &rq, 1500) < 0) { if (why) *why = "vendor command failed"; }
        else { ok = r[0] == 0; if (why) *why = ok ? "Broadcom: SCO routed to the host (HCI)" : "vendor command status " + std::to_string(r[0]); }
    }
    ::close(s); return ok;
}

static bool parseAddr(const std::string& s, bdaddr_t* out) { return !s.empty() && str2ba(s.c_str(), out) == 0; }

class CallBridge::Impl {
public:
    explicit Impl(const Config& c) : cfg_(c) {
        core_.vehicle.manufacturer = c.bt_call_make; core_.vehicle.model = c.bt_call_model;   // what the vehicle sees for AT+CGMI/CGMM
        core_.log = [](const std::string& s) { CLOG(s); };
        // AT traffic leaves through a per-link outbox: the callers hold m_ (reader threads, the tick thread), and a blocking send on a congested RFCOMM link
        // used to stall every other part of the bridge (ringing, the audio watchdog, the other link) for as long as the radio took. Order per link is kept.
        core_.phone.send = [this](const std::string& s) { phone_out_.post(phone_fd_, s); };
        core_.vehicle.send = [this](const std::string& s) { veh_out_.post(veh_fd_, s); };
        core_.onCallAudio = [this](bool a) { CLOG(std::string("call audio ") + (a ? "wanted" : "no longer wanted")); audioWanted(a); if (!a) closeAudio("call ended"); };
        core_.vehicle.onReady = [this]() { CLOG("vehicle hands-free link established"); if (cfg_.bt_call_sim) simArm(nowMs());
            // A call already in progress when the vehicle link comes up (ui54b: the Pi booted mid-call; the phone's audio was offered before the vehicle existed and
            // the three attempts to open it were spent while there was no vehicle to bridge to) is moved to the vehicle now, like a real phone pairing mid-call.
            if (core_.audioWanted() && !audio_live_) { hf_sco_tries_ = 0; audio_wanted_ms_ = nowMs(); CLOG("call already active when the vehicle link came up: moving its audio to the vehicle"); } };
        if (cfg_.bt_call_sim) {                                           // test call: the vehicle's answer / hang up / dial drive the staged call instead of a phone
            core_.onCallAudio = [this](bool a) { CLOG(std::string("call audio ") + (a ? "wanted" : "no longer wanted")); audioWanted(a); if (a) { if (!phone_linked_) startSimAudio(); } else closeAudio("call ended"); };
            core_.vehicle.onAnswer = [this]() { if (!phone_linked_ && sim_ == Sim::Ringing) { CLOG("sim: vehicle ANSWERED the test call (ATA)"); simConnect(nowMs(), "answered on the vehicle"); } else core_.phone.answer(); };
            core_.vehicle.onHangup = [this]() { if (!phone_linked_ && (sim_ == Sim::Ringing || sim_ == Sim::Dialing || sim_ == Sim::Active)) { CLOG("sim: vehicle HUNG UP (AT+CHUP)"); simEnd(); } else core_.phone.hangup(); };
            core_.vehicle.onDial = [this](const std::string& n) { if (!phone_linked_ && (sim_ == Sim::Armed || sim_ == Sim::Done)) simDial(n); else core_.phone.dial(n); };
            core_.vehicle.onRedial = [this]() { if (!phone_linked_ && (sim_ == Sim::Armed || sim_ == Sim::Done)) simDial(kSimNumber); else core_.phone.redial(); };
        }
        core_.vehicle.onOther = [](const std::string& l) { CLOG("vehicle sent an unsupported command: " + l); };
        tick_ = std::thread([this] { while (run_) { { std::lock_guard<std::mutex> g(m_); const int64_t t = nowMs(); core_.tick(t); if (cfg_.bt_call_sim) simTick(t); audioWatch(t); } std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
    }
    ~Impl() { stop(); }
    void stop() {
        if (!run_.exchange(false)) return;
        closeAudio("stopping"); closeLinks();
        if (tick_.joinable()) tick_.join();
        phone_out_.stop(); veh_out_.stop();
        if (hf_sco_.joinable()) hf_sco_.join();
        closeAudio("stopping (late)");                 // a pump started by an HF attempt that was mid-flight when we began stopping
        if (accept_.joinable()) accept_.join();
        if (vaccept_.joinable()) vaccept_.join();
        if (hf_sco_.joinable()) hf_sco_.join();
        { int f = veh_sco_pending_.exchange(-1); if (f >= 0) ::close(f); }
        std::lock_guard<std::mutex> g(thr_m_); for (auto& t : readers_) if (t.joinable()) t.join(); readers_.clear();
    }
    void setAdapters(const std::string& phone, const std::string& vehicle) {
        phone_adapter_ = phone; veh_adapter_ = vehicle;
        if (!accept_.joinable()) accept_ = std::thread([this] { scoAcceptLoop(); });
        if (!vehicle.empty() && !vaccept_.joinable()) vaccept_ = std::thread([this] { vehicleScoAcceptLoop(); });
    }
    void setAa(std::shared_ptr<AaCall> a) { aa_ = std::move(a); }
    bool phoneLinked() const { return phone_linked_.load(); }
    void onPhoneLink(int fd, const std::string& addr) {
        { std::lock_guard<std::mutex> g(m_); if (phone_fd_ >= 0) { ::shutdown(phone_fd_, SHUT_RDWR); } }
        CLOG("phone hands-free link from " << addr);
        spawnReader(fd, true, addr);
    }
    void onVehicleLink(int fd, const std::string& addr) {
        { std::lock_guard<std::mutex> g(m_); if (veh_fd_ >= 0) { ::shutdown(veh_fd_, SHUT_RDWR); } }
        CLOG("vehicle hands-free link from " << addr);
        spawnReader(fd, false, addr);
    }
private:
    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    static void writeAll(int fd, const std::string& s) {
        if (fd < 0) return;
        size_t off = 0;
        while (off < s.size()) { ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL); if (n <= 0) return; off += (size_t)n; }
    }
    struct Outbox {                                    // one writer thread per link; post() never blocks the caller
        std::mutex m; std::condition_variable cv; std::deque<std::pair<int, std::string>> q; bool stop_ = false; std::thread t;
        void post(int fd, const std::string& s) { if (fd < 0) return; { std::lock_guard<std::mutex> g(m); if (stop_) return; if (q.size() > 256) { q.pop_front(); } q.emplace_back(fd, s); if (!t.joinable()) t = std::thread([this] { run(); }); } cv.notify_one(); }
        void run() { for (;;) { std::pair<int, std::string> it; { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return stop_ || !q.empty(); }); if (stop_ && q.empty()) return; it = std::move(q.front()); q.pop_front(); } writeAll(it.first, it.second); } }
        void stop() { { std::lock_guard<std::mutex> g(m); stop_ = true; } cv.notify_all(); if (t.joinable()) t.join(); }
        ~Outbox() { stop(); }
    } phone_out_, veh_out_;
    void spawnReader(int fd, bool is_phone, const std::string& addr) {
        std::lock_guard<std::mutex> g(thr_m_);
        readers_.emplace_back([this, fd, is_phone, addr] { readerLoop(fd, is_phone, addr); });
    }
    void readerLoop(int fd, bool is_phone, const std::string& addr) {
        { std::lock_guard<std::mutex> g(m_);
          if (is_phone) { phone_fd_ = fd; phone_addr_ = addr; phone_linked_ = true; phone_link_ms_ = nowMs(); core_.phone.start();
              if (cfg_.bt_call_sim && sim_ != Sim::Off) { if (sim_ == Sim::Ringing || sim_ == Sim::Dialing || sim_ == Sim::Active) simEnd(); sim_ = Sim::Done; CLOG("sim: a real phone linked -> test call mode stands aside (its calls go to the phone)"); } }
          else { veh_fd_ = fd; veh_addr_ = addr; core_.vehicle.reset(); if (cfg_.bt_call_sim) { core_.vehicle.ind.service = 1; core_.vehicle.ind.signal = 4; core_.vehicle.ind.battchg = 4; sim_ = Sim::Off; } } }
        char buf[512];
        while (run_) {
            pollfd p{fd, POLLIN, 0}; int r = ::poll(&p, 1, 200);
            if (r < 0 && errno != EINTR) break;
            if (r > 0) {
                ssize_t n = ::read(fd, buf, sizeof buf);
                if (n <= 0) break;
                std::lock_guard<std::mutex> g(m_);
                if (is_phone ? phone_fd_ != fd : veh_fd_ != fd) break;       // superseded by a newer link
                if (is_phone) core_.phone.feed(buf, (size_t)n); else core_.vehicle.feed(buf, (size_t)n);
            }
        }
        bool wasCurrent = false;
        { std::lock_guard<std::mutex> g(m_);
          if (is_phone && phone_fd_ == fd) { phone_fd_ = -1; phone_linked_ = false; wasCurrent = true; core_.phoneDown(); }
          if (!is_phone && veh_fd_ == fd) { veh_fd_ = -1; wasCurrent = true; core_.vehicle.reset(); } }
        if (wasCurrent) { CLOG((is_phone ? "phone" : "vehicle") << std::string(" hands-free link closed")); closeAudio(is_phone ? "phone link closed" : "vehicle link closed"); }
        ::close(fd);
    }
    void closeLinks() { std::lock_guard<std::mutex> g(m_); if (phone_fd_ >= 0) ::shutdown(phone_fd_, SHUT_RDWR); if (veh_fd_ >= 0) ::shutdown(veh_fd_, SHUT_RDWR); }

    // ---- staged test call (bt_call_sim) ------------------------------------------------------------------------------------------------
    // All of this runs under m_ (tick thread or the vehicle reader). The "phone" events are injected through core_.phone.onIndicator/onClip,
    // so the vehicle sees exactly what a real phone's indicators would produce (+CIEV, RING, +CLIP, call audio wanted).
    enum class Sim { Off, Armed, Ringing, Dialing, Active, Done };
    static constexpr const char* kSimNumber = "2055550123";
    Sim sim_ = Sim::Off; int64_t sim_t_ = 0; int sim_step_ = 0, sim_calls_ = 0;
    void simArm(int64_t now) { sim_ = Sim::Armed; sim_t_ = now + (int64_t)cfg_.bt_call_sim_delay_s * 1000; CLOG("sim: vehicle hands-free link up; an incoming TEST CALL rings in " << cfg_.bt_call_sim_delay_s << " s"); }
    void simTick(int64_t now) {
        switch (sim_) {
        case Sim::Armed:
            if (phone_linked_) { sim_ = Sim::Done; break; }
            if (now >= sim_t_) { CLOG("sim: INCOMING test call from " << kSimNumber << " -> answer it on the vehicle (screen or steering wheel)");
                core_.phone.onClip(kSimNumber, 129); core_.phone.onIndicator("callsetup", 1); sim_ = Sim::Ringing; sim_t_ = now + (int64_t)cfg_.bt_call_sim_ring_s * 1000; }
            break;
        case Sim::Ringing:
            if (now >= sim_t_) { CLOG("sim: not answered on the vehicle within " << cfg_.bt_call_sim_ring_s << " s -> answering by myself"); simConnect(now, "auto-answered"); }
            break;
        case Sim::Dialing:
            if (now >= sim_t_) { if (sim_step_ == 0) { core_.phone.onIndicator("callsetup", 3); sim_step_ = 1; sim_t_ = now + 2000; CLOG("sim: outgoing call ringing at the far end"); }
                                 else simConnect(now, "outgoing call picked up"); }
            break;
        case Sim::Active:
            if (now >= sim_t_) { CLOG("sim: test call ended after " << cfg_.bt_call_sim_len_s << " s (not hung up on the vehicle)"); simEnd(); }
            break;
        default: break;
        }
    }
    void simConnect(int64_t now, const char* how) {
        ++sim_calls_; core_.phone.onIndicator("call", 1); core_.phone.onIndicator("callsetup", 0);
        sim_ = Sim::Active; sim_t_ = now + (int64_t)cfg_.bt_call_sim_len_s * 1000;
        CLOG("sim: call #" << sim_calls_ << " ACTIVE (" << how << "): test tone to the vehicle for up to " << cfg_.bt_call_sim_len_s << " s; talk so the vehicle microphone can be checked");
    }
    void simEnd() { core_.phone.onIndicator("call", 0); core_.phone.onIndicator("callsetup", 0); sim_ = Sim::Done; CLOG("sim: idle; dial any number on the vehicle for an outgoing test call"); }
    void simDial(const std::string& n) { CLOG("sim: vehicle DIALS " << n << " -> outgoing test call"); core_.phone.onClip(n, 129); core_.phone.onIndicator("callsetup", 2); sim_ = Sim::Dialing; sim_step_ = 0; sim_t_ = nowMs() + 1500; }
    void startSimAudio() {                                        // AG side opens the audio link (as a phone does once a call is active)
        bdaddr_t vsrc, vdst;
        if (veh_fd_ < 0 || !parseAddr(veh_adapter_, &vsrc) || !parseAddr(veh_addr_, &vdst)) { CLOG("sim: no vehicle link/adapter for call audio (adapter '" << veh_adapter_ << "', vehicle '" << veh_addr_ << "')"); return; }
        closeAudio("new sim audio");
        std::lock_guard<std::mutex> g(audio_m_);
        if (pump_.joinable()) { audio_stop_ = true; pump_.join(); }
        audio_stop_ = false;
        pump_ = std::thread([this, vsrc, vdst] {
            int vs = connectSco(vsrc, vdst);
            if (vs < 0) { CLOG("sim: the vehicle did not accept call audio (SCO connect failed)"); return; }
            simAudioLoop(vs); ::close(vs);
        });
    }
    void simAudioLoop(int vs) {
        sco_options opt; socklen_t ol = sizeof opt; int mtu = 48;
        if (::getsockopt(vs, SOL_SCO, SCO_OPTIONS, &opt, &ol) == 0 && opt.mtu > 0 && opt.mtu <= 512) mtu = opt.mtu;
        CLOG("sim: call audio UP to the vehicle (SCO mtu " << mtu << "): 440 Hz tone bursts out, vehicle microphone in (first 30 s saved to /var/log/aa-bridge-sim-mic.raw)");
        FILE* rec = std::fopen("/var/log/aa-bridge-sim-mic.raw", "wb"); long rec_bytes = 0;
        const auto t0 = std::chrono::steady_clock::now(); auto last_log = t0;
        long sent = 0, recv = 0, eagain = 0, pkts_in = 0; int peak = 0, peak_total = 0; uint32_t si = 0;
        std::vector<uint8_t> buf((size_t)mtu); uint8_t in[512];
        while (!audio_stop_) {
            const auto now = std::chrono::steady_clock::now();
            const long elapsed_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
            long due = elapsed_ms * 16 - sent;                                                     // 8 kHz x 16 bit = 16 bytes per ms
            int guard = 0;
            while (due >= mtu && guard++ < 8) {
                for (int i = 0; i + 1 < mtu; i += 2, ++si) {                                        // 0.4 s on / 0.4 s off
                    const bool on = ((si / 3200) % 2) == 0; const int16_t v = on ? (int16_t)(9000.0 * std::sin(2.0 * M_PI * 440.0 * (double)si / 8000.0)) : 0;
                    std::memcpy(&buf[(size_t)i], &v, 2);
                }
                const ssize_t w = ::send(vs, buf.data(), (size_t)mtu, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (w == mtu) { sent += mtu; due -= mtu; } else { ++eagain; if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { CLOG("sim: audio send error: " << strerror(errno)); audio_stop_ = true; } break; }
            }
            pollfd p{vs, POLLIN, 0};
            if (::poll(&p, 1, 3) > 0) {
                if (p.revents & POLLIN) {
                    const ssize_t n = ::read(vs, in, sizeof in);
                    if (n == 0) { CLOG("sim: the vehicle closed call audio"); break; }
                    if (n > 0) { recv += n; ++pkts_in; for (ssize_t i = 0; i + 1 < n; i += 2) { int16_t s; std::memcpy(&s, in + i, 2); peak = std::max(peak, std::abs((int)s)); }
                        if (rec && rec_bytes < 480000) { rec_bytes += (long)std::fwrite(in, 1, (size_t)n, rec); } }
                } else if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { CLOG("sim: call audio link dropped"); break; }
            }
            if (now - last_log >= std::chrono::seconds(5)) {
                last_log = now; peak_total = std::max(peak_total, peak);
                CLOG("sim: call audio " << elapsed_ms / 1000 << " s: tone sent " << sent << " B, mic received " << recv << " B in " << pkts_in << " packets, mic peak 5 s " << peak << " (" << (peak > 1500 ? "sound" : peak > 0 ? "near silence" : "nothing") << "), send-busy " << eagain);
                peak = 0;
            }
        }
        if (rec) std::fclose(rec);
        CLOG("sim: call audio finished: tone sent " << sent << " B, mic received " << recv << " B, mic peak overall " << std::max(peak_total, peak) << ", mic saved " << rec_bytes << " B");
    }

    // ---- bridging a phone SCO to the vehicle (from the accept loop, or from our own HF-initiated connect) ----------------------------
    void bridgeAudio(int ps, const char* how) {
        if (!run_) { ::close(ps); return; }                                   // shutting down: no new pump
        if (strcmp(how, "we opened") != 0) phone_took_audio_ = false;        // the PHONE routed audio to us: it wants the vehicle again
        if (!phone_linked_) CLOG("NOTE: the phone sends call audio but has no hands-free link to us (headset mode): the vehicle gets audio without any call state");
        std::string vaddr; { std::lock_guard<std::mutex> g(m_); vaddr = veh_fd_ >= 0 ? veh_addr_ : ""; }
        bdaddr_t vsrc, vdst;
        if (vaddr.empty() || !parseAddr(veh_adapter_, &vsrc) || !parseAddr(vaddr, &vdst)) { CLOG("no vehicle hands-free link: the call stays on the phone"); ::close(ps); return; }
        closeAudio("new call audio");
        int vs = -1; const char* vhow = "we opened the vehicle link";
        if (cfg_.bt_call_veh_sco == "wait") {                                     // let the vehicle (hands-free side) open its own audio link first
            const int64_t t0 = nowMs();
            while (nowMs() - t0 < cfg_.bt_call_veh_sco_wait_ms && !audio_stop_) { vs = veh_sco_pending_.exchange(-1); if (vs >= 0) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            if (vs >= 0) vhow = "the VEHICLE opened its link"; else CLOG("the vehicle did not open call audio within " << cfg_.bt_call_veh_sco_wait_ms << " ms: opening it ourselves");
        }
        if (vs < 0) vs = connectSco(vsrc, vdst);
        if (vs < 0) { CLOG("vehicle refused call audio: the call stays on the phone"); ::close(ps); return; }
        CLOG("call audio up: phone <-> vehicle (" << how << "; " << vhow << "); packet sizes phone " << scoMtu(ps) << " B, vehicle " << scoMtu(vs) << " B (re-chunked to each side)");
        std::lock_guard<std::mutex> g(audio_m_);
        if (pump_.joinable()) { audio_stop_ = true; pump_.join(); }
        stats_.a_to_b = 0; stats_.b_to_a = 0; stats_.dropped = 0; stats_.err = 0; audio_stop_ = false; audio_live_ = true;
        pump_ = std::thread([this, ps, vs] {
            const char* why = pumpAudio(ps, vs, audio_stop_, stats_, 0, cfg_.bt_call_pkt_phone, cfg_.bt_call_pkt_vehicle);
            if (!audio_stop_ && (strstr(why, "side A") || strstr(why, "phone audio link"))) phone_took_audio_ = true;   // the phone ended it, not us: do not pull it back
            ::close(ps); ::close(vs); audio_live_ = false; audio_down_ms_ = nowMs();
            CLOG("call audio ended: " << why << (stats_.err.load() ? std::string(" (") + strerror(stats_.err.load()) + ")" : "") << "; bytes phone->vehicle " << stats_.a_to_b.load() << ", vehicle->phone " << stats_.b_to_a.load() << ", dropped " << stats_.dropped.load() << "; packet sizes used: to phone " << stats_.mtu_a.load() << " B (it sends " << stats_.rx_a.load() << "), to vehicle " << stats_.mtu_b.load() << " B (it sends " << stats_.rx_b.load() << ")"
                 << (core_.audioWanted() ? "; the call is still active: re-opening audio shortly unless the phone does" : ""));
        });
    }
    // HF-initiated audio (HFP allows either side to open the SCO link when no codec negotiation is in use). ui36 drive: Sync showed the call but no audio
    // reached the vehicle; the bridge only ever waited for the phone to open audio. Now: call active + no audio for 1.5 s -> we open it; retry after an error.
    std::atomic<bool> phone_took_audio_{false}; int64_t phone_link_ms_ = 0;
    std::atomic<bool> audio_live_{false}; std::atomic<int64_t> audio_down_ms_{0}; int64_t audio_wanted_ms_ = 0; int hf_sco_tries_ = 0; bool hf_sco_busy_ = false; std::thread hf_sco_;
    void audioWanted(bool a) {
        if (a) { call_start_ms_ = nowMs(); call_audio_ms_ = 0; if (nowMs() - phone_link_ms_ > 3000) phone_took_audio_ = false; else if (phone_took_audio_) CLOG("call already active on a fresh phone link: not pulling its audio (the phone offers it if it wants the vehicle)"); }
        else if (call_start_ms_) {                                   // one line per call for the log reader: correlate with [Av]/[HuSession] focus, mic and guidance lines by time
            CLOG("CALL SUMMARY: " << (nowMs() - call_start_ms_) / 1000 << " s, number '" << core_.vehicle.number << "', audio bytes phone->vehicle " << stats_.a_to_b.load() << " vehicle->phone " << stats_.b_to_a.load()
                 << ", audio opened by " << (hf_sco_tries_ ? "us (" + std::to_string(hf_sco_tries_) + " attempt(s))" : "the phone") << (audio_live_ ? ", audio still up at hang-up" : ", audio already down at hang-up"));
            call_start_ms_ = 0;
        }
        audio_wanted_ms_ = a ? nowMs() : 0; hf_sco_tries_ = 0;
    }
    int64_t call_start_ms_ = 0, call_audio_ms_ = 0;
    void audioWatch(int64_t now) {                                     // under m_, every 100 ms
        if (!core_.audioWanted() || audio_live_ || hf_sco_busy_ || !phone_linked_ || aa_) return;
        if (phone_took_audio_) return;                                   // ui58a: the user moved the call to the handset and we kept grabbing it back
        if (veh_fd_ < 0) { audio_wanted_ms_ = now; hf_sco_tries_ = 0; return; }   // nothing to bridge to yet: the attempts are not spent, the clock restarts when the vehicle link is up
        const int64_t since = std::max(audio_wanted_ms_, audio_down_ms_.load());
        if (since == 0 || now - since < 1500) return;
        if (hf_sco_tries_ >= 3) { if (hf_sco_tries_ == 3) { ++hf_sco_tries_; CLOG("call active but no call audio after 3 attempts to open it: the call stays on the phone"); } return; }
        ++hf_sco_tries_; hf_sco_busy_ = true;
        const std::string pa = phone_adapter_, dst = phone_addr_; const int n = hf_sco_tries_;
        if (hf_sco_.joinable()) hf_sco_.join();
        hf_sco_ = std::thread([this, pa, dst, n] {
            bdaddr_t s, d;
            if (!parseAddr(pa, &s) || !parseAddr(dst, &d)) { CLOG("cannot open call audio to the phone: adapter '" << pa << "' phone '" << dst << "'"); std::lock_guard<std::mutex> g(m_); hf_sco_busy_ = false; return; }
            CLOG("call active but the phone has not opened call audio: opening it ourselves (" << n << "/3)");
            int ps = connectSco(s, d);
            if (ps < 0) { CLOG("the phone did not accept our call audio link (" << n << "/3)"); { std::lock_guard<std::mutex> g(m_); hf_sco_busy_ = false; } audio_down_ms_ = nowMs(); return; }
            bridgeAudio(ps, "we opened");
            std::lock_guard<std::mutex> g(m_); hf_sco_busy_ = false;
        });
    }

    // SCO the VEHICLE opens toward the dongle (HF-initiated audio). Kept in veh_sco_pending_ for bridgeAudio (wait mode); logged either way, stale ones are closed.
    std::atomic<int> veh_sco_pending_{-1}; std::thread vaccept_; int64_t veh_sco_pending_ms_ = 0;
    void vehicleScoAcceptLoop() {
        bdaddr_t src; if (!parseAddr(veh_adapter_, &src)) return;
        int ls = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_SCO); if (ls < 0) { CLOG("vehicle SCO listen socket failed: " << strerror(errno)); return; }
        sockaddr_sco la; memset(&la, 0, sizeof la); la.sco_family = AF_BLUETOOTH; bacpy(&la.sco_bdaddr, &src);
        if (::bind(ls, (sockaddr*)&la, sizeof la) < 0 || ::listen(ls, 1) < 0) { CLOG("vehicle SCO listen failed: " << strerror(errno)); ::close(ls); return; }
        CLOG("listening for call audio the VEHICLE opens (SCO) on " << veh_adapter_ << " (mode " << cfg_.bt_call_veh_sco << ")");
        while (run_) {
            pollfd p{ls, POLLIN, 0}; if (::poll(&p, 1, 200) <= 0) {
                int f = veh_sco_pending_.load(); if (f >= 0 && nowMs() - veh_sco_pending_ms_ > 15000) { if (veh_sco_pending_.compare_exchange_strong(f, -1)) { ::close(f); CLOG("vehicle-opened call audio not used within 15 s: closed"); } }
                continue; }
            sockaddr_sco ra; socklen_t l = sizeof ra; int vs = ::accept(ls, (sockaddr*)&ra, &l); if (vs < 0) continue;
            char a[18]; ba2str(&ra.sco_bdaddr, a); CLOG("the VEHICLE opened call audio (SCO) from " << a << " itself (mtu " << scoMtu(vs) << ")");
            int old = veh_sco_pending_.exchange(vs); veh_sco_pending_ms_ = nowMs(); if (old >= 0) ::close(old);
        }
        ::close(ls);
    }

    // ---- audio (SCO) -----------------------------------------------------------------------------------------------------------
    void closeAudio(const char* why) {
        std::lock_guard<std::mutex> g(audio_m_);
        if (!pump_.joinable()) return;
        audio_stop_ = true;
        if (std::this_thread::get_id() == pump_.get_id()) return;
        pump_.join();
        CLOG("call audio closed (" << why << ")");
    }
    int connectSco(const bdaddr_t& src, const bdaddr_t& dst) {
        int s = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_SCO);
        if (s < 0) { CLOG("SCO socket failed: " << strerror(errno)); return -1; }
        sockaddr_sco la; memset(&la, 0, sizeof la); la.sco_family = AF_BLUETOOTH; bacpy(&la.sco_bdaddr, &src);
        if (::bind(s, (sockaddr*)&la, sizeof la) < 0) { CLOG("SCO bind failed: " << strerror(errno)); ::close(s); return -1; }
        sockaddr_sco ra; memset(&ra, 0, sizeof ra); ra.sco_family = AF_BLUETOOTH; bacpy(&ra.sco_bdaddr, &dst);
        ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) | O_NONBLOCK);
        int r = ::connect(s, (sockaddr*)&ra, sizeof ra);
        if (r < 0 && errno != EINPROGRESS) { CLOG("SCO connect failed: " << strerror(errno)); ::close(s); return -1; }
        pollfd p{s, POLLOUT, 0};
        if (r < 0) { if (::poll(&p, 1, 6000) <= 0) { CLOG("SCO connect timed out"); ::close(s); return -1; }
            int err = 0; socklen_t l = sizeof err; ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
            if (err) { CLOG("SCO connect failed: " << strerror(err)); ::close(s); return -1; } }
        return s;
    }
    void scoAcceptLoop() {
        bdaddr_t src; if (!parseAddr(phone_adapter_, &src)) { CLOG("SCO: bad phone adapter address '" << phone_adapter_ << "'"); return; }
        int ls = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_SCO);
        if (ls < 0) { CLOG("SCO listen socket failed: " << strerror(errno)); return; }
        sockaddr_sco la; memset(&la, 0, sizeof la); la.sco_family = AF_BLUETOOTH; bacpy(&la.sco_bdaddr, &src);
        if (::bind(ls, (sockaddr*)&la, sizeof la) < 0 || ::listen(ls, 1) < 0) { CLOG("SCO listen failed: " << strerror(errno)); ::close(ls); return; }
        CLOG("listening for the phone's call audio (SCO) on " << phone_adapter_);
        while (run_) {
            pollfd p{ls, POLLIN, 0}; if (::poll(&p, 1, 200) <= 0) continue;
            sockaddr_sco ra; socklen_t l = sizeof ra; int ps = ::accept(ls, (sockaddr*)&ra, &l);
            if (ps < 0) continue;
            char a[18]; ba2str(&ra.sco_bdaddr, a); CLOG("phone opened call audio (SCO) from " << a);
            if (aa_) {                                                       // call audio over Android Auto: no vehicle Bluetooth link needed
                closeAudio("new call audio");
                std::lock_guard<std::mutex> g(audio_m_);
                if (pump_.joinable()) { audio_stop_ = true; pump_.join(); }
                audio_stop_ = false;
                CLOG("call audio up: phone -> Android Auto (vehicle sound + microphone)");
                pump_ = std::thread([this, ps] { const char* why = aa_->run(ps, audio_stop_); ::close(ps); CLOG("call audio (Android Auto) finished: " << why); });
                continue;
            }
            bridgeAudio(ps, "phone opened");
        }
        ::close(ls);
    }

    std::shared_ptr<AaCall> aa_;
    Config cfg_; hfp::CallCore core_; std::mutex m_, thr_m_, audio_m_;
    int phone_fd_ = -1, veh_fd_ = -1; std::string phone_addr_, veh_addr_, phone_adapter_, veh_adapter_;
    std::atomic<bool> run_{true}, audio_stop_{false}, phone_linked_{false};
    std::thread tick_, accept_, pump_; std::vector<std::thread> readers_; PumpStats stats_;
};

CallBridge::CallBridge(const Config& c) : impl_(std::make_unique<Impl>(c)) {}
CallBridge::~CallBridge() = default;
void CallBridge::setAdapters(const std::string& p, const std::string& v) { impl_->setAdapters(p, v); }
void CallBridge::onPhoneLink(int fd, const std::string& a) { impl_->onPhoneLink(fd, a); }
void CallBridge::onVehicleLink(int fd, const std::string& a) { impl_->onVehicleLink(fd, a); }
void CallBridge::setAaCall(std::shared_ptr<AaCall> a) { impl_->setAa(std::move(a)); }
bool CallBridge::phoneLinked() const { return impl_->phoneLinked(); }
void CallBridge::stop() { impl_->stop(); }
}  // namespace btcall
