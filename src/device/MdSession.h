#pragma once
// MdSession: the Pi acting as an Android phone ("MD", mobile device) toward
// the car head unit over the two FunctionFS bulk endpoints.
//
// This is the phone-role half of the Android Auto protocol, which aasdk does
// NOT implement (aasdk is head-unit only). The flow is taken from the one
// public implementation known to have worked against a real car,
// tomasz-grobelny/AACS (AAServer/src/AaCommunicator.cpp), and cross-checked
// against aasdk's head-unit side so both ends agree:
//
//   HU -> MD  VERSION_REQUEST (1)           plain
//   MD -> HU  VERSION_RESPONSE (2)          plain  (major=1 minor=5 status=0)
//   HU <-> MD ENCAPSULATED_SSL (3)          plain  TLS handshake, MD is the TLS SERVER
//   HU -> MD  AUTH_COMPLETE (4)             plain
//   MD -> HU  SERVICE_DISCOVERY_REQUEST (5) enc
//   HU -> MD  SERVICE_DISCOVERY_RESPONSE(6) enc    (lists the HU's channels; we pick the video sink)
//   MD -> HU  CHANNEL_OPEN_REQUEST (7)      enc    on the video channel
//   HU -> MD  CHANNEL_OPEN_RESPONSE (8)     enc
//   MD -> HU  MEDIA_SETUP (0x8000)          enc    type = H264
//   HU -> MD  MEDIA_CONFIG (0x8003)         enc
//   HU -> MD  VIDEO_FOCUS_NOTIFICATION (0x8008)    focus = PROJECTED
//   MD -> HU  MEDIA_START (0x8001)          enc
//   MD -> HU  MEDIA_DATA (0x0000)           enc    8-byte BE timestamp(us) + H.264 AU, repeat
//   HU -> MD  MEDIA_ACK (0x8004)            enc
//   HU -> MD  PING_REQUEST (11) / MD -> HU PING_RESPONSE (12) throughout
//
// Wire frame: [channel:1][flags:1][len:2 BE][payload]; First-fragment frames
// carry an extra [total_len:4 BE] before the payload. flags: bit3 = encrypted,
// bits0-1 = frame type (3 whole, 1 first, 0 middle, 2 last). Encryption is a
// TLS record per frame (SSL_write per chunk, ciphertext goes on the wire).
//
// The session runs by itself with no phone attached: whatever the compositor
// feeds sendVideo() (camera-only, or camera over the phone's AA video) is what
// the truck shows. That is the whole point of this project.

#include "../Config.h"
#include "../MsgRing.h"
#include "../TouchFrame.h"
#include "../VideoParams.h"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ssl_st;
struct ssl_ctx_st;
struct bio_st;

class MdSession {
public:
    MdSession(const Config& config, int read_fd, int write_fd);
    ~MdSession();

    // Runs the session until the head unit disconnects, the HU says bye,
    // or stop() is called. Blocks. Returns when the session has ended.
    void run();
    std::function<void()> onDead;                                          // called once from the watchdog thread when the truck is declared dead (Device: unbind the UDC to wake blocked I/O)
    void checkSilence();                                                   // truck-silence watchdog (called from the reader loop)
    void stop();
    bool endedBySilence() const { return ended_by_silence_; }      // the session was ended by the truck-silence watchdog (the caller re-attaches USB)

    // Called from the compositor's output thread. Dropped until the video
    // channel is started.
    void sendVideo(const uint8_t* data, size_t size, uint64_t pts_us);
    bool videoReady() const { return video_ready_; }

private:
    // ---- wire framing ----
    enum FrameType : uint8_t { FT_MIDDLE = 0, FT_FIRST = 1, FT_LAST = 2, FT_BULK = 3 };
    static constexpr uint8_t FLAG_ENCRYPTED = 0x08;
    static constexpr size_t  kMaxPlainChunk = 8 * 1024;   // per TLS record / frame
    static constexpr size_t  kReadBuf       = 64 * 1024;

    void readerLoop();
    void writerLoop();
    void parseIncoming();                                  // consumes rx_acc_
    void onFrame(uint8_t channel, uint8_t flags, std::vector<uint8_t> payload);
    std::vector<int> pending_open_;
    void openChannel(int id);
    void onMessage(uint8_t channel, const std::vector<uint8_t>& msg);
    void onControlMessage(uint16_t id, const uint8_t* p, size_t n);
    void onVideoMessage(uint16_t id, const uint8_t* p, size_t n);

    void enqueue(uint8_t channel, bool encrypted, uint16_t msg_id,
                 const std::string& body, uint8_t extra = 0);
    void enqueueRaw(uint8_t channel, bool encrypted, std::vector<uint8_t> plain, uint8_t extra = 0);
    int input_channel_ = -1;
public:
    // Touch from the truck: x,y in the truck touchscreen's coordinate space, action 0=down 1=up 2=move
    using TouchHandler = std::function<void(int x, int y, int action)>;
    void setTouchHandler(TouchHandler h) { touch_handler_ = std::move(h); }
    using TouchFrameHandler = std::function<void(const TouchFrame&)>;   // full multi-finger event (preferred over TouchHandler)
    void setTouchFrameHandler(TouchFrameHandler h) { touch_frame_handler_ = std::move(h); }
    void setKeyHandler(std::function<void(int, bool, bool, int)> h) { key_handler_ = std::move(h); }   // steering-wheel / media keys the vehicle sends (needs the binding request)
    const std::string& bindingRequestBytes() const { return binding_req_; }
    int touchWidth() const { return touch_w_; }
    int touchHeight() const { return touch_h_; }
    // Vehicle state from the truck's sensor channel: what 1 = reverse (0/1), 2 = moving (0/1)
    // Also: 3 gear (aap Gear enum value), 4 parking brake (0/1), 5 speed (m/s*1000), 6 driving status (DrivingStatus mask), 7 night mode (0/1)
    std::function<void(int what, int value)> onVehicle;
    int last_speed_e3_ = -100000;
    MsgRing ring_;
    int nav_channel_ = -1; std::atomic<bool> nav_open_{false};
    int media_channel_ = -1; std::atomic<bool> media_open_{false};
    // ---- audio (vehicle's sink channels for MEDIA / GUIDANCE PCM, and its microphone source), config relay_audio
    struct AudioCh { int ch = -1; std::atomic<bool> ready{false}, started{false}, want_start{false}; std::atomic<int> unacked{0}; int max_unacked = 4; int session = 0; std::atomic<long> dropped{0}, sent{0}; };
    AudioCh aud_[2]; int mic_ch_ = -1; std::atomic<bool> mic_open_{false}; std::atomic<bool> mic_want_{false}; int32_t mic_session_ = 0; long mic_frames_ = 0;
    void onAudioChannelMessage(int idx, uint16_t id, const uint8_t* p, size_t n);
    void onMicChannelMessage(uint16_t id, const uint8_t* p, size_t n);
    // Fired once discovery is parsed: the part of the codec frame the head unit actually shows (AAP "UI resolution": codec frame minus
    // the advertised margins, clipped centred). x,y,w,h in codec-frame pixels; w==0 means the head unit advertised no margins.
    std::function<void(int x, int y, int w, int h)> onVisibleWindow;
    std::function<void(const VideoParams&)> onVideoParams;   // density etc. of the chosen video config
    // Navigation relay (vehicle's navigation_status_service channel). onNav: a message FROM the vehicle (Start/Stop); sendNav/sendNavFocus: toward the vehicle.
    std::function<void(uint16_t id, const std::string& body)> onNav;
    std::function<void(bool open)> onNavChannel;
    void sendNav(uint16_t id, const std::string& body);
    void sendNavFocus(const std::string& body);
    // Media playback status relay (vehicle's media_playback_service channel)
    std::function<void(uint16_t id, const std::string& body)> onMedia;
    std::function<void(bool open)> onMediaChannel;
    void sendMedia(uint16_t id, const std::string& body);
    // Audio toward the vehicle (stream 0 = MEDIA, 1 = GUIDANCE) and microphone from it. All no-ops when the vehicle offers no such channel.
    void startAudio(int stream, int session);
    void stopAudio(int stream);
    void sendAudio(int stream, uint64_t ts_us, const uint8_t* pcm, size_t n);       // dropped (counted) when the vehicle's flow-control window is full
    bool audioReady(int stream) const { return stream >= 0 && stream < 2 && aud_[stream].ready; }
    long audioSent(int stream) const { return aud_[stream].sent; }
    long audioDropped(int stream) const { return aud_[stream].dropped; }
    void micRequest(bool open);
    std::function<void(uint64_t ts_us, const uint8_t* pcm, size_t n)> onMicData;
    void sendControl(uint16_t id, const std::string& body);
    void flushPendingFocus(const char* why); std::string pending_focus_;   // AudioFocusRequest waiting for the MEDIA channel (see sendControl)                          // raw control-channel message toward the vehicle
    std::function<void(uint16_t id, const std::string& body)> onControlRaw;           // raw control message from the vehicle (voice button, audio focus)        // NavFocusRequestNotification (control channel), carried raw from the phone
    // Vehicle sensor relay: the sensor types the truck offers (fired once after discovery) and every raw SensorBatch it sends (relay_sensors).
    std::function<void(const std::vector<int>& offered)> onSensorTypes;
    std::function<void(const std::string& raw_batch)> onSensorRaw;
    std::function<void(const std::string& car_address)> onCarAddress;   // the vehicle's Bluetooth address, once we have announced ours
    std::function<std::string()> btAddress;       // Bluetooth address to tell the vehicle (the call-bridge dongle, else the onboard radio)
    std::function<void(const std::vector<int>& fuel_types, const std::vector<int>& ev_connectors)> onSensorCaps;   // what the vehicle declares (electric, CCS/J1772 ...)
    std::function<void(int w, int h)> onTouchGeometry;   // fired once discovery tells us the touchscreen size
private:
    TouchHandler touch_handler_;
    TouchFrameHandler touch_frame_handler_;
    int bt_channel_ = -1; std::string bt_car_address_; void onBtMessage(uint16_t id, const uint8_t* p, size_t n);
    std::function<void(int keycode, bool down, bool longpress, int meta)> key_handler_;
    int touch_w_ = 0, touch_h_ = 0;
    void onInputMessage(uint16_t id, const uint8_t* p, size_t n);
    void onSensorMessage(uint16_t id, const uint8_t* p, size_t n);
    int sensor_channel_ = -1;
    std::vector<int> sensor_types_;          // types the truck offers that we care about
    bool veh_reverse_ = false, veh_moving_ = false, veh_known_ = false;
    std::string sent_config_;
    std::string binding_req_;
    bool writeAll(const uint8_t* p, size_t n);

    // ---- TLS (we are the server) ----
    bool initSsl();
    void onSslHandshake(const uint8_t* p, size_t n);
    std::vector<uint8_t> sslDecrypt(const std::vector<uint8_t>& ct);
    bool sslEncrypt(const uint8_t* p, size_t n, std::vector<uint8_t>& ct);
    void drainWriteBio(std::vector<uint8_t>& out);

    void startVideoIfReady();
    void reclaimFocus();
    void endSession(const std::string& why);

    Config config_;
    int read_fd_, write_fd_;
    std::atomic<bool> running_{false};
    std::atomic<bool> video_ready_{false};
    std::thread reader_, writer_;

    std::vector<uint8_t> rx_acc_;
    std::vector<uint8_t> reassembly_[256];
    bool reassembling_[256] = {};

    struct OutFrame { std::vector<uint8_t> bytes; };
    std::deque<OutFrame> tx_;
    // Serialises 'encrypt + queue' per message. Without it two threads can take consecutive TLS sequence numbers and queue them in the opposite order: the peer then sees
    // records out of order and drops the session with 'bad record mac' (tests/tls_order_test.cpp reproduces it). Lock order: enc_m_ -> ssl_m_ / tx_m_ (both leaf locks).
    std::mutex enc_m_;
    std::mutex tx_m_;
    std::condition_variable tx_cv_;

    ssl_ctx_st* ctx_ = nullptr;
    ssl_st*     ssl_ = nullptr;
    bio_st*     rbio_ = nullptr;
    bio_st*     wbio_ = nullptr;
    std::mutex  ssl_m_;
    bool        tls_done_ = false;

    int  video_channel_ = -1;
    int  video_config_index_ = 0;
    bool video_ui_margins_ = false; int ui_l_ = 0, ui_r_ = 0, ui_t_ = 0, ui_b_ = 0;   // ui_config.margins of the chosen config
    int  video_codec_w_ = 0, video_codec_h_ = 0, video_wmargin_ = 0, video_hmargin_ = 0;   // of the chosen video config
    bool got_media_config_ = false;
    bool got_video_focus_ = false;
    bool focus_native_ = false; int focus_reqs_ = 0; std::chrono::steady_clock::time_point focus_lost_t_, focus_req_t_;   // focus_reclaim
    bool video_started_ = false;
    int  max_unacked_ = 1;
    std::atomic<int> unacked_{0};
    std::atomic<bool> ended_by_silence_{false};
    int64_t session_start_ms_ = 0;
    std::thread watchdog_;
    std::atomic<bool> reader_done_{false};
    std::atomic<int64_t> last_rx_ms_{0};                 // when the truck last sent us anything (silence detector)
    std::atomic<unsigned> frames_dropped_{0};           // pictures we skipped because the truck had too many un-ACKed (corrupts its decoder until the next IDR)
    int64_t last_hb_ms_ = 0;
    int quiet_dumps_ = 0;                                  // forensics dumps made in the current quiet episode (see dumpQuietForensics)
    std::chrono::steady_clock::time_point config_time_;
};
