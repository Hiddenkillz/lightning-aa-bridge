#pragma once
// HuSession: the Pi acting as the Android Auto HEAD UNIT toward the phone (TCP, wireless AA).
// Hand-written (like MdSession is for the phone role) so every message is explicit and testable.
//
//   HU -> phone VERSION_REQUEST(1)             plain     1.6
//   phone -> HU VERSION_RESPONSE(2)            plain
//   HU <-> phone ENCAPSULATED_SSL(3)           plain     TLS handshake, we are the TLS CLIENT (HU cert)
//   HU -> phone AUTH_COMPLETE(4)               plain     only when the handshake has FINISHED
//   phone -> HU SERVICE_DISCOVERY_REQUEST(5)   enc       we answer with video sink + input + sensors
//   phone -> HU CHANNEL_OPEN_REQUEST(7) per channel      we answer OK (control bit set)
//   video:  SETUP(0x8000) -> CONFIG(0x8003, READY, max_unacked 4); FOCUS_REQUEST(0x8007) -> FOCUS(0x8008 PROJECTED);
//           START(0x8001); MEDIA(0x0001 = SPS/PPS) and MEDIA_WITH_TIMESTAMP(0x0000) -> compositor, each frame ACKed (0x8004)
//   input:  BINDING_REQUEST(0x8002) -> RESPONSE(0x8003); we send InputReport(0x8001) touches
//   sensor: START_REQUEST(0x8001) -> RESPONSE(0x8002) + SensorBatch(0x8003): driving status 0, night mode off
//   control: PING answered, AUDIO/NAV focus requests answered (GAIN), BYEBYE answered then session ends
// Frame format identical to MdSession: [channel][flags][len16 BE] (+[total32] on FIRST), flags bit3 enc,
// bit2 control (set on service-channel control messages), bits0-1 frame type (3 bulk,1 first,0 mid,2 last).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../Config.h"
#include "../MsgRing.h"
#include "../TouchFrame.h"
#include "../VideoParams.h"

struct ssl_st;
struct ssl_ctx_st;
struct bio_st;

class HuSession {
public:
    // fd: connected stream socket to the phone. The session does not own the fd's lifetime beyond run().
    HuSession(const Config& config, int fd);
    ~HuSession();

    void run();                 // blocks until the phone disconnects / says bye / stop()
    void stop();

    using FrameSink = std::function<void(const uint8_t* data, size_t size, uint64_t pts_us)>;
    void setFrameSink(FrameSink s) { frame_sink_ = std::move(s); }
    // Flow control: video ACKs are held back while this reports more than `limit` bytes still waiting to be decoded, so the phone slows its
    // encoder instead of burying us (we used to ACK on arrival: with the decoder behind, the queue grew to seconds of lag). Forced after 1.5 s.
    void setQueueDepth(std::function<size_t()> f, size_t limit_bytes) { queue_depth_ = std::move(f); ack_limit_ = limit_bytes; }          // phone H.264 (config + pictures), Annex-B
    void setVideoGeometry(int w, int h) { vw_ = w; vh_ = h; }               // codec frame we advertise (default config.video_*)
    // The part of the codec frame we actually display (what the truck shows). Advertised to the phone as AAP margins + ui_config, and the
    // touchscreen size becomes the window size: the phone lays its UI out inside the window, and touches arrive in window pixels (as on Sync 4).
    void setVideoParams(const VideoParams& v) { vp_ = v; }          // the truck's density / viewing distance etc., passed on to the phone
    void setVisibleWindow(int x, int y, int w, int h) { win_x_ = x; win_y_ = y; win_w_ = w; win_h_ = h; }

    // Touch toward the phone, coordinates in the touchscreen space we advertise (== video size). action 0 down 1 up 2 move.
    void sendTouch(int x, int y, int action);
    void sendKey(int keycode, bool down, bool longpress);
    std::function<void(int ch, uint16_t id, const std::string& payload)> onOtherMessage;   // messages on channels this session has no handler for (tests: a fake vehicle's Bluetooth service)
    void sendRaw(int ch, uint16_t id, const std::string& payload) { enqueue(ch, true, id, payload); }   // a key (steering wheel / media) to the phone
    void sendTouchFrame(const TouchFrame& f);          // all fingers, phone coordinates (two-finger map zoom etc. pass through unchanged)

    bool videoStarted() const { return video_started_; }
    uint64_t framesReceived() const { return frames_rx_; }
    // Navigation relay (see NavRelay.h): raw messages from the phone's navigation channel, the channel-open event, and nav-focus requests (control channel)
    std::function<void(uint16_t id, const std::string& body)> onNavMessage;
    std::function<void()> onNavOpen;
    void requestUiConfig(int l, int r, int t, int b) { std::lock_guard<std::mutex> g(ui_m_); if (!video_started_) { pending_ui_[0] = l; pending_ui_[1] = r; pending_ui_[2] = t; pending_ui_[3] = b; has_pending_ui_ = true; return; } sendUiConfig(l, r, t, b); }   // before the video channel runs the SSL session may not be ready: truck 34 crashed in SSL_write
    void flushPendingUiConfig() { std::lock_guard<std::mutex> g(ui_m_); if (has_pending_ui_) { has_pending_ui_ = false; sendUiConfig(pending_ui_[0], pending_ui_[1], pending_ui_[2], pending_ui_[3]); } }   // HU -> phone: new UI margins (Split layout)
    std::function<void()> onUiConfigReply;                                          // the phone answered an UpdateUiConfigRequest
    std::function<void(const std::string& body)> onNavFocus;
    void sendNav(uint16_t id, const std::string& body) { enqueue(CH_NAV, true, id, body); }
    // Media playback status relay (RawRelay): the phone's now-playing messages, and the channel-open event
    std::function<void(uint16_t id, const std::string& body)> onMediaMessage;
    std::function<void()> onMediaOpen;
    void sendMedia(uint16_t id, const std::string& body) { enqueue(CH_MEDIA_STATUS, true, id, body); }
    // Audio / microphone / control relay (see AvRelay.h). stream: 0 = MEDIA, 1 = GUIDANCE (the system-audio channel is acknowledged and dropped).
    std::function<void(int stream, int session)> onAudioStart;
    std::function<void(int stream)> onAudioStop;
    std::function<void(int stream, uint64_t ts_us, const uint8_t* pcm, size_t n)> onAudioData;
    std::function<void(bool open)> onMicRequest;                                    // the phone opens/closes the vehicle microphone
    std::function<void(uint16_t id, const std::string& body)> onControlRaw;         // control messages the phone sends that the vehicle should see (focus request, battery, call availability)
    void sendMic(uint64_t ts_us, const uint8_t* pcm, size_t n);                     // microphone PCM to the phone (AVInput channel)
    void sendControl(uint16_t id, const std::string& body) { enqueue(0, true, id, body); }   // raw control message to the phone (voice button, focus notification)
    void onMic(uint16_t id, const uint8_t* p, size_t n);

private:
    enum FrameType : uint8_t { FT_MIDDLE = 0, FT_FIRST = 1, FT_LAST = 2, FT_BULK = 3 };
    static constexpr uint8_t FLAG_CONTROL = 0x04, FLAG_ENCRYPTED = 0x08;
    static constexpr int CH_NAV = 8, CH_MEDIA_STATUS = 9;
    static constexpr int CH_VIDEO = 1, CH_INPUT = 2, CH_SENSOR = 3, CH_AUD_MEDIA = 4, CH_AUD_GUIDE = 5, CH_AUD_SYS = 6, CH_MIC = 7;

    void readerLoop();
    void writerLoop();
    void parseIncoming();
    void onFrame(uint8_t ch, uint8_t flags, std::vector<uint8_t> payload);
    void onMessage(uint8_t ch, const std::vector<uint8_t>& msg);
    void onControl(uint16_t id, const uint8_t* p, size_t n);
    void onVideo(uint16_t id, const uint8_t* p, size_t n);
    void onInput(uint16_t id, const uint8_t* p, size_t n);
    void onSensor(uint16_t id, const uint8_t* p, size_t n);
public:
    // Value of the sensors reported to the phone (not to be confused with what the truck reports). Sent now if the phone already subscribed.
    void setPhoneSensors(int driving_status, bool night);
    // Vehicle sensor relay: sensor types the vehicle offers (the relayable ones are advertised to the phone), and raw SensorBatch messages from the vehicle.
    void setVehicleSensorTypes(const std::vector<int>& offered);
    void setVehicleSensorCaps(const std::vector<int>& fuel, const std::vector<int>& ev) { std::lock_guard<std::mutex> l(sensor_m_); relay_fuel_ = fuel; relay_ev_ = ev; }
    void relayVehicleSensors(const std::string& raw_batch);
    void sendSensorBatchRaw(const std::string& b) { enqueue(CH_SENSOR, true, 0x8003, b); }
private:
    void onAudio(uint8_t ch, uint16_t id, const uint8_t* p, size_t n);
    MsgRing ring_;
    int audio_session_[8] = {0};
    int mic_session_ = 1;

    void sendUiConfig(int l, int r, int t, int b);      // HU -> phone UpdateUiConfigRequest (video 0x8009): new UI margins
    void enqueue(uint8_t ch, bool enc, uint16_t id, const std::string& body, uint8_t extra = 0);
    void enqueueRaw(uint8_t ch, bool enc, std::vector<uint8_t> plain, uint8_t extra = 0);
    void sendServiceDiscoveryResponse();
    void endSession(const std::string& why);

    bool initSsl();
    void startTls();
    void onSslHandshake(const uint8_t* p, size_t n);
    std::vector<uint8_t> sslDecrypt(const std::vector<uint8_t>& ct);
    bool sslEncrypt(const uint8_t* p, size_t n, std::vector<uint8_t>& ct);
    void drainWriteBio(std::vector<uint8_t>& out);

    Config cfg_;
    int fd_;
    int vw_, vh_;
    VideoParams vp_;
    int win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;      // 0 = whole frame
    std::atomic<bool> running_{false};
    std::thread writer_;
    std::thread touch_flusher_;
    std::thread probe_thr_;
    bool probe_started_ = false;
    // Touch MOVE coalescing: Sync 4 reports a drag every ~6 ms (170/s); the phone falls behind (AA-only lag, then drops the link). Moves are
    // limited to ~60/s, newest position wins; DOWN/UP/POINTER events are never delayed and flush any pending move first.
    std::mutex touch_m_;
    TouchFrame touch_pending_; bool touch_has_pending_ = false; int64_t touch_last_sent_ms_ = 0;
    void emitTouch(const TouchFrame& f);
    void touchFlusherLoop();
    void flushAcks();
    std::function<size_t()> queue_depth_; size_t ack_limit_ = 100000;
    std::mutex ack_m_; std::deque<int64_t> ack_ts_; uint64_t acks_forced_ = 0; size_t ack_backlog_max_ = 0;
    FrameSink frame_sink_;

    std::vector<uint8_t> rx_acc_;

    long last_rx_ms_ = 0, rx_msgs_ = 0;                 // for the close-reason log line
    std::vector<uint8_t> reassembly_[256];

    struct OutFrame { std::vector<uint8_t> bytes; };
    std::deque<OutFrame> tx_;
    // Serialises 'encrypt + queue' per message. Without it two threads can take consecutive TLS sequence numbers and queue them in the opposite order: the peer then sees
    // records out of order and drops the session with 'bad record mac' (tests/tls_order_test.cpp reproduces it). Lock order: enc_m_ -> ssl_m_ / tx_m_ (both leaf locks).
    std::mutex enc_m_;
    std::mutex tx_m_;
    std::condition_variable tx_cv_;

    ssl_ctx_st* ctx_ = nullptr;
    ssl_st* ssl_ = nullptr;
    bio_st* rbio_ = nullptr;
    bio_st* wbio_ = nullptr;
    std::mutex ssl_m_;
    bool tls_done_ = false;

    int session_id_ = 0;
    std::atomic<bool> video_started_{false};
    std::atomic<uint64_t> frames_rx_{0};
    std::atomic<bool> input_open_{false};
    // Sensors toward the phone: the phone asks for each type (START_REQUEST); we answer with the CURRENT value and push every change.
    bool sensor_driving_ = false, sensor_night_ = false;
    std::mutex ui_m_; bool has_pending_ui_ = false; int pending_ui_[4] = {0, 0, 0, 0};
    bool relay_types_known_ = false;                                 // false until the vehicle's sensor list arrived: then the usual set is offered (the phone may connect first)
    std::vector<int> relay_fuel_, relay_ev_;                         // the vehicle's declared fuel types / EV connector types (an electric truck is an EV to Maps only if the phone is told)
    std::vector<int> relay_types_;                                   // relayable sensor types the vehicle offers (advertised to the phone)
    std::map<int, bool> relay_started_; std::map<int, int64_t> relay_last_ms_; std::map<int, std::string> relay_cache_; long relay_sent_ = 0;     // the phone subscribed to this type
    int driving_status_ = 0; bool night_ = false;            // current values (DrivingStatus mask, night mode)
    std::mutex sensor_m_;
};
