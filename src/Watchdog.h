// Process-level safety nets so the box can sit in a truck unattended.
//  * LagGuard      : the mixer output timeline falling minutes behind real time (truck 27/32) is unusable and does not heal; restart the process
//                    (USB re-enumerates, pipelines are rebuilt) once the lag has stayed above a limit for a while.
//  * SdNotify      : raw systemd notify (no libsystemd): READY=1 / WATCHDOG=1 over $NOTIFY_SOCKET.
//  * HangWatchdog  : pings systemd's watchdog only while the video pipeline is still producing frames; if everything wedges (deadlock, D-state, GStreamer hang)
//                    the pings stop and systemd kills and restarts the service (WatchdogSec= in the unit).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

inline std::atomic<uint64_t> g_frame_progress{0};       // bumped for every encoded frame leaving the compositor

class LagGuard {
public:
    LagGuard(int64_t limit_ms, int64_t hold_ms) : limit_(limit_ms), hold_(hold_ms) {}
    // returns true once lag has been > limit continuously for hold_ms. lag < 0 means "unknown": ignored (does not reset).
    bool feed(int64_t lag_ms, int64_t now_ms) {
        if (limit_ <= 0 || lag_ms < 0) return false;
        if (lag_ms <= limit_) { over_since_ = -1; return false; }
        if (over_since_ < 0) over_since_ = now_ms;
        return now_ms - over_since_ >= hold_;
    }
private:
    int64_t limit_, hold_, over_since_ = -1;
};

inline bool sdNotify(const std::string& msg, const char* sock_path_override = nullptr) {
    const char* p = sock_path_override ? sock_path_override : getenv("NOTIFY_SOCKET");
    if (!p || !*p) return false;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return false;
    sockaddr_un a{}; a.sun_family = AF_UNIX; size_t n = strlen(p); if (n >= sizeof a.sun_path) { ::close(fd); return false; }
    memcpy(a.sun_path, p, n); if (a.sun_path[0] == '@') a.sun_path[0] = '\0';        // abstract socket
    ssize_t r = ::sendto(fd, msg.data(), msg.size(), 0, (sockaddr*)&a, (socklen_t)(offsetof(sockaddr_un, sun_path) + n));
    ::close(fd); return r == (ssize_t)msg.size();
}

class HangWatchdog {
public:
    // stall_s: no frame progress for this long => stop pinging (systemd then restarts us after WatchdogSec)
    HangWatchdog(int stall_s, std::function<uint64_t()> progress = [] { return g_frame_progress.load(); }, const char* sock = nullptr)
        : stall_s_(stall_s), progress_(std::move(progress)), sock_(sock ? sock : "") {}
    ~HangWatchdog() { stop(); }
    void start() { running_ = true; th_ = std::thread([this] { loop(); }); }
    void stop() { running_ = false; if (th_.joinable()) th_.join(); }
    int pings() const { return pings_; }
    int withheld() const { return withheld_; }
private:
    void loop() {
        using clk = std::chrono::steady_clock;
        uint64_t last = progress_(); auto last_move = clk::now(); bool ready_sent = false;
        while (running_) {
            const uint64_t cur = progress_(); const auto now = clk::now();
            if (cur != last) { last = cur; last_move = now; }
            const bool alive = std::chrono::duration_cast<std::chrono::seconds>(now - last_move).count() < stall_s_;
            const char* sp = sock_.empty() ? nullptr : sock_.c_str();
            if (!ready_sent) { sdNotify("READY=1", sp); ready_sent = true; }
            if (alive) { sdNotify("WATCHDOG=1", sp); ++pings_; }
            else { if (withheld_++ == 0) std::cout << "[Watchdog] no encoded frame for " << stall_s_ << " s: systemd watchdog pings withheld, the service will be restarted" << std::endl; }
            for (int i = 0; i < 20 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    int stall_s_; std::function<uint64_t()> progress_; std::string sock_;
    std::atomic<bool> running_{false}; std::thread th_; std::atomic<int> pings_{0}, withheld_{0};
};
