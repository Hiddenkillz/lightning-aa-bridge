// MsgRing: the last N protocol messages of a session (direction, channel, id, length, ms), dumped to the log when the session ends or the peer goes quiet.
// Why: "phone closed the connection" / "truck silent" told us THAT a session ended, never what was said just before. Cheap (a few string writes per message under a short lock).
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

class MsgRing {
public:
    void push(bool rx, int ch, unsigned id, size_t len) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> l(m_); Entry& e = e_[n_ % kN]; e = {ms, rx, ch, id, len}; ++n_;
    }
    // the last messages, oldest first, with their age relative to `now`
    std::string dump(const char* tag) const {
        std::lock_guard<std::mutex> l(m_); std::ostringstream o;
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t cnt = n_ < kN ? n_ : kN; o << "[" << tag << "] last " << cnt << " messages before the end (oldest first, age in ms):";
        for (size_t i = n_ - cnt; i < n_; ++i) { const Entry& e = e_[i % kN]; o << "  " << (e.rx ? "RX" : "TX") << " ch" << e.ch << " 0x" << std::hex << e.id << std::dec << " len" << e.len << " -" << (now - e.ms); }
        if (!cnt) o << " (none)";
        return o.str();
    }
    size_t total() const { std::lock_guard<std::mutex> l(m_); return n_; }
private:
    static constexpr size_t kN = 16;
    struct Entry { long long ms = 0; bool rx = false; int ch = 0; unsigned id = 0; size_t len = 0; };
    mutable std::mutex m_; std::array<Entry, kN> e_{}; size_t n_ = 0;
};
