#pragma once
// Copies call audio between two SCO sockets (any datagram/stream fds) in both directions. SCO is isochronous: every read is one packet (e.g. 60 bytes
// of 8 kHz CVSD = 3.75 ms) and is written to the other side as-is. A side that cannot take data right now loses that packet (never blocks the other).
#include <poll.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace btcall {
struct PumpStats { std::atomic<long> a_to_b{0}, b_to_a{0}, dropped{0}; std::atomic<int> err{0}; std::atomic<int> mtu_a{0}, mtu_b{0}; std::atomic<int> rx_a{0}, rx_b{0}; };   // rx_*: first packet size seen FROM that side

// SCO packet size of a socket (0 when it is not a SCO socket or the query fails)
inline int scoMtu(int fd) { sco_options o; socklen_t l = sizeof o; if (::getsockopt(fd, SOL_SCO, SCO_OPTIONS, &o, &l) == 0 && o.mtu > 0 && o.mtu <= 1024) return (int)o.mtu; return 0; }

// Runs until either side closes, `stop` becomes true, or an unrecoverable error. fds are NOT closed here. Returns a short reason string.
// Each direction is re-chunked to the DESTINATION's SCO packet size (mtu_a for packets written to a, mtu_b for packets written to b; 0 = query the socket,
// and if that fails pass packets through unchanged). Leftover bytes wait for the next packet (at most one packet of latency).
// mtu_a / mtu_b: >0 = fixed packet size for that side; 0 = the socket's reported mtu; -1 = AUTO: the size of the first packet that side sends us
// (a USB dongle delivers CVSD audio in 48-byte units and needs the same on the way in; the kernel reports 64, the controller buffer size), until then the socket mtu.
inline const char* pumpAudio(int a, int b, std::atomic<bool>& stop, PumpStats& st, int idle_timeout_ms = 0, int mtu_a = 0, int mtu_b = 0) {
    uint8_t buf[1024];
    const bool auto_a = mtu_a < 0, auto_b = mtu_b < 0;
    if (mtu_a <= 0) mtu_a = scoMtu(a); if (mtu_b <= 0) mtu_b = scoMtu(b);
    st.mtu_a = mtu_a; st.mtu_b = mtu_b; st.rx_a = 0; st.rx_b = 0;                   // the stats object is reused call after call: learn the sizes afresh every session (ui42a: 2nd call fell back to 64)
    bool wait_a = auto_a, wait_b = auto_b;                                             // auto: hold bytes for a side until we have seen a packet FROM it (its size), 500 ms at most
    const auto t_start = std::chrono::steady_clock::now();
    std::vector<uint8_t> acc_to_a, acc_to_b;                                          // bytes waiting to be sent to a / to b
    auto lastData = std::chrono::steady_clock::now();
    auto push = [&](int to, int mtu, std::vector<uint8_t>& acc, const uint8_t* d, size_t n, std::atomic<long>& counter, bool& waiting) {
        if (waiting) {
            if (std::chrono::steady_clock::now() - t_start < std::chrono::milliseconds(500)) {
                acc.insert(acc.end(), d, d + n); if (acc.size() > 1024) acc.erase(acc.begin(), acc.begin() + (long)(acc.size() - 512)); return;
            }
            waiting = false;                                                            // nothing came from that side: fall back to the socket's mtu
        }
        if (mtu <= 0) {                                                                // not a SCO socket: pass through as-is
            if (!acc.empty()) { ssize_t w = ::send(to, acc.data(), acc.size(), MSG_DONTWAIT | MSG_NOSIGNAL); if (w == (ssize_t)acc.size()) counter += (long)w; else st.dropped += 1; acc.clear(); }
            if (!d || !n) return;
            ssize_t w = ::send(to, d, n, MSG_DONTWAIT | MSG_NOSIGNAL); if (w == (ssize_t)n) counter += (long)n; else st.dropped += 1; return;
        }
        if (d && n) acc.insert(acc.end(), d, d + n);
        size_t off = 0;
        while (acc.size() - off >= (size_t)mtu) {
            ssize_t w = ::send(to, acc.data() + off, (size_t)mtu, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w == mtu) counter += mtu; else st.dropped += 1;
            off += (size_t)mtu;
        }
        if (off) acc.erase(acc.begin(), acc.begin() + (long)off);
        if (acc.size() > (size_t)mtu * 8) acc.clear();                                  // never let a stalled side pile up
    };
    while (!stop) {
        pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
        int r = ::poll(p, 2, 100);
        if (r < 0) { if (errno == EINTR) continue; return "poll error"; }
        if (r == 0) {
            if (idle_timeout_ms > 0 && std::chrono::steady_clock::now() - lastData > std::chrono::milliseconds(idle_timeout_ms)) return "idle";
            continue;
        }
        for (int i = 0; i < 2; ++i) {
            if (p[i].revents & (POLLERR | POLLNVAL)) { int e = 0; socklen_t l = sizeof e; ::getsockopt(i == 0 ? a : b, SOL_SOCKET, SO_ERROR, &e, &l); st.err = e; return i == 0 ? "phone audio link error" : "vehicle audio link error"; }
            if (p[i].revents & POLLIN) {
                const int from = i == 0 ? a : b;
                ssize_t n = ::read(from, buf, sizeof buf);
                if (n == 0) return i == 0 ? "side A closed" : "side B closed";
                if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; return i == 0 ? "side A read error" : "side B read error"; }
                lastData = std::chrono::steady_clock::now();
                if (i == 0) { if (!st.rx_a) { st.rx_a = (int)n; if (auto_a && n > 0 && n <= 1024) { mtu_a = (int)n; st.mtu_a = mtu_a; } wait_a = false; if (!acc_to_a.empty()) push(a, mtu_a, acc_to_a, nullptr, 0, st.b_to_a, wait_a); } }
                else        { if (!st.rx_b) { st.rx_b = (int)n; if (auto_b && n > 0 && n <= 1024) { mtu_b = (int)n; st.mtu_b = mtu_b; } wait_b = false; if (!acc_to_b.empty()) push(b, mtu_b, acc_to_b, nullptr, 0, st.a_to_b, wait_b); } }
                if (i == 0) push(b, mtu_b, acc_to_b, buf, (size_t)n, st.a_to_b, wait_b); else push(a, mtu_a, acc_to_a, buf, (size_t)n, st.b_to_a, wait_a);
            } else if (p[i].revents & POLLHUP) return i == 0 ? "side A hangup" : "side B hangup";
        }
    }
    return "stopped";
}
}  // namespace btcall
