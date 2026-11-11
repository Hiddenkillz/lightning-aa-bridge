#pragma once
#include <array>
// UiController: owns the display mode + prefs + on-screen menu. Applies layouts
// to the compositor, routes Sync 4 touches (menu | camera window | phone),
// persists prefs, and accepts text commands on a control FIFO.
//
// Control FIFO commands (one per line):  echo "mode pip" > /run/aa-bridge/control
//   mode aa|cam|split|pip      switch display mode now
//   start aa|cam|split|pip     "always start on" preference (saved)
//   corner tl|tr|bl|br | size s|m|l | split 0.25..0.75 | autocam 0|1   (saved)
//   roi x y w h  /  roi reset  camera safe zone, normalised 0..1 (saved)
//   phone 0|1 | camera 0|1     presence (until HeadUnit/RTSP report it)
//   reverse 0|1                gear = reverse (auto-camera when enabled)
//   moving 0|1                 vehicle moving (informational; the menu is never locked)
//   touch X Y d|m|u            inject a touch in truck-touch coordinates (remote testing)
//   menu open|close|settings   show / hide the pill, or open the Settings page
//   camfill 0|1 | mirror 0|1 | flip 0|1 | zoom 1..4 | zoomreset   camera view (saved)
//   start settings             "always start on Settings"
//   theme auto|dark|light      appearance (saved)
//   save                       persist prefs now
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include "../TouchFrame.h"
#include "../Config.h"
#include "../compositor/Compositor.h"
#include "Gfx.h"
#include "InputRouter.h"
#include "Layout.h"
#include "Menu.h"
#include "../obd/Obd.h"

namespace ui {

class UiController {
public:
    UiController(const Config& cfg, std::shared_ptr<Compositor> comp);
    ~UiController();

    void start();
    void stop();

    // state inputs
    void setPhonePresent(bool v);
    void setUiConfigSender(std::function<void(int l, int r, int t, int b)> f) { std::lock_guard<std::mutex> l(m_); ui_config_cb_ = std::move(f); }   // asks the phone for a new UI region
    void reshapeReply();                                                                                                                      // the phone answered UpdateUiConfig
    void onPhoneVideo();
    void setCameraPresent(bool v);
    void setReverse(bool v);
    void setMoving(bool v);
    // Everything the truck's sensor channel reports (what: 1 reverse, 2 moving, 3 gear enum, 4 parking brake, 5 speed m/s*1000, 6 driving-status mask, 7 night mode).
    void setTruckSensor(int what, int value);
    // What the phone is told about driving status / night mode (decided here: truck value or a Debug override). Called whenever either changes
    // and once when installed.
    void setPhoneSensorSink(std::function<void(int driving_status, bool night)> sink);
    // Battery preconditioning toggle (staged; only when config precondition_button=1). The sink is told every change the USER makes; setPreconditionState() is for
    // the other direction (e.g. the vehicle / Maps started it). State is never saved: always OFF after a restart.
    void setPreconditionSink(std::function<void(bool on)> sink) { std::lock_guard<std::mutex> l(m_); precond_sink_ = std::move(sink); }
    void setPreconditionState(bool on);
    // Vehicle data (OBD-II poller) for the Energy page: a fresh snapshot ~1/s. Only re-renders while the page is shown.
    void setEnergy(const obd::Snapshot& s);
    bool energyShown() const { return cfg_.obd_enable; }
    bool preconditionOn() const { std::lock_guard<std::mutex> l(m_); return precond_ != 0; }
    void setVisibleWindow(int x, int y, int w, int h);              // from the SDR video margins (AAP UI resolution); w==0: none advertised
    void setTouchGeometry(int touch_w, int touch_h);                 // from service discovery

    // touch path: Sync 4 -> here. Phone-bound touches go to the forwarder already remapped.
    using PhoneTouch = std::function<void(const TouchFrame&)>;     // every finger, phone-frame coordinates
    void setPhoneTouchForwarder(PhoneTouch f) { std::lock_guard<std::mutex> l(m_); forward_ = std::move(f); }
    void onTouch(int x, int y, int action);
    // Every touch event with ALL fingers (touch-space coordinates). Gestures that start on the phone are forwarded in full (two-finger map zoom
    // etc.); a pinch that starts on the PiP window resizes the PiP instead.
    void onTouchFrame(const TouchFrame& f);

    void setMode(Mode m);
    void command(const std::string& line);

    Mode mode() const { std::lock_guard<std::mutex> l(m_); return mode_; }
    Screen screen() const { std::lock_guard<std::mutex> l(m_); return menu_.screen(); }
    Rect buttonRect(int id) const {                     // window-space rect of a widget currently on screen (tests, diagnostics); empty if none
        std::lock_guard<std::mutex> l(m_);
        for (const auto& b : menu_.buttons()) if (b.id == id) return b.rect;
        return {};
    }
    void zoomLagTickLocked(int64_t now); int64_t zoom_lag_since_ms_ = 0; int zoom_lag_resets_ = 0;   // zoom self-heal (mixer lag with a zoomed camera)
    bool live_applied_ = false; Layout live_applied_v_;                           // last live layout given to the compositor (dedup)
    void setWindowCallback(std::function<void(int, int, int, int)> f) { std::lock_guard<std::mutex> l(m_); window_cb_ = std::move(f); }   // the vehicle's window changed (cached by main)
    std::function<void(int, int, int, int)> window_cb_;
    bool splitDragOn() const { return cfg_.split_drag || prefs_.split_drag; }   // config or the Debug toggle
    bool layoutCameraLive() const { std::lock_guard<std::mutex> l(m_); return layout_.camera.live; }
    bool layoutPhoneLive() const { std::lock_guard<std::mutex> l(m_); return layout_.phone.live; }
    int overlayAlphaAt(int x, int y) const {            // alpha of the window overlay at window pixel (x,y): 0 = see-through (tests: is the PiP hole where the camera is?)
        std::lock_guard<std::mutex> l(m_);
        if (x < 0 || y < 0 || x >= canvas_.w() || y >= canvas_.h()) return -1;
        return canvas_.data()[((size_t)y * canvas_.w() + x) * 4 + 3];
    }
    Rect phoneRect() const { std::lock_guard<std::mutex> l(m_); return layout_.phone.rect; }
    Rect cameraRect() const { std::lock_guard<std::mutex> l(m_); return layout_.camera.rect; }
    Prefs prefs() const { std::lock_guard<std::mutex> l(m_); return prefs_; }

    static bool loadPrefs(const std::string& path, Prefs& p);
    static bool savePrefs(const std::string& path, const Prefs& p);

private:
    // *Locked helpers require m_ held
    void applyLocked();
    void renderOverlayLocked();
    void repushOverlayLocked();       // 1 Hz keepalive: re-send the last overlay without re-rendering
    void renderCalibLocked();
    bool pinchLocked(const TouchFrame& f);
    void onTouchLocked(int x, int y, int action, const TouchFrame* frame);
    void updateBaseLocked();          // Android Auto splash as the video base layer (when the phone pane is wanted but not live)
    std::array<int, 7> base_key_{}; bool base_key_valid_ = false;
    void applyLayoutOnlyLocked();     // compute + push the layout (no logging / overlay / status): cheap enough for live gestures
    void setVisibleLocked(int x, int y, int w, int h);
    void windowChangedLocked(bool changed);               // tell a live phone the new layout, cache the window   // the part of the output frame the head unit actually shows
    Model modelLocked() const;
    void applyIntentLocked(const Intent& it);
    void handleUiTouchLocked(const Routed& r);
    void panLocked(int ox, int oy, int action);
    void dragPipLocked(int ox, int oy, int action);           // one-finger drag of the PiP window: follows the finger, snaps to a corner on release
    enum class PipDrag { None, Pending, Dragging } pipd_ = PipDrag::None; int pipd_sx_ = 0, pipd_sy_ = 0, pipd_rx_ = 0, pipd_ry_ = 0; int64_t pipd_render_ms_ = 0, pipd_gap_ms_ = 80; bool pipd_dirty_ = false;
    void commandLocked(const std::string& line, bool& save, bool& ok);
    void splitDragLocked(int ox, int oy, Action a);            // the Split divider: follows the finger (phone pane = placeholder), commits 700 ms after release
    void splitCommitLocked(int64_t now);
    // Split divider drag state. split_hold_: keep the phone pane on its placeholder until the phone has re-laid its UI out for the new pane (or gave up).
    bool split_drag_ = false, split_hold_ = false; double split_frac_ = 0.5; int split_edge_ = 0; int64_t split_commit_ms_ = 0; bool split_pending_ = false; int64_t split_render_ms_ = 0;
    Mode shown_mode_ = Mode::FullCam; int64_t mode_changed_ms_ = -1000000;   // for the mode toast
    std::string build_tag_;                                    // /etc/aa-bridge-build (written by the deploy script)
    void writeStatusLocked();
    static int64_t nowMs();
    void loop();

    Config cfg_;
    std::shared_ptr<Compositor> comp_;
    mutable std::mutex m_;
    Prefs prefs_;
    Mode mode_ = Mode::FullCam, prev_mode_ = Mode::FullCam;
    int64_t phone_session_ms_ = 0;
    // truck sensors as reported (-1 = not reported) and what we last told the phone
    bool truck_reverse_ = false, truck_moving_ = false;
    int truck_gear_ = -1, truck_pb_ = -1, truck_speed_e3_ = -1, truck_driving_ = -1, truck_night_ = -1;
    int sent_driving_ = -1, sent_night_ = -1;
    std::function<void(int, bool)> sensor_sink_;
    std::function<void(bool)> precond_sink_;
    int precond_ = 0;                                   // transient
    obd::Snapshot obd_; bool obd_have_ = false; int64_t energy_render_ms_ = 0;   // last vehicle-data snapshot (Energy page)
    void fillEnergyLocked(Model& m) const;
    void evalSensorsLocked();                              // effective reverse (Debug override) + the values sent to the phone
    bool phone_session_ = false;                           // a phone session is up (video may not have started yet)
    bool phone_ = false, camera_ = false, reverse_ = false, moving_ = false, zone_edit_ = false;
    int touch_w_ = 0, touch_h_ = 0;
    bool window_from_margins_ = false;                     // the head unit told us (SDR margins): do not guess from the touchscreen aspect
    enum class Reshape { None, Requested, Accepted, Failed, Restoring };      // Split: has the phone re-laid its UI out for the pane?
    Reshape reshape_ = Reshape::None; int reshape_h_ = 0, crop_bottom_applied_ = -1; int64_t reshape_ms_ = 0; int restore_tries_ = 0;
    std::function<void(int l, int r, int t, int b)> ui_config_cb_;
    void reshapeTickLocked(int64_t now);
    void zoomApplyLocked();
    int64_t zoom_apply_ms_ = 0; bool zoom_dirty_ = false;
    int vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;               // visible window inside the output frame (UI is laid out in this space)
    std::vector<uint8_t> full_;                            // full-frame RGBA scratch for the panel overlay
    std::vector<uint8_t> handle_buf_;                      // last small edge-layer content
    bool panel_on_ = false; const uint8_t* panel_ptr_ = nullptr;   // what repushOverlayLocked() sends                                // which overlay layer carries the UI right now
    bool flip_applied_ = false, last_mirror_ = false, last_flip_ = false;
    enum class ZoomGesture { None, Pan, Pinch } zoomg_ = ZoomGesture::None;   // full-screen camera: drag pans, two fingers zoom
    int zpan_x_ = 0, zpan_y_ = 0; double zoom_d0_ = 1, zoom_z0_ = 1;
    struct CalibTouch { int x, y, a; };
    std::vector<CalibTouch> calib_touches_;
    int calib_count_ = 0;
    enum class Pinch { None, Active, Swallow } pinch_ = Pinch::None;   // Swallow: eat the rest of the gesture after a pinch ended
    double pinch_d0_ = 1, pinch_f0_ = 0.3; bool pinch_zoom_ = false;
    InputRouter router_;
    Menu menu_;
    Canvas canvas_;
    Layout layout_;
    int pressed_ui_ = ID_NONE;
    PhoneTouch forward_;
    int64_t last_overlay_push_ = 0;
    bool save_pending_ = false; int64_t save_req_ms_ = 0;     // prefs are written by the UI thread shortly after a change, never on the touch path (fsync on an SD card can take 100 ms+)
    int64_t slow_log_ms_ = 0;                                  // rate limit for the SLOW touch / overlay log lines
    void saveLaterLocked() { save_pending_ = true; save_req_ms_ = nowMs(); }
    void flushPrefs(bool force);
    std::atomic<bool> run_{false};
    std::thread thread_;
};

}  // namespace ui
