#pragma once
// Touch-to-picture timing for the log: where does the Android Auto delay come from?
//   touch ... phone frame arrives  = truck->Pi->Wi-Fi->phone render+encode->Wi-Fi->Pi (outside our control)
//   touch ... encoded frame out    = the above plus our decode + composite + encode
// After every touch-DOWN the next few frames on each side are logged with their delta (ms) and size.
#include <atomic>
#include <chrono>
#include <cstdint>
inline int64_t probeNowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct LatencyProbe {
    std::atomic<int64_t> touch_ms{0};
    std::atomic<int> in_left{0}, out_left{0};
    static LatencyProbe& get() { static LatencyProbe p; return p; }
    void touched() { touch_ms = probeNowMs(); in_left = 8; out_left = 8; }
};
