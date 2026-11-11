#pragma once
// Every few seconds log which threads of this process burn the CPU (GStreamer names its streaming threads after the element, e.g.
// "openh264dec0:src", "x264enc0:src", "mix:src"), so a log shows where the Pi's time goes. Linux only; cheap (reads /proc/self/task/*).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

class ThreadCpuReporter {
public:
    void start(int period_s = 5) {
        run_ = true;
        th_ = std::thread([this, period_s] {
            const long hz = sysconf(_SC_CLK_TCK);
            std::map<std::string, unsigned long long> prev; auto t0 = std::chrono::steady_clock::now();
            while (run_) {
                for (int i = 0; i < period_s * 10 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!run_) break;
                std::map<std::string, unsigned long long> cur; std::map<std::string, std::string> names;
                if (DIR* d = opendir("/proc/self/task")) {
                    while (dirent* e = readdir(d)) {
                        if (e->d_name[0] == '.') continue;
                        std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/stat"); std::string line; std::getline(f, line);
                        size_t lp = line.find('('), rp = line.rfind(')'); if (lp == std::string::npos || rp == std::string::npos) continue;
                        std::string comm = line.substr(lp + 1, rp - lp - 1);
                        std::istringstream ss(line.substr(rp + 2)); std::string tok; unsigned long long ut = 0, st = 0;
                        for (int k = 3; ss >> tok; ++k) { if (k == 14) ut = std::stoull(tok); if (k == 15) { st = std::stoull(tok); break; } }
                        std::string key = std::string(e->d_name); cur[key] = ut + st; names[key] = comm;
                    }
                    closedir(d);
                }
                auto t1 = std::chrono::steady_clock::now(); double sec = std::chrono::duration<double>(t1 - t0).count(); t0 = t1;
                std::vector<std::pair<double, std::string>> v; double total = 0;
                for (auto& kv : cur) { auto it = prev.find(kv.first); if (it == prev.end()) continue;
                    double pct = 100.0 * (double)(kv.second - it->second) / (double)hz / sec; total += pct; if (pct >= 3.0) { std::ostringstream o; o << names[kv.first] << " " << (int)(pct + 0.5) << "%"; v.push_back({pct, o.str()}); } }
                prev = cur;
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
                std::ostringstream out; out << "[CPU] total " << (int)(total + 0.5) << "% of 400:"; for (size_t i = 0; i < v.size() && i < 7; ++i) out << "  " << v[i].second;
                std::cout << out.str() << std::endl;
            }
        });
    }
    void stop() { run_ = false; if (th_.joinable()) th_.join(); }
    ~ThreadCpuReporter() { stop(); }
private:
    std::atomic<bool> run_{false}; std::thread th_;
};
