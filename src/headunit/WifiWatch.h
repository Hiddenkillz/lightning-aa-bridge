// WifiWatch: copies hostapd's station events (join / leave / AP up) into the main log, next to the [Bt] and [HuSession] lines, so one file tells the whole phone story.
// Why: truck 33 showed the phone "joined Wi-Fi" yet never opened the TCP session (twice), and the Pi had no idea whether the phone associated, was kicked, or left on its own.
// hostapd already writes /var/log/aa-bridge-hostapd.log (setup script, -f); this only reads it (starting at its current end), so it cannot disturb the AP. Read-only, one thread.
#pragma once
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

class WifiWatch {
public:
    explicit WifiWatch(std::string path = "/var/log/aa-bridge-hostapd.log") : path_(std::move(path)) {}
    ~WifiWatch() { stop(); }
    void start() { running_ = true; th_ = std::thread([this] { loop(); }); }
    void stop() { running_ = false; if (th_.joinable()) th_.join(); }
    // keep only what matters; returns "" for noise (exposed for the unit test)
    static std::string interesting(const std::string& line) {
        static const char* keys[] = {"AP-ENABLED", "AP-DISABLED", "AP-STA-CONNECTED", "AP-STA-DISCONNECTED", "IEEE 802.11: associated", "IEEE 802.11: disassociated",
                                     "IEEE 802.11: deauthenticated", "IEEE 802.11: authenticated", "HT_SCAN", "EAPOL-4WAY-HS-COMPLETED", "WPA: received EAPOL-Key", "no ACK", "did not acknowledge", "timeout"};
        for (const char* k : keys) if (line.find(k) != std::string::npos) {
            // drop the leading epoch timestamps: hostapd prints "<epoch>: [<epoch>: ]wlan0: ..."
            auto p = line.find("wlan0:"); return p == std::string::npos ? line : line.substr(p + 7);
        }
        return "";
    }
private:
    void loop() {
        std::ifstream f; std::streampos pos = -1;
        while (running_) {
            if (!f.is_open()) { f.open(path_); if (!f.is_open()) { sleepMs(1000); continue; } f.seekg(0, std::ios::end); pos = f.tellg(); }
            f.clear(); f.seekg(0, std::ios::end); std::streampos end = f.tellg();
            if (end < pos) pos = 0;                                  // file was rotated/truncated
            f.seekg(pos); std::string line;
            while (running_ && std::getline(f, line)) { if (f.eof()) break; const std::string s = interesting(line); if (!s.empty()) std::cout << "[Wifi] " << s << std::endl; pos = f.tellg(); }
            sleepMs(250);
        }
    }
    static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    std::string path_; std::atomic<bool> running_{false}; std::thread th_;
};
