#include "HeadUnit.h"
#include "HuSession.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

class HeadUnit::Impl {
public:
    Impl(const Config& c, std::shared_ptr<Compositor> comp) : cfg_(c), comp_(std::move(comp)) {}

    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(kPort);
        if (::bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0 || ::listen(listen_fd_, 2) < 0) {
            std::cerr << "[HeadUnit] cannot listen on :" << kPort << ": " << strerror(errno) << std::endl; return;
        }
        running_ = true;
        thread_ = std::thread([this] { acceptLoop(); });
        std::cout << "[HeadUnit] Listening on TCP :" << kPort << " (phone-facing HU role)" << std::endl;
    }
    void stop() {
        running_ = false;
        { std::lock_guard<std::mutex> lk(m_); if (cur_) cur_->stop(); }
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }
    void touchFrame(const TouchFrame& f) { std::lock_guard<std::mutex> lk(m_); if (cur_) cur_->sendTouchFrame(f); }
    void touch(int x, int y, int a) { std::lock_guard<std::mutex> lk(m_); if (cur_) cur_->sendTouch(x, y, a); }
    void setPresence(PresenceCallback cb) { presence_ = std::move(cb); }
    void setNav(std::shared_ptr<NavRelay> r) { nav_ = std::move(r); }
    void setMedia(std::shared_ptr<RawRelay> r) { media_ = std::move(r); }
    void setAv(std::shared_ptr<AvRelay> r) { av_ = std::move(r); }
    void sendUi(int l, int r, int t, int b) { std::shared_ptr<HuSession> s; { std::lock_guard<std::mutex> lk(m_); s = cur_; } if (s) s->requestUiConfig(l, r, t, b); }
    void setUiReply(std::function<void()> f) { std::lock_guard<std::mutex> lk(m_); ui_reply_ = std::move(f); }
    void key(int kc, bool down, bool lp) { std::shared_ptr<HuSession> s; { std::lock_guard<std::mutex> lk(m_); s = cur_; } if (s) s->sendKey(kc, down, lp); }
    void setSensorCaps(const std::vector<int>& f, const std::vector<int>& e) { std::lock_guard<std::mutex> lk(m_); veh_fuel_ = f; veh_ev_ = e; }
    void setSensorTypes(const std::vector<int>& t) { std::lock_guard<std::mutex> lk(m_); veh_sensors_ = t; }
    void relaySensors(const std::string& raw) { std::shared_ptr<HuSession> s; { std::lock_guard<std::mutex> lk(m_); s = cur_; } if (s) s->relayVehicleSensors(raw); }
    void setSensors(int d, bool n) { std::shared_ptr<HuSession> s; { std::lock_guard<std::mutex> lk(m_); drv_ = d; night_ = n; s = cur_; } if (s) s->setPhoneSensors(d, n); }
    void setVp(const VideoParams& v) { std::lock_guard<std::mutex> lk(m_); vp_ = v; }
    void setWindow(int x, int y, int w, int h) { std::lock_guard<std::mutex> lk(m_); wx_ = x; wy_ = y; ww_ = w; wh_ = h; }

private:
    static constexpr int kPort = 5277;
    void acceptLoop() {
        while (running_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 500) <= 0) continue;
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            { sockaddr_in pa{}; socklen_t pl = sizeof pa; char ip[32] = "?"; if (getpeername(fd, (sockaddr*)&pa, &pl) == 0) inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
              std::cout << "[HeadUnit] phone connected (fd " << fd << ", from " << ip << ")" << std::endl; }
            // One phone at a time: a new connection replaces the old one.
            std::shared_ptr<HuSession> old;
            { std::lock_guard<std::mutex> lk(m_); old = cur_; }
            if (old) old->stop();
            if (worker_.joinable()) worker_.join();
            auto s = std::make_shared<HuSession>(cfg_, fd);
            { std::lock_guard<std::mutex> lk(m_); s->onUiConfigReply = ui_reply_; s->setVehicleSensorTypes(veh_sensors_); s->setVehicleSensorCaps(veh_fuel_, veh_ev_); s->setVideoParams(vp_); if (ww_ > 0) s->setVisibleWindow(wx_, wy_, ww_, wh_); s->setPhoneSensors(drv_, night_); }
            s->setFrameSink([this](const uint8_t* d, size_t n, uint64_t pts) { comp_->pushPhoneFrame(d, n, pts); });
            s->setQueueDepth([this] { return comp_->phoneQueueBytes(); }, 100000);
            if (nav_) {
                std::weak_ptr<HuSession> ws = s; auto r = nav_;
                s->onNavMessage = [r](uint16_t id, const std::string& b) { r->fromPhone(id, b); };
                s->onNavOpen = [r]() { r->phoneChannelOpen(); };
                s->onNavFocus = [r](const std::string& b) { r->focusFromPhone(b); };
                r->setPhoneSend([ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendNav(id, b); });
            }
            if (media_) {
                std::weak_ptr<HuSession> ws = s; auto r = media_;
                s->onMediaMessage = [r](uint16_t id, const std::string& b) { r->fromPhone(id, b); };
                s->onMediaOpen = [r]() { r->phoneChannelOpen(); };
                r->setPhoneSend([ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendMedia(id, b); });
            }
            if (av_) {
                std::weak_ptr<HuSession> ws = s; auto r = av_;
                s->onAudioStart = [r](int st, int se) { r->phoneAudioStart(st, se); };
                s->onAudioStop = [r](int st) { r->phoneAudioStop(st); };
                s->onAudioData = [r](int st, uint64_t ts, const uint8_t* p, size_t n) { r->phoneAudioData(st, ts, p, n); };
                s->onMicRequest = [r](bool o) { r->phoneMicRequest(o); };
                s->onControlRaw = [r](uint16_t id, const std::string& b) { r->controlFromPhone(id, b); };
                r->setPhone([ws](uint64_t ts, const uint8_t* p, size_t n) { if (auto x = ws.lock()) x->sendMic(ts, p, n); },
                            [ws](uint16_t id, const std::string& b) { if (auto x = ws.lock()) x->sendControl(id, b); });
            }
            { std::lock_guard<std::mutex> lk(m_); cur_ = s; }
            worker_ = std::thread([this, s, fd] {
                if (presence_) presence_(true);
                s->run();
                if (nav_) nav_->setPhoneSend({});
                if (media_) media_->setPhoneSend({});
                if (av_) { av_->phoneAudioStop(0); av_->phoneAudioStop(1); av_->phoneMicRequest(false); av_->clearPhone(); }   // the vehicle must not be left with a stream or microphone nobody feeds
                { std::lock_guard<std::mutex> lk(m_); if (cur_ == s) cur_.reset(); }
                if (presence_) presence_(false);
                ::close(fd);
                std::cout << "[HeadUnit] phone session finished" << std::endl;
            });
        }
        std::shared_ptr<HuSession> s; { std::lock_guard<std::mutex> lk(m_); s = cur_; }
        if (s) s->stop();
        if (worker_.joinable()) worker_.join();
    }

    Config cfg_;
    std::shared_ptr<Compositor> comp_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_, worker_;
    std::mutex m_;
    std::shared_ptr<HuSession> cur_;
    PresenceCallback presence_;
    std::shared_ptr<NavRelay> nav_; std::shared_ptr<RawRelay> media_;
    std::shared_ptr<AvRelay> av_;
    std::vector<int> veh_sensors_, veh_fuel_, veh_ev_;
    std::function<void()> ui_reply_;
    VideoParams vp_;
    int wx_ = 0, wy_ = 0, ww_ = 0, wh_ = 0;
    int drv_ = 0; bool night_ = false;                     // sensor values for the phone (default: parked, daytime = unrestricted)
};

HeadUnit::HeadUnit(const Config& c, std::shared_ptr<Compositor> comp) : impl_(std::make_unique<Impl>(c, std::move(comp))) {}
HeadUnit::~HeadUnit() { stop(); }
void HeadUnit::start() { impl_->start(); }
void HeadUnit::stop() { impl_->stop(); }
void HeadUnit::setPresenceCallback(PresenceCallback cb) { impl_->setPresence(std::move(cb)); }
void HeadUnit::sendUiConfig(int l, int r, int t, int b) { impl_->sendUi(l, r, t, b); }
void HeadUnit::setUiConfigReplyCallback(std::function<void()> f) { impl_->setUiReply(std::move(f)); }
void HeadUnit::setVehicleSensorCaps(const std::vector<int>& f, const std::vector<int>& e) { impl_->setSensorCaps(f, e); }
void HeadUnit::setVehicleSensorTypes(const std::vector<int>& t) { impl_->setSensorTypes(t); }
void HeadUnit::relayVehicleSensors(const std::string& raw) { impl_->relaySensors(raw); }
void HeadUnit::setAvRelay(std::shared_ptr<AvRelay> r) { impl_->setAv(std::move(r)); }
void HeadUnit::setNavRelay(std::shared_ptr<NavRelay> r) { impl_->setNav(std::move(r)); }
void HeadUnit::setMediaRelay(std::shared_ptr<RawRelay> r) { impl_->setMedia(std::move(r)); }
void HeadUnit::setVisibleWindow(int x, int y, int w, int h) { impl_->setWindow(x, y, w, h); }
void HeadUnit::setVideoParams(const VideoParams& v) { impl_->setVp(v); }
void HeadUnit::setPhoneSensors(int d, bool n) { impl_->setSensors(d, n); }
void HeadUnit::injectTouchFrame(const TouchFrame& f) { impl_->touchFrame(f); }
void HeadUnit::injectKey(int keycode, bool down, bool longpress) { impl_->key(keycode, down, longpress); }
void HeadUnit::injectTouch(int x, int y, int a) { impl_->touch(x, y, a); }
