// RawRelay: carries one AA channel between the two sessions untouched (id + payload), both directions, once both sides have the channel open.
// Used for the media playback status service (now-playing metadata the vehicle shows on its cluster / media screen). No Start/Stop semantics (that is NavRelay).
#pragma once
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

class RawRelay {
public:
    using Send = std::function<void(uint16_t id, const std::string& body)>;
    explicit RawRelay(std::string tag) : tag_(std::move(tag)) {}
    void setTruckSend(Send s) { std::lock_guard<std::mutex> l(m_); truck_send_ = std::move(s); }     // set when the vehicle's channel is open; {} when gone
    void setPhoneSend(Send s) { std::lock_guard<std::mutex> l(m_); phone_send_ = std::move(s); if (!phone_send_) phone_open_ = false; }
    void phoneChannelOpen() { { std::lock_guard<std::mutex> l(m_); phone_open_ = true; } log("phone opened the channel"); }
    void fromPhone(uint16_t id, const std::string& body) {
        Send t; bool fwd; { std::lock_guard<std::mutex> l(m_); t = truck_send_; fwd = (bool)t; ++from_phone_; if (!fwd) ++dropped_; }
        logMsg("phone->vehicle", id, body, fwd); if (fwd) t(id, body);
    }
    void fromTruck(uint16_t id, const std::string& body) {
        Send p; bool fwd; { std::lock_guard<std::mutex> l(m_); p = phone_send_; fwd = phone_open_ && p; ++from_truck_; }
        logMsg("vehicle->phone", id, body, fwd); if (fwd) p(id, body);
    }
    long fromPhoneCount() const { std::lock_guard<std::mutex> l(m_); return from_phone_; }
    long droppedCount() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
private:
    static std::string hex(const std::string& b, size_t max) { static const char* d = "0123456789abcdef"; std::string o; for (size_t i = 0; i < b.size() && i < max; ++i) { o += d[(uint8_t)b[i] >> 4]; o += d[(uint8_t)b[i] & 15]; } if (b.size() > max) o += "..."; return o; }
    void log(const std::string& s) { std::cout << "[" << tag_ << "] " << s << std::endl; }
    void logMsg(const char* dir, uint16_t id, const std::string& body, bool fwd) {
        { std::lock_guard<std::mutex> l(m_); if (logged_ >= 2000) return; ++logged_; }
        std::ostringstream o; o << dir << " id=0x" << std::hex << id << std::dec << " len=" << body.size() << (fwd ? "" : " (NOT forwarded)") << " raw=" << hex(body, 120); log(o.str());
    }
    mutable std::mutex m_; std::string tag_; Send truck_send_, phone_send_; bool phone_open_ = false; long from_phone_ = 0, from_truck_ = 0, dropped_ = 0, logged_ = 0;
};
