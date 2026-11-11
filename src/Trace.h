#pragma once
// Protocol trace gate. Every received/sent AAP frame used to be logged (hex dumps): 81% of all log bytes, ~9 KB/s of constant SD-card writes
// (measured on truck test 25: 7 of 8.8 MB per 16 min). Day to day those lines are noise, so the trace is OFF by default and each session
// gets a small free budget at its start so the handshake is still fully logged. `log_trace=1` in aa-bridge.conf turns it all back on.
#include <atomic>
namespace trace {
inline std::atomic<bool> enabled{false};
inline std::atomic<int> budget{0};
inline void newSession(int free_lines = 600) { budget = free_lines; }
inline bool ok() {
    if (enabled.load(std::memory_order_relaxed)) return true;
    int b = budget.load(std::memory_order_relaxed);
    while (b > 0) if (budget.compare_exchange_weak(b, b - 1)) return true;
    return false;
}
}  // namespace trace
