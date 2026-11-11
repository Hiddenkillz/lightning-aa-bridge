#include <chrono>
#include "UiController.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include "../compositor/UiBlend.h"
#include "Placeholder.h"
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace ui {

static const char* cornerName(Corner c) { return c == Corner::TL ? "tl" : c == Corner::TR ? "tr" : c == Corner::BL ? "bl" : "br"; }
static bool parseCorner(const std::string& s, Corner& c) {
    if (s == "tl") c = Corner::TL; else if (s == "tr") c = Corner::TR; else if (s == "bl") c = Corner::BL; else if (s == "br") c = Corner::BR; else return false;
    return true;
}
static const char* sizeName(PipSize s) { return s == PipSize::S ? "s" : s == PipSize::M ? "m" : "l"; }
static bool parseSize(const std::string& s, PipSize& z) {
    if (s == "s") z = PipSize::S; else if (s == "m") z = PipSize::M; else if (s == "l") z = PipSize::L; else return false;
    return true;
}

int64_t UiController::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

UiController::UiController(const Config& cfg, std::shared_ptr<Compositor> comp)
    : cfg_(cfg), comp_(std::move(comp)), menu_(cfg.video_width, cfg.video_height), canvas_(cfg.video_width, cfg.video_height) {
    camCropAlign() = cfg_.cam_crop_align;
    { std::ifstream b("/etc/aa-bridge-build"); std::getline(b, build_tag_); while (!build_tag_.empty() && (build_tag_.back() == '\n' || build_tag_.back() == '\r' || build_tag_.back() == ' ')) build_tag_.pop_back(); }
    loadPrefs(cfg_.prefs_path, prefs_);
    mode_ = prev_mode_ = prefs_.start_last ? prefs_.last_mode : prefs_.start_mode;
    camera_ = cfg_.camera_test;      // every real camera starts OFFLINE (placeholder) and goes live when its first frame arrives (Compositor watchdog)
    phone_ = cfg_.phone_test;       // real phone presence comes from HeadUnit later
    vw_ = cfg_.video_width; vh_ = cfg_.video_height;
    if (!cfg_.calib && cfg_.visible_w > 0 && cfg_.visible_h > 0) {          // explicit window from config
        vx_ = cfg_.visible_x; vy_ = cfg_.visible_y; vw_ = cfg_.visible_w; vh_ = cfg_.visible_h;
        menu_ = Menu(vw_, vh_); canvas_ = Canvas(vw_, vh_);
    }
    router_.configure(cfg_.touch_w > 0 ? cfg_.touch_w : vw_, cfg_.touch_h > 0 ? cfg_.touch_h : vh_, vw_, vh_, vw_, vh_);
    if (prefs_.start_settings) menu_.openSettings();            // "always start on Settings": no mode underneath is needed, the page is opaque
}

// The head unit shows only a window of our frame; everything the user must see or touch is laid out inside it.
// std::stod accepts "nan"/"inf" and std::clamp passes NaN through: every number from outside (prefs file, control FIFO) goes through this.
static double finiteOr(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

void UiController::setVisibleLocked(int x, int y, int w, int h) {
    // Defensive: the window comes from the head unit's advertised margins. Whatever arrives, keep it a non-empty even-sized rectangle inside the
    // output frame (a negative size would throw in Canvas, an oversized one would lay panes out beyond the picture).
    const int FW = std::max(2, cfg_.video_width), FH = std::max(2, cfg_.video_height);
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = FW; h = FH; }
    x = std::clamp(x, 0, FW - 2) & ~1; y = std::clamp(y, 0, FH - 2) & ~1;      // even: the phone crop (4:2:0) and the overlay must agree to the pixel
    w = std::clamp(w, 2, FW - x) & ~1; h = std::clamp(h, 2, FH - y) & ~1;
    if (x == vx_ && y == vy_ && w == vw_ && h == vh_) return;
    vx_ = x; vy_ = y; vw_ = w; vh_ = h;
    const Screen keep = menu_.screen();
    menu_ = Menu(vw_, vh_); canvas_ = Canvas(vw_, vh_); menu_.setScreen(keep);
    comp_->setPhoneCrop(vx_, std::max(0, cfg_.video_width - vx_ - vw_), vy_, std::max(0, cfg_.video_height - vy_ - vh_));   // phone UI lives in the visible region
    crop_bottom_applied_ = std::max(0, cfg_.video_height - vy_ - vh_);
    router_.configure(cfg_.touch_w > 0 ? cfg_.touch_w : vw_, cfg_.touch_h > 0 ? cfg_.touch_h : vh_, vw_, vh_, vw_, vh_);      // phone touch space = its UI area (the window), like Sync 4
    std::cout << "[Ui] visible window " << vw_ << "x" << vh_ << " at " << vx_ << "," << vy_ << " of the " << cfg_.video_width << "x" << cfg_.video_height
              << " frame (touch space " << (cfg_.touch_w > 0 ? cfg_.touch_w : vw_) << "x" << (cfg_.touch_h > 0 ? cfg_.touch_h : vh_) << ")" << std::endl;
}

UiController::~UiController() { stop(); }

void UiController::start() {
    run_ = true;
    { std::lock_guard<std::mutex> l(m_); applyLocked(); }       // starts asleep: no pill until the first touch
    thread_ = std::thread([this] { loop(); });
}

void UiController::stop() {
    run_ = false;
    if (thread_.joinable()) thread_.join();
    flushPrefs(true);                                   // nothing pending may be lost on a clean shutdown
}

// Writes the prefs if a change is pending (older than 60 ms so a burst of changes becomes one write). Called by the UI thread, outside the lock while writing.
void UiController::flushPrefs(bool force) {
    Prefs snap;
    { std::lock_guard<std::mutex> l(m_); if (!save_pending_ || (!force && nowMs() - save_req_ms_ < 60)) return; snap = prefs_; save_pending_ = false; }
    if (savePrefs(cfg_.prefs_path, snap)) std::cout << "[Ui] prefs saved" << std::endl;
}

// ---- state inputs ----
// "phone" for the layout means a LIVE picture. A session being up is not enough: until the first decoded frame the pane keeps showing its placeholder
// (otherwise there is a black gap between "connected" and the first frame).
void UiController::setPhonePresent(bool v) {
    {
        std::lock_guard<std::mutex> l(m_);
        phone_session_ = v; if (v) phone_session_ms_ = nowMs();
        reshape_ = Reshape::None; reshape_h_ = 0; crop_bottom_applied_ = -1;     // every phone session starts with the full window layout
        phone_ = v && cfg_.phone_test;                               // the synthetic phone has video from the start
        applyLocked();
    }
    if (v && !cfg_.phone_test) comp_->armPhoneVideoNotify([this] { onPhoneVideo(); });
}
void UiController::onPhoneVideo() {
    std::lock_guard<std::mutex> l(m_);
    if (!phone_session_ || phone_) return;
    phone_ = true;
    std::cout << "[Ui] first phone frame: pane goes live, " << (nowMs() - phone_session_ms_) << " ms after the phone session started" << std::endl;
    applyLocked();
}
void UiController::setCameraPresent(bool v) { std::lock_guard<std::mutex> l(m_); camera_ = v; applyLocked(); }
void UiController::setMoving(bool v) {
    std::lock_guard<std::mutex> l(m_);
    truck_moving_ = v; moving_ = v;                                // informational only: the menu is never locked
    evalSensorsLocked();
    applyLocked();
}
void UiController::setPreconditionState(bool on) {
    std::lock_guard<std::mutex> l(m_);
    if ((precond_ != 0) == on) return;
    precond_ = on ? 1 : 0;
    std::cout << "[Ui] precondition " << (on ? "ON" : "OFF") << " (reported)" << std::endl;
    applyLocked();
}
void UiController::setReverse(bool v) {
    std::lock_guard<std::mutex> l(m_);
    if (truck_reverse_ == v) return;
    truck_reverse_ = v;
    evalSensorsLocked();
    applyLocked();
}
void UiController::setTruckSensor(int what, int v) {
    if (what == 1) { setReverse(v != 0); return; }
    if (what == 2) { setMoving(v != 0); return; }
    std::lock_guard<std::mutex> l(m_);
    switch (what) { case 3: truck_gear_ = v; break; case 4: truck_pb_ = v; break; case 5: truck_speed_e3_ = v; break; case 6: truck_driving_ = v; break; case 7: truck_night_ = v; break; default: return; }
    evalSensorsLocked();
    if (menu_.screen() == Screen::Debug) renderOverlayLocked();     // live readout on the Debug page
}
void UiController::setPhoneSensorSink(std::function<void(int, bool)> sink) {
    std::lock_guard<std::mutex> l(m_);
    sensor_sink_ = std::move(sink); sent_driving_ = sent_night_ = -1;
    evalSensorsLocked();
}
// Effective reverse = the Debug gear override, else the truck. A change drives the "camera when in Reverse" rule. The phone is told the Debug
// override or the truck's value; unchanged values are not re-sent.
void UiController::evalSensorsLocked() {
    const bool rev = prefs_.dbg_gear == 0 ? truck_reverse_ : prefs_.dbg_gear == 2;
    if (rev != reverse_) {
        reverse_ = rev;
        std::cout << "[Ui] reverse=" << rev << (prefs_.dbg_gear ? " (Debug gear override)" : " (truck)") << std::endl;
        if (prefs_.auto_cam_reverse) { if (rev) { prev_mode_ = mode_; mode_ = Mode::FullCam; } else mode_ = prev_mode_; }
    }
    const int truck_drv = truck_driving_ >= 0 ? truck_driving_ : (truck_moving_ ? 26 : 0);     // 26 = no keyboard + no config + short messages (video stays)
#ifdef AA_BRIDGE_DEBUG_SENSORS
    const int drv = prefs_.dbg_driving == 0 ? truck_drv : prefs_.dbg_driving == 2 ? 26 : 0;     // bench/test builds only: the Debug page can override the vehicle's driving status
#else
    const int drv = truck_drv;                                                                   // release builds: what the vehicle reports is what the phone is told, no override
#endif
    const bool night = prefs_.dbg_night == 0 ? truck_night_ == 1 : prefs_.dbg_night == 2;
    if (sensor_sink_ && (drv != sent_driving_ || (night ? 1 : 0) != sent_night_)) {
        sent_driving_ = drv; sent_night_ = night ? 1 : 0;
        std::cout << "[Ui] sensors to phone: driving_status=" << drv << " night=" << (night ? 1 : 0) << std::endl;
        sensor_sink_(drv, night);
    }
}
void UiController::setEnergy(const obd::Snapshot& s) { std::lock_guard<std::mutex> l(m_); obd_ = s; obd_have_ = true; }
// Rows in priority order: what a smaller window would keep is at the top.
void UiController::fillEnergyLocked(Model& m) const {
    const obd::Snapshot& s = obd_; const int64_t now = s.now_ms;
    auto f1 = [](double v) { char b[32]; snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
    auto f0 = [](double v) { char b[32]; snprintf(b, sizeof b, "%.0f", v); return std::string(b); };
    auto f2 = [](double v) { char b[32]; snprintf(b, sizeof b, "%.2f", v); return std::string(b); };
    auto degF = [](double c) { return c * 9.0 / 5.0 + 32.0; };
    const bool pre = s.preconditioning();
    auto row = [&](const char* label, const obd::Field& f, int max_age_ms, std::string val, const char* unit, int level = 0) {
        Model::EnergyRow r; r.label = label; r.unit = unit; r.level = f.fresh(now, max_age_ms) ? level : -1; r.value = val; m.energy_rows.push_back(r); };
    if (s.plugged()) {                                                     // plugged in: the page becomes a charging screen (mi/kWh is meaningless parked)
        const bool chg = s.charging();
        m.energy_big_a_label = chg ? "Charging" : "Charger";
        m.energy_big_a = s.pack_kw.fresh(now, 3000) ? f1(std::max(0.0, -s.pack_kw.v)) + " kW" : "\xe2\x80\x94";
        m.energy_big_b_label = s.soc_target.fresh(now, 30000) ? "Battery / target" : "Battery";
        m.energy_big_b = !s.soc.fresh(now, 5000) ? "\xe2\x80\x94" : f0(s.soc.v) + "%" + (s.soc_target.fresh(now, 30000) ? " to " + f0(s.soc_target.v) + "%" : "");
        const bool dc = s.dc_status.fresh(now, 30000) && s.dc_status.v != 0;
        row("Charge state", s.chg_state, 30000, obd::chargingStateName((int)s.chg_state.v), "", chg ? 1 : 0);
        if (dc) row("DC fast charge", s.dc_status, 30000, "status " + f0(s.dc_status.v), "", 1);
        const double offer = s.evse_offer_a();
        row("Charger offers", s.pilot_duty, 30000, offer > 0 ? f0(offer) + " A" : "n/a", "");
        row("Drawing", s.evse_a, 30000, f0(s.evse_a.v) + " A @ " + f0(s.evse_v.v) + " V", "");
        row("Into the pack", s.chg_out_a, 30000, f1(s.chg_out_a.v) + " A @ " + f0(s.pack_v.v) + " V", "");
        row("Pack allows", s.chg_power_limit_kw, 30000, f1(s.chg_power_limit_kw.v), "kW");
        row("Charger max", s.chg_out_kw_max, 120000, f1(s.chg_out_kw_max.v), "kW");
        row("Pack asks for", s.chg_a_req, 30000, f0(s.chg_a_req.v) + " A / " + f0(s.chg_v_req.v) + " V", "");
        row("Coupler temp", dc ? s.coupler_dc1_c : s.coupler_ac_c, 60000, f0(degF(dc ? s.coupler_dc1_c.v : s.coupler_ac_c.v)), "\xc2\xb0" "F");
        row("Energy remaining", s.energy_kwh, 20000, f1(s.energy_kwh.v), "kWh");
        row("Pack temperature", s.t_pack, 40000, f0(degF(s.t_pack.v)), "\xc2\xb0" "F", pre ? 1 : 0);
        row("Pack min / max", s.t_min, 40000, f0(degF(s.t_min.v)) + " / " + f0(degF(s.t_max.v)), "\xc2\xb0" "F");
        row("Thermal mode", s.thermal_mode, 20000, obd::thermalModeName((int)s.thermal_mode.v), "", (int)s.thermal_mode.v != 0 ? 1 : 0);
        row("Coolant inlet", s.t_coolant_in, 40000, f0(degF(s.t_coolant_in.v)), "\xc2\xb0" "F");
        row("12 V battery", s.lv_v, 5000, f2(s.lv_v.v) + " V  " + (s.lv_a.v > 0 ? "+" : "") + f0(s.lv_a.v) + " A", "", s.lv_v.fresh(now, 5000) && s.lv_v.v < 12.0 ? 2 : 0);
        row("Battery health", s.soh, 7200000, f1(s.soh.v), "%", s.soh.fresh(now, 7200000) && s.soh.v < 90 ? 2 : 0);
        row("Outside / cabin", s.t_ext, 120000, f0(degF(s.t_ext.v)) + " / " + f0(degF(s.t_int.v)), "\xc2\xb0" "F");
        m.energy_link = "OBD: charging view, " + std::to_string(s.replies) + " replies";
        return;
    }
    // big tiles
    m.energy_big_a_label = "Power";
    m.energy_big_a = s.pack_kw.fresh(now, 3000) ? (s.pack_kw.v <= -0.5 ? "+" + f1(-s.pack_kw.v) + " kW" : f1(s.pack_kw.v) + " kW") : "\xe2\x80\x94";   // "+" = regen into the pack
    m.energy_big_b_label = "mi/kWh (30 s)";
    m.energy_big_b = !s.mi_per_kwh_30s.fresh(now, 3000) ? "\xe2\x80\x94" : s.mi_per_kwh_30s.v < 0 ? "regen" : s.mi_per_kwh_30s.v == 0 ? "0" : f1(std::min(s.mi_per_kwh_30s.v, 99.9));
    // rows
    row("Battery", s.soc, 5000, f1(s.soc.v), "%");
    row("Energy remaining", s.energy_kwh, 20000, f1(s.energy_kwh.v), "kWh");
    row("Pack temperature", s.t_pack, 40000, f0(degF(s.t_pack.v)), "\xc2\xb0" "F", pre ? 1 : 0);
    row("Preconditioning", s.chg_state, 20000, pre ? std::string("ON") : "off", "", pre ? 1 : 0);
    row("Thermal mode", s.thermal_mode, 20000, obd::thermalModeName((int)s.thermal_mode.v), "", (int)s.thermal_mode.v != 0 ? 1 : 0);
    row("12 V battery", s.lv_v, 5000, f2(s.lv_v.v), "V", s.lv_v.fresh(now, 5000) && s.lv_v.v < 12.0 ? 2 : 0);
    row("12 V current", s.lv_a, 20000, (s.lv_a.v > 0 ? "+" : "") + f0(s.lv_a.v), "A");
    row("12 V charge", s.lv_soc, 20000, f0(s.lv_soc.v), "%", s.lv_soc.fresh(now, 20000) && s.lv_soc.v < 60 ? 2 : 0);
    row("Battery health", s.soh, 7200000, f1(s.soh.v), "%", s.soh.fresh(now, 7200000) && s.soh.v < 90 ? 2 : 0);
    row("Pack min / max", s.t_min, 40000, f0(degF(s.t_min.v)) + " / " + f0(degF(s.t_max.v)), "\xc2\xb0" "F");
    row("Coolant inlet", s.t_coolant_in, 40000, f0(degF(s.t_coolant_in.v)), "\xc2\xb0" "F");
    row("Heater", s.heater_kw, 20000, obd::heaterModeName((int)s.heater_mode.v) + std::string(s.heater_kw.v > 0.05 ? "  " + f1(s.heater_kw.v) + " kW" : ""), "", s.heater_kw.v > 0.05 ? 1 : 0);
    row("Pack voltage", s.pack_v, 5000, f0(s.pack_v.v), "V");
    row("Pack current", s.pack_a, 5000, f0(s.pack_a.v), "A");
    row("Capacity", s.cap_ah, 7200000, f1(s.cap_ah.v), "Ah");
    row("Lifetime throughput", s.throughput_ah, 7200000, f0(s.throughput_ah.v), "Ah");
    row("Module spread", s.soc_spread, 240000, f2(s.soc_spread.v), "%");
    row("Module V min / avg", s.mod_v_min, 40000, f2(s.mod_v_min.v) + " / " + f2(s.mod_v_avg.v), "V");
    row("Motor / inverter", s.motor_coil_c, 40000, f0(degF(s.motor_coil_c.v)) + " / " + f0(degF(s.inverter_c.v)), "\xc2\xb0" "F");
    row("12 V age", s.lv_age_h, 7200000, f0(s.lv_age_h.v / 24.0), "days");
    row("Outside / cabin", s.t_ext, 120000, f0(degF(s.t_ext.v)) + " / " + f0(degF(s.t_int.v)), "\xc2\xb0" "F");
    row("Odometer", s.odo_km, 40000, f0(s.odo_km.v * 0.621371), "mi");
    row("Speed source", s.speed_mps, 3000, s.speed_from_obd ? std::string("OBD") : "vehicle link", "");
    m.energy_link = !obd_have_ ? "OBD: starting" : !s.port_present ? "OBD adapter: not found" : !s.adapter_ok ? "OBD adapter: " + s.port + " (no answer)"
        : !s.vehicle_awake ? "OBD: vehicle not answering" : "OBD: " + std::to_string(s.replies) + " replies, " + std::to_string(s.errors) + " errors";
}
void UiController::setMode(Mode m) { std::lock_guard<std::mutex> l(m_); mode_ = prev_mode_ = m; applyLocked(); }
void UiController::setVisibleWindow(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> l(m_);
    if (w <= 0 || cfg_.calib || cfg_.visible_w > 0) return;            // none advertised (touch-aspect fallback applies) / calibration / explicit config
    window_from_margins_ = true;
    const int ox = vx_, oy = vy_, ow = vw_, oh = vh_;
    setVisibleLocked(x, y, w, h);
    windowChangedLocked(ox != vx_ || oy != vy_ || ow != vw_ || oh != vh_);
}

// After the window moved (from the vehicle's margins or from its touch aspect): a phone that is already up was advertised and laid out for the old
// one (it connected before the vehicle announced its window, ui51a: AA laid out 1920 wide). Tell it the new layout: in Split the pane request is
// re-issued by applyLayoutOnlyLocked (its bookkeeping is reset), otherwise a full-window UpdateUiConfig via the Restoring path. Then cache it.
void UiController::windowChangedLocked(bool changed) {
    if (changed && phone_session_ && ui_config_cb_ && cfg_.window_relayout) {
        const int FW = cfg_.video_width, FH = cfg_.video_height;
        if (reshape_ == Reshape::Requested || reshape_ == Reshape::Accepted) { reshape_ = Reshape::None; reshape_h_ = 0; std::cout << "[Ui] window changed while the phone was laid out for the old one: re-asking for the Split layout" << std::endl; }
        else { reshape_ = Reshape::Restoring; reshape_h_ = 0; reshape_ms_ = nowMs(); restore_tries_ = 0;
               std::cout << "[Ui] window changed while the phone was laid out for the old one: asking it for the full window (margins l" << vx_ << " r" << std::max(0, FW - vx_ - vw_) << " t" << vy_ << " b" << std::max(0, FH - vy_ - vh_) << ")" << std::endl;
               ui_config_cb_(vx_, std::max(0, FW - vx_ - vw_), vy_, std::max(0, FH - vy_ - vh_)); }
    }
    applyLocked();
    if (changed && window_cb_) window_cb_(vx_, vy_, vw_, vh_);
}

void UiController::setTouchGeometry(int w, int h) {
    std::lock_guard<std::mutex> l(m_);
    touch_w_ = w; touch_h_ = h;
    if (!cfg_.calib && cfg_.visible_w == 0 && !window_from_margins_ && w > 0 && h > 0) {
        // Sync 4 centre-crops our frame to the screen aspect (measured on the truck: 1200x1244 -> 695x720 window of 1280x720).
        const int W = cfg_.video_width, H = cfg_.video_height;
        int vw = W, vh = H;
        if ((long long)w * H < (long long)h * W) vw = even((int)((long long)H * w / h));      // taller than the frame: crop the sides
        else vh = even((int)((long long)W * h / w));                                           // wider than the frame: crop top/bottom
        const int ox = vx_, oy = vy_, ow = vw_, oh = vh_;
        setVisibleLocked((W - vw) / 2, (H - vh) / 2, vw, vh);
        windowChangedLocked(ox != vx_ || oy != vy_ || ow != vw_ || oh != vh_);
    }
}

// ---- model / layout / overlay ----
Model UiController::modelLocked() const {
    Model m;
    {   // readout of what the truck reports (Debug page)
        auto gearName = [](int g) -> std::string { return g < 0 ? "?" : g == 101 ? "P" : g == 102 ? "R" : g == 100 ? "D" : g == 0 ? "N" : std::to_string(g); };
        std::ostringstream a, b;
        a << "Gear " << gearName(truck_gear_) << "   Parking brake " << (truck_pb_ < 0 ? "?" : truck_pb_ ? "on" : "off");
        if (truck_speed_e3_ >= 0) a << "   Speed " << (int)(truck_speed_e3_ * 3.6 / 1000.0 + 0.5) << " km/h";
        b << "Driving status " << (truck_driving_ < 0 ? std::string("?") : std::to_string(truck_driving_)) << "   Night " << (truck_night_ < 0 ? "?" : truck_night_ ? "yes" : "no")
          << "   Sent to phone: " << (sent_driving_ < 0 ? 0 : sent_driving_) << (sent_night_ == 1 ? " / night" : " / day");
        m.truck_line1 = a.str(); m.truck_line2 = b.str();
    }
    m.energy_shown = cfg_.obd_enable; if (m.energy_shown) fillEnergyLocked(m);
    m.precond_shown = cfg_.precondition_button; m.precond = precond_; m.base_art = comp_->baseArt(); m.pip_corners = cfg_.pip_corners; m.corner_restore = cfg_.pip_corner_restore && comp_->uiBlender(); m.pip_dragging = pipd_ == PipDrag::Dragging || pinch_ != Pinch::None;   // window moving/resizing: no corner covers (they would trail)
    m.mode = mode_; m.prefs = prefs_; m.phone = phone_; m.camera = camera_; m.moving = moving_; m.layout = layout_;
    Prefs full = prefs_; full.roi = Roi(); full.cam_zoom = 1.0; full.cam_fill = false;
    m.cam_view = compute(Mode::FullCam, full, vw_, vh_, cfg_.camera_width, cfg_.camera_height, false, true).camera.rect;
    m.status = std::string(phone_ ? "Phone connected" : "No phone") + "  |  " + (camera_ ? "Camera OK" : "No camera");
    m.build = build_tag_;
    m.light = prefs_.theme == 2 || (prefs_.theme == 0 && sent_night_ != 1);        // Auto: the appearance follows the night mode the phone is told (vehicle, or the Debug override)
    m.split_drag_enabled = splitDragOn(); m.split_dragging = split_drag_; m.split_edge = split_edge_; m.mode_changed_ms = mode_changed_ms_;
    if (menu_.screen() == Screen::Debug) {                                           // third readout line (only read while the page is up)
        std::ostringstream s; s << "Camera " << cfg_.camera_width << "x" << cfg_.camera_height << "   ";
        std::ifstream t("/sys/class/thermal/thermal_zone0/temp"); long mc = -1; if (t >> mc) s << "CPU " << (mc + 500) / 1000 << " C   ";
        s << (split_hold_ ? "Split: waiting for the phone's layout" : phone_session_ ? "Phone session up" : "No phone session");
        m.info_line = s.str();
    }
    return m;
}

// The Android Auto splash as the video base layer: black everywhere, the art "covered" into the phone pane when that pane is wanted but has no live picture. It only
// changes with the phone pane (mode / presence / window), never while the PiP is dragged, so dragging costs nothing here and the camera pad simply sits above it.
void UiController::updateBaseLocked() {
    if (!comp_->baseArt()) return;
    const Pane& p = layout_.phone;
    const bool art = p.visible && !p.live && !p.rect.empty() && !(split_drag_ && mode_ == Mode::Split);   // during a divider drag the overlay draws flat boxes; the base stays a plain frame (no per-step work)
    const std::array<int, 7> key = {art ? 1 : 0, art ? p.rect.x : 0, art ? p.rect.y : 0, art ? p.rect.w : 0, art ? p.rect.h : 0, vx_, vy_};
    if (base_key_valid_ && key == base_key_) return;
    base_key_ = key; base_key_valid_ = true;
    const int W = cfg_.video_width, H = cfg_.video_height;
    std::vector<uint8_t> f((size_t)W * H * 3 / 2, 128);
    std::fill(f.begin(), f.begin() + (size_t)W * H, 16);
    if (art) {
        const int x = (vx_ + p.rect.x) & ~1, y = (vy_ + p.rect.y) & ~1;
        const int w = std::min(p.rect.w & ~1, W - x), h = std::min(p.rect.h & ~1, H - y);
        if (w > 0 && h > 0) {
            const std::vector<uint8_t> px = placeholderCover(Art::AndroidAuto, w, h, 0);          // copied out of the cache right away
            YuvaLayer::fromRgba(px.data(), w, h, x, y).blendInto(f.data(), W, f.data() + (size_t)W * H, W / 2, f.data() + (size_t)W * H + (size_t)(W / 2) * (H / 2), W / 2, W, H);
        }
    }
    comp_->setBaseI420(f.data());
}

// Split reshape: the phone is asked (UpdateUiConfig) to lay its UI out for the 16:9 top pane. Until it replies the pane shows the scaled whole picture (fallback).
void UiController::reshapeReply() {
    std::lock_guard<std::mutex> l(m_);
    if (reshape_ == Reshape::Restoring) { reshape_ = Reshape::None; reshape_h_ = 0; std::cout << "[Ui] split reshape: the phone confirmed the full window layout" << std::endl; applyLocked(); return; }
    if (reshape_ != Reshape::Requested) { std::cout << "[Ui] split reshape: a reply arrived while not waiting for one (state " << (int)reshape_ << ")" << std::endl; return; }
    reshape_ = Reshape::Accepted;
    std::cout << "[Ui] split reshape: the phone ANSWERED the layout request after " << (nowMs() - reshape_ms_) << " ms: pane now shows the phone's own layout" << (split_hold_ ? " (divider drag: live picture back)" : "") << std::endl;
    split_hold_ = false;
    applyLocked();
}
void UiController::reshapeTickLocked(int64_t now) {      // the phone never answered: stay on the fallback (until the layout is left)
    if (reshape_ == Reshape::Restoring && now - reshape_ms_ > 700) {          // the phone has not confirmed the restore: ask again (a lost request left the picture stretched and touch dead)
        const int FW = cfg_.video_width, FH = cfg_.video_height;
        if (restore_tries_ < 3 && ui_config_cb_ && phone_session_) { ++restore_tries_; reshape_ms_ = now; std::cout << "[Ui] split reshape: no confirmation, asking again (" << restore_tries_ << ")" << std::endl; ui_config_cb_(vx_, std::max(0, FW - vx_ - vw_), vy_, std::max(0, FH - vy_ - vh_)); }
        else { reshape_ = Reshape::None; reshape_h_ = 0; std::cout << "[Ui] split reshape: giving up waiting for the restore" << std::endl; applyLocked(); }
        return;
    }
    if (reshape_ == Reshape::Requested && now - reshape_ms_ > 2000) {
        reshape_ = Reshape::Failed;
        std::cout << "[Ui] split reshape: NO reply from the phone in 2 s -> keeping the scaled fallback" << std::endl;
        split_hold_ = false;
        applyLocked();
    }
}
void UiController::applyLayoutOnlyLocked() {
    Mode m = mode_; Prefs p = prefs_; bool ph = phone_ && !(m == Mode::Split && (split_drag_ || split_hold_)), cam = camera_ && !(m == Mode::Split && split_drag_);   // Split drag: both panes are placeholders while the finger is down (the camera is resized ONCE, on release); the phone pane stays a placeholder until the phone has laid out for the final size
    if (split_drag_ && m == Mode::Split) p.split_ratio = std::clamp(split_frac_, 0.25, 0.75);
    if (zone_edit_) { m = Mode::FullCam; p.roi = Roi(); p.cam_zoom = 1.0; p.cam_fill = false; ph = false; cam = true; }   // show the whole camera frame while drawing the zone
    const bool want = cfg_.split_reshape && m == Mode::Split && phone_ && ui_config_cb_;      // phone_: its first picture is up (the session is ready for control messages)
    layout_ = compute(m, p, vw_, vh_, cfg_.camera_width, cfg_.camera_height, ph, cam, !want || reshape_ == Reshape::Accepted ? true : false);
    if (m == Mode::Split && !want) layout_ = compute(m, p, vw_, vh_, cfg_.camera_width, cfg_.camera_height, ph, cam, true);
    // phone re-layout bookkeeping (margins are in the codec frame: window offset + what is left of it)
    const int FW = cfg_.video_width, FH = cfg_.video_height, pane_h = layout_.phone_pane.h;
    const bool frozen = m == Mode::Split && (split_drag_ || split_pending_);          // divider being dragged / settling: no requests to the phone until the commit
    if (want && frozen) { /* the pane shows its placeholder; the commit (splitCommitLocked) applies again without the freeze */ }
    else if (want && (reshape_ == Reshape::None || reshape_h_ != pane_h)) {
        reshape_ = Reshape::Requested; reshape_h_ = pane_h; reshape_ms_ = nowMs();
        std::cout << "[Ui] split reshape: asking the phone for a " << vw_ << "x" << pane_h << " UI (margins l" << vx_ << " r" << std::max(0, FW - vx_ - vw_) << " t" << vy_ << " b" << std::max(0, FH - vy_ - pane_h) << ")" << std::endl;
        ui_config_cb_(vx_, std::max(0, FW - vx_ - vw_), vy_, std::max(0, FH - vy_ - pane_h));
        layout_ = compute(m, p, vw_, vh_, cfg_.camera_width, cfg_.camera_height, ph, cam, false);
    } else if (!want && (reshape_ == Reshape::Requested || reshape_ == Reshape::Accepted)) {
        if (phone_session_ && ui_config_cb_) { std::cout << "[Ui] split reshape: leaving Split, restoring the full window layout" << std::endl; ui_config_cb_(vx_, std::max(0, FW - vx_ - vw_), vy_, std::max(0, FH - vy_ - vh_)); reshape_ = Reshape::Restoring; reshape_ms_ = nowMs(); restore_tries_ = 1; }
        else { reshape_ = Reshape::None; reshape_h_ = 0; }
    } else if (!want && reshape_ == Reshape::Failed) { reshape_ = Reshape::None; reshape_h_ = 0; }
    {   // phone crop: the whole window normally; in an accepted split only the pane (the rest of the frame is margin the phone left empty)
        const bool pane_crop = reshape_ == Reshape::Accepted || reshape_ == Reshape::Restoring;      // while restoring, keep showing only the pane the phone still draws into
        const int bottom_extra = pane_crop ? std::max(0, vh_ - (m == Mode::Split ? pane_h : reshape_h_)) : 0;
        const int cb = std::max(0, FH - vy_ - vh_) + bottom_extra;
        if (cb != crop_bottom_applied_) { crop_bottom_applied_ = cb; comp_->setPhoneCrop(vx_, std::max(0, FW - vx_ - vw_), vy_, cb); }
        router_.setPhoneSpace(vw_, (pane_crop && cfg_.split_touch_region == 1) ? (m == Mode::Split ? pane_h : reshape_h_) : vh_);
    }
    //  laid out in the visible window (placeholder panes included)...
    {   // ...only live panes reach the video compositor, placed at their position in the frame; identical live layouts are not re-applied (a divider drag step changes
        // only placeholder panes, and every g_object_set on a mixer pad costs a renegotiation)
        const Layout live = shifted(liveOnly(layout_), vx_, vy_);
        auto same = [](const Pane& a, const Pane& b) { return a.visible == b.visible && a.live == b.live && a.rect.x == b.rect.x && a.rect.y == b.rect.y && a.rect.w == b.rect.w && a.rect.h == b.rect.h; };
        const bool unchanged = live_applied_ && same(live.phone, live_applied_v_.phone) && same(live.camera, live_applied_v_.camera) && live.camera_radius == live_applied_v_.camera_radius
            && live.camera_crop.left == live_applied_v_.camera_crop.left && live.camera_crop.right == live_applied_v_.camera_crop.right && live.camera_crop.top == live_applied_v_.camera_crop.top && live.camera_crop.bottom == live_applied_v_.camera_crop.bottom;
        if (!unchanged) { comp_->applyLayout(live); live_applied_ = true; live_applied_v_ = live; }
    }
    updateBaseLocked();
    if (!flip_applied_ || last_mirror_ != prefs_.cam_mirror || last_flip_ != prefs_.cam_flip) {
        flip_applied_ = true; last_mirror_ = prefs_.cam_mirror; last_flip_ = prefs_.cam_flip;
        comp_->setCameraFlip(prefs_.cam_mirror, prefs_.cam_flip);
    }
    router_.setLayout(layout_);
    {   // rounded PiP corners over a live phone picture: the video path restores the phone's pixels there (see Compositor::setCornerRestore)
        const bool on = cfg_.pip_corner_restore && cfg_.pip_corners && comp_->uiBlender() && m == Mode::Pip && layout_.camera.live && layout_.camera.visible && layout_.phone.live
                        && layout_.camera_radius > 1 && !layout_.camera.rect.empty();
        const ui::Rect cr = layout_.camera.rect;
        comp_->setCornerRestore(on, cr.x + vx_, cr.y + vy_, cr.w, cr.h, layout_.camera_radius, vx_, vy_, vw_, vh_);
    }
}

void UiController::applyLocked() {
    if (!zone_edit_ && mode_ != prefs_.last_mode) { prefs_.last_mode = mode_; saveLaterLocked(); }      // "Always start on: Last" returns to this
    if (mode_ != shown_mode_) { shown_mode_ = mode_; mode_changed_ms_ = nowMs(); menu_.noteActivity(mode_changed_ms_); }   // the mode toast shows for a few seconds
    applyLayoutOnlyLocked();
    std::cout << "[Ui] mode=" << modeName(mode_) << (zone_edit_ ? " (zone edit)" : "") << " phone=" << phone_ << " camera=" << camera_ << std::endl;
    renderOverlayLocked();
    writeStatusLocked();
}

void UiController::renderCalibLocked() {
    // Full-frame (video_w x video_h) calibration card. Everything is in VIDEO-FRAME PIXELS, so what is visible on the Sync 4
    // screen tells us exactly which part of the frame is shown and how it is scaled.
    Canvas& cv = canvas_;
    const int W = cv.w(), H = cv.h();
    cv.clear();
    const Color grid{255, 255, 255, 90}, major{255, 255, 0, 170}, txt{255, 255, 255, 255}, bg{0, 0, 0, 170};
    for (int x = 0; x <= W; x += 80) cv.fillRoundRect({x == W ? W - 2 : x, 0, (x % 160 == 0) ? 3 : 1, H}, 0, (x % 160 == 0) ? major : grid);
    for (int y = 0; y <= H; y += 80) cv.fillRoundRect({0, y == H ? H - 2 : y, W, (y % 160 == 0) ? 3 : 1}, 0, (y % 160 == 0) ? major : grid);
    for (int x = 0; x < W; x += 160) {                       // x labels top and bottom
        cv.fillRoundRect({x + 4, 6, 70, 28}, 4, bg);        cv.text(x + 8, 20, std::to_string(x), 22, txt);
        cv.fillRoundRect({x + 4, H - 36, 70, 28}, 4, bg);   cv.text(x + 8, H - 22, std::to_string(x), 22, txt);
    }
    for (int y = 0; y < H; y += 160) {                       // y labels on the left and at x=640
        cv.fillRoundRect({4, y + 40, 70, 28}, 4, bg);       cv.text(8, y + 54, std::to_string(y), 22, txt);
        cv.fillRoundRect({644, y + 40, 70, 28}, 4, bg);     cv.text(648, y + 54, std::to_string(y), 22, txt);
    }
    // aspect checks: circle should look round; outlines mark candidate visible widths
    cv.strokeRoundRect({W / 2 - 150, H / 2 - 150, 300, 300}, 150, 4, Color{0, 255, 255, 255});
    cv.strokeRoundRect({0, 0, W, H}, 0, 6, Color{255, 0, 0, 255});                       // full frame (red)
    cv.strokeRoundRect({0, 0, 720, 720}, 0, 4, Color{0, 255, 0, 255});                   // 1:1 from left (green)
    cv.strokeRoundRect({0, 0, 695, 720}, 0, 3, Color{255, 0, 255, 255});                 // 1200:1244 from left (magenta)
    cv.strokeRoundRect({0, 0, 960, 720}, 0, 3, Color{255, 128, 0, 255});                 // 4:3 from left (orange)
    cv.strokeRoundRect({160, 0, 960, 720}, 0, 3, Color{0, 128, 255, 255});               // 4:3 centred (blue)
    cv.fillRoundRect({90, 108, 770, 80}, 6, Color{0, 0, 0, 235});
    cv.text(100, 130, "CALIBRATION  frame " + std::to_string(W) + "x" + std::to_string(H) + "  touch space " + std::to_string(touch_w_) + "x" + std::to_string(touch_h_), 26, txt);
    cv.text(100, 160, "red=frame green=720sq mag=695 orange=4:3L blue=4:3C  cyan=circle", 20, txt);
    // raw touch readout + markers
    cv.fillRoundRect({90, 196, 640, 30 + 26 * 8}, 6, Color{0, 0, 0, 235});
    cv.text(100, 212, "TAP anywhere. raw x,y -> marker (red=raw, cyan=scaled to frame)", 20, txt);
    int row = 0;
    for (auto it = calib_touches_.rbegin(); it != calib_touches_.rend() && row < 8; ++it, ++row) {
        const double sx = touch_w_ > 0 ? (double)it->x * W / touch_w_ : it->x, sy = touch_h_ > 0 ? (double)it->y * H / touch_h_ : it->y;
        cv.text(100, 242 + row * 26, "raw " + std::to_string(it->x) + "," + std::to_string(it->y) + " a" + std::to_string(it->a)
                + "  scaled " + std::to_string((int)sx) + "," + std::to_string((int)sy), 20, txt);
    }
    for (const auto& t : calib_touches_) {
        if (t.a == 2) continue;                              // moves: skip markers (still logged)
        auto cross = [&](int x, int y, Color col) { cv.fillRoundRect({x - 22, y - 2, 44, 4}, 0, col); cv.fillRoundRect({x - 2, y - 22, 4, 44}, 0, col); };
        cross(t.x, t.y, Color{255, 40, 40, 255});
        if (touch_w_ > 0) cross((int)((double)t.x * W / touch_w_), (int)((double)t.y * H / touch_h_), Color{0, 255, 255, 255});
    }
    comp_->setPanel(cv.data(), true);
    comp_->setHandle(nullptr, false);
    last_overlay_push_ = nowMs();
}

void UiController::renderOverlayLocked() {
    if (cfg_.calib) { renderCalibLocked(); return; }
    struct Slow { UiController* u; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); ~Slow() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); const int64_t now = nowMs();
        if (ms > 25 && now - u->slow_log_ms_ > 500) { u->slow_log_ms_ = now; std::cout << "[Ui] SLOW overlay refresh: " << (int)ms << " ms (render + convert, on the calling thread)" << std::endl; } } } slow{this};
    const Model m = modelLocked();
    menu_.render(canvas_, m, nowMs());
    router_.setButtons(menu_.buttons());
    if (!menu_.needsPanel(m)) {                              // live Android Auto, pill collapsed/open: only the right-edge strip has content
        const Rect hr = handleRect(canvas_.w(), canvas_.h());
        handle_buf_.assign((size_t)hr.w * hr.h * 4, 0);
        for (int y = 0; y < hr.h; ++y)
            std::memcpy(&handle_buf_[(size_t)y * hr.w * 4], canvas_.data() + ((size_t)(hr.y + y) * canvas_.w() + hr.x) * 4, (size_t)hr.w * 4);
        panel_on_ = false;
    } else {
        panel_on_ = true;
        const int W = cfg_.video_width, H = cfg_.video_height;
        if (comp_->uiBlender()) panel_ptr_ = canvas_.data();         // the blender takes the window-sized canvas and its position directly (no full-frame copy)
        else if (vw_ == W && vh_ == H) panel_ptr_ = canvas_.data();
        else {                                               // blit the window-sized canvas into a full-frame layer at its position
            full_.assign((size_t)W * H * 4, 0);
            for (int y = 0; y < vh_ && y + vy_ < H; ++y)
                std::memcpy(&full_[((size_t)(y + vy_) * W + vx_) * 4], canvas_.data() + (size_t)y * vw_ * 4, (size_t)std::min(vw_, W - vx_) * 4);
            panel_ptr_ = full_.data();
        }
    }
    repushOverlayLocked();
    last_overlay_push_ = nowMs();
}

void UiController::repushOverlayLocked() {
    if (!panel_on_) {
        const Rect hr = handleRect(canvas_.w(), canvas_.h());
        comp_->setHandleOrigin(vx_ + hr.x, vy_ + hr.y);
        comp_->setHandle(handle_buf_.data(), true);
        comp_->setPanel(nullptr, false);
        return;
    }
    if (comp_->uiBlender()) comp_->setPanelRegion(panel_ptr_, vx_, vy_, vw_, vh_, true); else comp_->setPanel(panel_ptr_, true);
    comp_->setHandle(nullptr, false);
}

// ---- touch ----
void UiController::onTouch(int x, int y, int action) {          // single finger (tests, FIFO)
    TouchFrame f; f.action = action; f.index = 0; f.p.push_back({0, x, y});
    onTouchFrame(f);
}

void UiController::onTouchFrame(const TouchFrame& f) {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    std::lock_guard<std::mutex> l(m_);
    const auto t1 = clk::now();
    struct Slow { UiController* u; clk::time_point t0, t1; ~Slow() {            // runs while the lock is still held (declared after it)
        const auto t2 = clk::now(); const double wait = std::chrono::duration<double, std::milli>(t1 - t0).count(), work = std::chrono::duration<double, std::milli>(t2 - t1).count();
        const int64_t now = nowMs();
        if (wait + work > 40 && now - u->slow_log_ms_ > 500) { u->slow_log_ms_ = now; std::cout << "[Ui] SLOW touch: waited " << (int)wait << " ms for the UI lock, handled in " << (int)work << " ms" << std::endl; } } } slow{this, t0, t1};
    if (f.p.empty()) return;
    if (cfg_.calib) { onTouchLocked(f.p[0].x, f.p[0].y, f.action, &f); return; }
    if (pinchLocked(f)) return;
    const int raw = f.action;
    if ((raw == 5 || raw == 6) && router_.currentTarget() == Target::Phone) {          // extra finger of a phone-owned gesture: forward all fingers
        if (forward_) {
            TouchFrame o; o.action = raw; o.index = f.index;
            for (const auto& q : f.p) { TouchPointer t; t.id = q.id; router_.toPhone(q.x, q.y, t.x, t.y); o.p.push_back(t); }
            forward_(o);
        }
        return;
    }
    if (raw == 5 || raw == 6) return;                                                  // extra fingers of UI / camera gestures: ignored
    onTouchLocked(f.p[0].x, f.p[0].y, raw == 3 ? 1 : raw, &f);
}

void UiController::onTouchLocked(int x, int y, int action, const TouchFrame* frame) {
    if (cfg_.calib) {
        std::cout << "[Calib] touch raw " << x << "," << y << " action " << action << " touchspace " << touch_w_ << "x" << touch_h_ << std::endl;
        if (action != 2 || calib_touches_.empty()) { calib_touches_.push_back({x, y, action}); if (calib_touches_.size() > 40) calib_touches_.erase(calib_touches_.begin()); }
        if (action != 2) renderOverlayLocked();
        return;
    }
    Routed r = router_.route(x, y, static_cast<Action>(action));
    const bool woke = menu_.noteActivity(nowMs());          // any touch brings the pill / window buttons back for a few seconds
    if (woke) std::cout << "[Ui] pill wake (touch at " << x << "," << y << " target " << (int)r.target << ")" << std::endl;
    switch (r.target) {
    case Target::Phone:
        if (forward_ && frame) {
            TouchFrame o; o.action = frame->action == 3 ? 1 : frame->action; o.index = frame->index;
            for (const auto& q : frame->p) { TouchPointer t; t.id = q.id; router_.toPhone(q.x, q.y, t.x, t.y); o.p.push_back(t); }
            forward_(o);
        }
        if (woke) renderOverlayLocked();
        break;
    case Target::Camera:                                  // the camera picture takes no taps (window buttons do that); full-screen: drag pans a zoomed view
        if (woke) renderOverlayLocked();                  // window buttons appear on the first touch
        if ((mode_ == Mode::FullCam || mode_ == Mode::Split) && !zone_edit_) panLocked(r.ox, r.oy, action);
        else if (mode_ == Mode::Pip && !zone_edit_) dragPipLocked(r.ox, r.oy, action);
        break;
    case Target::Ui:
        handleUiTouchLocked(r);
        break;
    default:
        if (action == 0) renderOverlayLocked();
        break;
    }
}

bool UiController::pinchLocked(const TouchFrame& f) {
    std::vector<std::pair<int, int>> pts; for (const auto& q : f.p) pts.push_back({q.x, q.y});
    const int raw = f.action;
    if (cfg_.calib) return false;
    const bool ending = (raw == 1 || raw == 3);
    if (pinch_ == Pinch::Swallow) { if (ending) pinch_ = Pinch::None; return true; }       // rest of a pinch gesture: nothing leaks to phone/UI
    auto dist = [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return std::hypot((double)a.first - b.first, (double)a.second - b.second); };
    if (pinch_ == Pinch::Active && pinch_zoom_) {
        if (pts.size() >= 2 && raw != 6 && !ending) {
            double z = std::clamp(pinch_f0_ * dist(pts[0], pts[1]) / pinch_d0_, 1.0, 4.0);
            if (z < 1.03) z = 1.0;
            if (std::fabs(z - prefs_.cam_zoom) >= 0.01) { prefs_.cam_zoom = z; if (z <= 1.0) { prefs_.cam_cx = prefs_.cam_cy = 0.5; } zoomApplyLocked(); }
            return true;
        }
        saveLaterLocked();
        std::cout << "[Ui] camera pinch end: zoom " << prefs_.cam_zoom << " (saved)" << std::endl;
        pinch_ = ending ? Pinch::None : Pinch::Swallow; pinch_zoom_ = false;
        applyLocked();
        return true;
    }
    if (pinch_ == Pinch::Active) {
        if (pts.size() >= 2 && raw != 6 && !ending) {
            double f = clampPipFrac(pinch_f0_ * dist(pts[0], pts[1]) / pinch_d0_);
            if (std::fabs(f - prefs_.pip_frac) >= 0.004) { prefs_.pip_frac = f; prefs_.pip_size = nearestPipSize(f); applyLayoutOnlyLocked(); }
            return true;
        }
        // fingers lifting: finish, persist, and eat whatever is left of the gesture
        prefs_.pip_size = nearestPipSize(prefs_.pip_frac);
        saveLaterLocked();
        std::cout << "[Ui] pinch end: pip width " << (int)(prefs_.pip_frac * 100 + 0.5) << "% of the window (saved)" << std::endl;
        pinch_ = ending ? Pinch::None : Pinch::Swallow;
        applyLocked();
        return true;
    }
    // start (full-screen camera): second finger down while the first one is on the camera picture -> pinch zoom
    if (raw == 5 && pts.size() >= 2 && (mode_ == Mode::FullCam || mode_ == Mode::Split) && !zone_edit_ && camera_ && menu_.screen() != Screen::Settings && menu_.screen() != Screen::Debug
        && router_.currentTarget() == Target::Camera) {
        double d = dist(pts[0], pts[1]);
        if (d < 12) return false;
        pinch_ = Pinch::Active; pinch_zoom_ = true; pinch_d0_ = d; pinch_f0_ = prefs_.cam_zoom; zoomg_ = ZoomGesture::None;
        router_.cancel();
        std::cout << "[Ui] camera pinch start (zoom " << prefs_.cam_zoom << ")" << std::endl;
        return true;
    }
    // start: second finger down while the first one started on the PiP window (menu hidden, PiP showing)
    if (raw == 5 && pts.size() >= 2 && mode_ == Mode::Pip && (menu_.screen() == Screen::Hidden || menu_.screen() == Screen::Home) && layout_.camera.visible
        && router_.currentTarget() == Target::Camera) {
        double d = dist(pts[0], pts[1]);
        if (d < 12) return false;
        if (pipd_ != PipDrag::None) { pipd_ = PipDrag::None; prefs_.pip_drag = false; }
        pinch_ = Pinch::Active; pinch_zoom_ = false; pinch_d0_ = d; pinch_f0_ = prefs_.pip_frac;
        router_.cancel();                                // the first finger must not turn into a tap on release
        std::cout << "[Ui] pinch start (pip width " << (int)(prefs_.pip_frac * 100 + 0.5) << "%)" << std::endl;
        return true;
    }
    return false;
}

void UiController::dragPipLocked(int ox, int oy, int action) {
    if (action == 0) {
        pipd_ = PipDrag::Pending; pipd_sx_ = ox; pipd_sy_ = oy; pipd_rx_ = layout_.camera.rect.x; pipd_ry_ = layout_.camera.rect.y;
        return;
    }
    if (pipd_ == PipDrag::None) return;
    if (action == 2) {
        if (pipd_ == PipDrag::Pending) {
            if (std::abs(ox - pipd_sx_) < 14 && std::abs(oy - pipd_sy_) < 14) return;          // a tap/wobble is not a drag
            pipd_ = PipDrag::Dragging; std::cout << "[Ui] PiP drag start" << std::endl;
            prefs_.pip_drag = true; renderOverlayLocked();       // once: the window buttons disappear while dragging
        }
        prefs_.pip_drag = true; prefs_.pip_x = pipd_rx_ + (ox - pipd_sx_); prefs_.pip_y = pipd_ry_ + (oy - pipd_sy_);
        applyLayoutOnlyLocked();                                                              // pad geometry follows immediately (cheap)
        // The overlay only changes while dragging when the PiP is a PLACEHOLDER (camera absent: its artwork moves with it). A live picture is moved
        // by the mixer pad alone; re-rendering + converting the window overlay per step (30/s) saturated a Pi core and backed up the touch path.
        // The overlay DOES change while dragging when something is drawn around/under the PiP by the overlay: its own placeholder art (camera absent), OR the Android Auto
        // placeholder (no phone) which has a hole cut where the live camera is (truck 29: the hole stayed behind and the art covered the moving camera).
        // The gap adapts to what a refresh really costs (truck 29: 25-160 ms on the Pi, with the encoder running) so the refreshes use at most about a third of the time.
        if (!layout_.camera.live || (!layout_.phone.live && !comp_->baseArt())) {
            const int64_t now = nowMs();
            if (now - pipd_render_ms_ >= pipd_gap_ms_) {
                renderOverlayLocked();
                const int64_t end = nowMs(); pipd_gap_ms_ = std::clamp<int64_t>(3 * (end - now), 80, 600); pipd_render_ms_ = end; pipd_dirty_ = false;
            } else pipd_dirty_ = true;                       // skipped for now: the UI thread draws the latest position when the gap has passed, even if the finger stops
        }
        return;
    }
    // release
    if (pipd_ == PipDrag::Dragging) {
        // No snapping: it stays exactly where the finger let go (inside the window margin), and that position is what gets saved.
        pipFractions(layout_.camera.rect, vw_, vh_, prefs_.pip_px, prefs_.pip_py);
        prefs_.pip_corner = nearestCorner(layout_.camera.rect, vw_, vh_);
        prefs_.pip_drag = false;
        saveLaterLocked();
        std::cout << "[Ui] PiP drag end: dropped at " << (int)(prefs_.pip_px * 100 + 0.5) << "% / " << (int)(prefs_.pip_py * 100 + 0.5) << "% of its travel (saved)" << std::endl;
        pipd_ = PipDrag::None;
        applyLocked();
    } else pipd_ = PipDrag::None;
}

// Pinch/drag fire at the touch rate (60-100 Hz); every layout apply reconfigures the video pads and the crop. Applying at most every 60 ms (the loop flushes the last one)
// keeps the mixer from falling behind (truck 34: mixer lag grew to 9 s during zoom/pan with the camera flapping).
void UiController::zoomApplyLocked() {
    const int64_t now = nowMs();
    if (now - zoom_apply_ms_ >= 60) { zoom_apply_ms_ = now; zoom_dirty_ = false; applyLayoutOnlyLocked(); } else zoom_dirty_ = true;
}
void UiController::panLocked(int ox, int oy, int action) {
    if (action == 0) { zoomg_ = ZoomGesture::Pan; zpan_x_ = ox; zpan_y_ = oy; return; }
    if (zoomg_ != ZoomGesture::Pan) return;
    if (action == 2 && prefs_.cam_zoom > 1.01 && layout_.camera.rect.w > 0 && layout_.camera.rect.h > 0) {
        const double z = prefs_.cam_zoom;       // dragging the picture right moves the view left; one pane width = 1/z of the zoom area
        prefs_.cam_cx = std::clamp(prefs_.cam_cx - (double)(ox - zpan_x_) / (layout_.camera.rect.w * z), 0.0, 1.0);
        prefs_.cam_cy = std::clamp(prefs_.cam_cy - (double)(oy - zpan_y_) / (layout_.camera.rect.h * z), 0.0, 1.0);
        zpan_x_ = ox; zpan_y_ = oy;
        zoomApplyLocked();
    } else if (action == 1) {
        zoomg_ = ZoomGesture::None;
        if (prefs_.cam_zoom > 1.01) saveLaterLocked();
    }
}

void UiController::handleUiTouchLocked(const Routed& r) {
    if (r.ui_id == ID_SPLIT_HANDLE || (split_drag_ && r.action != Action::Down)) { splitDragLocked(r.ox, r.oy, r.action); return; }
    const Model m = modelLocked();
    if (r.action == Action::Down) {
        pressed_ui_ = r.ui_id;
        menu_.setPressed(r.ui_id);
        if (r.ui_id == ID_ZONE_CANVAS) menu_.zoneDrag(r.ox, r.oy, true, m);
        renderOverlayLocked();
    } else if (r.action == Action::Move) {
        if (r.ui_id == ID_ZONE_CANVAS) { menu_.zoneDrag(r.ox, r.oy, false, m); renderOverlayLocked(); }
    } else {  // Up
        int id = pressed_ui_; pressed_ui_ = ID_NONE;
        menu_.setPressed(ID_NONE);
        if (r.ui_id == ID_ZONE_CANVAS) { menu_.zoneDrag(r.ox, r.oy, false, m); renderOverlayLocked(); return; }
        bool inside = false;                              // activate only if released on the same button
        for (const auto& b : menu_.buttons()) if (b.id == id && b.rect.contains(r.ox, r.oy)) inside = true;
        if (id == r.ui_id && inside) applyIntentLocked(menu_.activate(id, m));
        applyLocked();
    }
}

void UiController::applyIntentLocked(const Intent& it) {
    bool save = false;
    switch (it.kind) {
    case Intent::SetMode: mode_ = prev_mode_ = (Mode)it.i; break;
    case Intent::SetStart: prefs_.start_mode = (Mode)it.i; prefs_.start_settings = false; prefs_.start_last = false; save = true; break;
    case Intent::SetStartLast: prefs_.start_last = true; prefs_.start_settings = false; save = true; break;
    case Intent::SetStartSettings: prefs_.start_settings = true; prefs_.start_last = false; save = true; break;
    case Intent::SetCamFill: prefs_.cam_fill = it.i != 0; save = true; break;
    case Intent::SetCamMirror: prefs_.cam_mirror = it.i != 0; save = true; break;
    case Intent::SetCamFlip: prefs_.cam_flip = it.i != 0; save = true; break;
    case Intent::SetPrecond:
        if (cfg_.precondition_button) {
            precond_ = it.i ? 1 : 0;
            std::cout << "[Ui] precondition " << (precond_ ? "ON" : "OFF") << " (user)" << (precond_sink_ ? "" : " - staged: no trigger message known yet, nothing sent to the vehicle") << std::endl;
            if (precond_sink_) precond_sink_(precond_ != 0);
        }
        break;
    case Intent::SysRestart:
        std::cout << "[Ui] USER REQUESTED: restart the bridge service (Debug page)" << std::endl;
        (void)::system("sync; (sleep 0.3; systemctl restart aa-bridge) > /dev/null 2>&1 &");
        break;
    case Intent::SysReboot:
        std::cout << "[Ui] USER REQUESTED: reboot the Pi (Debug page)" << std::endl;
        (void)::system("sync; (sleep 0.3; systemctl reboot) > /dev/null 2>&1 &");
        break;
    case Intent::SetDbgGear: prefs_.dbg_gear = it.i; save = true; break;
    case Intent::SetDbgDriving: prefs_.dbg_driving = it.i; save = true; break;
    case Intent::SetDbgNight: prefs_.dbg_night = it.i; save = true; break;
    case Intent::ResetCamView: prefs_.cam_zoom = 1.0; prefs_.cam_cx = prefs_.cam_cy = 0.5; save = true; break;
    case Intent::SetCorner: prefs_.pip_corner = (Corner)it.i; cornerPos(prefs_.pip_corner, prefs_.pip_px, prefs_.pip_py); save = true; break;
    case Intent::SetSize: prefs_.pip_size = (PipSize)it.i; prefs_.pip_frac = pipWidthFraction(prefs_.pip_size); save = true; break;
    case Intent::SetSplit: prefs_.split_ratio = it.d; save = true; break;
    case Intent::SetSplitDrag: prefs_.split_drag = it.i == 1; save = true; std::cout << "[Ui] USER SET: split divider drag " << (it.i == 1 ? "ON" : "OFF") << " (Debug)" << std::endl; if (split_drag_ && !prefs_.split_drag) { split_drag_ = false; split_hold_ = false; } break;
    case Intent::SetTheme: prefs_.theme = std::clamp(it.i, 0, 2); save = true; std::cout << "[Ui] appearance: " << (it.i == 0 ? "auto" : it.i == 1 ? "dark" : "light") << std::endl; break;
    case Intent::SetAutoCam: prefs_.auto_cam_reverse = it.i != 0; save = true; break;
    case Intent::ResetRoi: prefs_.roi = Roi(); zone_edit_ = false; save = true; break;
    case Intent::SaveRoi: prefs_.roi = it.roi; zone_edit_ = false; save = true; break;
    case Intent::BeginZoneEdit: zone_edit_ = true; break;
    case Intent::EndZoneEdit: zone_edit_ = false; break;
    default: break;
    }
    if (save) saveLaterLocked();
    evalSensorsLocked();
}

// ---- Split divider drag ----
// Finger down on the grab band: the phone pane switches to its placeholder (no stretched interim picture), the divider follows the finger.
// Top 10% / bottom 10% of the window are edge zones: releasing there switches to the full-screen camera / full-screen Android Auto.
// Otherwise the new size is committed 700 ms after release (a second grab inside that window just continues), the phone is asked ONCE for its
// new layout and the placeholder stays until it answers (or 2 s pass).
void UiController::splitDragLocked(int ox, int oy, Action a) {
    const int64_t now = nowMs();
    const double raw = vh_ > 0 ? (double)oy / vh_ : 0.5;
    if (a == Action::Down) {
        if (mode_ != Mode::Split || !splitDragOn()) return;
        split_drag_ = true; split_pending_ = false; split_hold_ = true; split_frac_ = std::clamp(raw, 0.25, 0.75); split_edge_ = 0;
        std::cout << "[Ui] split drag start (phone share " << (int)(prefs_.split_ratio * 100 + 0.5) << "%)" << std::endl;
        applyLocked();
        return;
    }
    if (!split_drag_) return;
    if (a == Action::Move) {
        const int edge = raw < 0.10 ? -1 : raw > 0.90 ? 1 : 0;
        const double f = std::clamp(raw, 0.25, 0.75);
        if (std::fabs(f - split_frac_) < 0.004 && edge == split_edge_) return;
        split_frac_ = f; split_edge_ = edge;
        if (now - split_render_ms_ >= 80) { split_render_ms_ = now; applyLayoutOnlyLocked(); renderOverlayLocked(); } else zoom_dirty_ = true;   // the loop flushes the last step; 80 ms: an overlay redraw costs ~30 ms on the Pi
        return;
    }
    // release
    split_drag_ = false;
    if (split_edge_ != 0) {
        mode_ = prev_mode_ = split_edge_ < 0 ? Mode::FullCam : Mode::FullAA; split_edge_ = 0; split_hold_ = false;
        std::cout << "[Ui] split drag end: released in the " << (mode_ == Mode::FullCam ? "top" : "bottom") << " zone -> " << modeName(mode_) << std::endl;
        applyLocked();
        return;
    }
    prefs_.split_ratio = split_frac_; split_pending_ = true; split_commit_ms_ = now + 700;
    std::cout << "[Ui] split drag end: phone share " << (int)(split_frac_ * 100 + 0.5) << "% (saved; the phone is told in 700 ms)" << std::endl;
    saveLaterLocked();
    applyLayoutOnlyLocked(); renderOverlayLocked();
    split_commit_ms_ = nowMs() + 700;                                                // the settle starts once the release work (camera back at its new size) is done
}
void UiController::splitCommitLocked(int64_t now) {
    if (!split_pending_ || split_drag_ || now < split_commit_ms_) return;
    split_pending_ = false;
    if (mode_ != Mode::Split) { split_hold_ = false; applyLocked(); return; }
    const bool want = cfg_.split_reshape && phone_ && ui_config_cb_;
    if (!want) split_hold_ = false;                                                  // no re-layout possible (no phone / disabled): just show the picture
    std::cout << "[Ui] split commit: phone share " << (int)(prefs_.split_ratio * 100 + 0.5) << "%" << (want ? " -> asking the phone for the new layout" : "") << std::endl;
    applyLocked();                                                                   // pane height changed -> applyLayoutOnlyLocked sends the UpdateUiConfig; the placeholder stays until the reply
}

// ---- commands (FIFO / remote) ----
void UiController::command(const std::string& line) {
    std::istringstream is(line);
    std::string c; is >> c;
    if (c == "touch") {                                   // goes through the real touch path (takes its own lock)
        int x, y; char act;
        if (is >> x >> y >> act) onTouch(x, y, act == 'd' ? 0 : act == 'u' ? 1 : 2);
        else std::cerr << "[Ui] bad command: " << line << std::endl;
        return;
    }
    std::lock_guard<std::mutex> l(m_);
    bool save = false, ok = true;
    commandLocked(line, save, ok);
    if (!ok) { std::cerr << "[Ui] bad command: " << line << std::endl; return; }
    if (save) saveLaterLocked();
}

void UiController::commandLocked(const std::string& line, bool& save, bool& ok) {
    std::istringstream is(line);
    std::string c, a; is >> c >> a;
    if (c == "mode") { Mode m; if ((ok = parseMode(a, m))) { mode_ = prev_mode_ = m; } }
    else if (c == "start") { if (a == "settings") { prefs_.start_settings = true; save = true; } else { Mode m; if ((ok = parseMode(a, m))) { prefs_.start_mode = m; prefs_.start_settings = false; save = true; } } }
    else if (c == "camfill") { prefs_.cam_fill = (a == "1"); save = true; }
    else if (c == "mirror") { prefs_.cam_mirror = (a == "1"); save = true; }
    else if (c == "flip") { prefs_.cam_flip = (a == "1"); save = true; }
    else if (c == "zoom") { try { prefs_.cam_zoom = finiteOr(std::stod(a), 1.0, 4.0, prefs_.cam_zoom); save = true; } catch (...) { ok = false; } }
    else if (c == "zoomreset") { prefs_.cam_zoom = 1.0; prefs_.cam_cx = prefs_.cam_cy = 0.5; save = true; }
    else if (c == "corner") { Corner x; if ((ok = parseCorner(a, x))) { prefs_.pip_corner = x; cornerPos(x, prefs_.pip_px, prefs_.pip_py); save = true; } }
    else if (c == "size") { PipSize x; if ((ok = parseSize(a, x))) { prefs_.pip_size = x; prefs_.pip_frac = pipWidthFraction(x); save = true; }
                            else { try { double f = std::stod(a); prefs_.pip_frac = finiteOr(f, kPipMinFrac, kPipMaxFrac, prefs_.pip_frac); prefs_.pip_size = nearestPipSize(prefs_.pip_frac); ok = true; save = true; } catch (...) {} } }
    else if (c == "split") { try { double x = std::stod(a); prefs_.split_ratio = (std::isfinite(x) && x <= 0.0) ? 0.0 : finiteOr(x, 0.25, 0.75, prefs_.split_ratio); save = true; } catch (...) { ok = false; } }
    else if (c == "autocam") { prefs_.auto_cam_reverse = (a == "1"); save = true; }
    else if (c == "theme") { if (a == "auto") prefs_.theme = 0; else if (a == "dark") prefs_.theme = 1; else if (a == "light") prefs_.theme = 2; else ok = false; save = ok; }
    else if (c == "roi") {
        if (a == "reset") prefs_.roi = Roi();
        else { double x, y, w, h; std::istringstream r(line.substr(line.find("roi") + 3));
               if ((ok = static_cast<bool>(r >> x >> y >> w >> h))) { prefs_.roi = {x, y, w, h}; prefs_.roi.clamp(); } }
        save = ok;
    }
    else if (c == "phone") phone_ = (a == "1");
    else if (c == "camera") camera_ = (a == "1");
    else if (c == "moving") moving_ = (a == "1");
    else if (c == "reverse") { truck_reverse_ = (a == "1"); evalSensorsLocked(); }       // simulates the truck's gear sensor
    else if (c == "dbg") {                                  // dbg gear|driving|night <n>  (same values as the Debug page rows)
        std::string k = a; int n = -1; is >> n;
        if (k == "gear" && n >= 0 && n <= 3) { prefs_.dbg_gear = n; save = true; }
        else if (k == "driving" && n >= 0 && n <= 2) { prefs_.dbg_driving = n; save = true; }
        else if (k == "night" && n >= 0 && n <= 2) { prefs_.dbg_night = n; save = true; }
        else ok = false;
        if (ok) evalSensorsLocked();
    }
    else if (c == "truck") {                                // truck <what> <value>: simulate a truck sensor reading (see setTruckSensor)
        int what = -1, val = 0; std::istringstream r(line.substr(line.find("truck") + 5)); r >> what >> val;
        switch (what) { case 3: truck_gear_ = val; break; case 4: truck_pb_ = val; break; case 5: truck_speed_e3_ = val; break; case 6: truck_driving_ = val; break; case 7: truck_night_ = val; break;
                        case 1: truck_reverse_ = val != 0; break; case 2: truck_moving_ = moving_ = val != 0; break; default: ok = false; }
        if (ok) evalSensorsLocked();
    }
    else if (c == "menu") { if (a == "open") menu_.open(); else if (a == "close") menu_.hide(); else if (a == "settings") menu_.openSettings(); else ok = false; menu_.noteActivity(nowMs()); }
    else if (c == "save") save = true;
    else ok = false;
    if (ok) applyLocked();
}

// ---- background loop: control FIFO + menu timers ----
// Zoom self-heal. A zoomed camera that the mixer cannot keep up with (ui49a) leaves the picture seconds behind, and because the zoom is a saved
// preference every restart inherits it. If the mixer lag stays above cam_zoom_lag_reset_ms for cam_zoom_lag_reset_hold_s while the camera is
// zoomed, the zoom is reset and saved (logged); the lag guard's restart, if it still comes, then starts clean.
void UiController::zoomLagTickLocked(int64_t now) {
    if (cfg_.cam_zoom_lag_reset_hold_s <= 0 || cfg_.cam_zoom_lag_reset_ms <= 0) return;
    const bool zoomed = prefs_.cam_zoom > 1.001 || std::fabs(prefs_.cam_cx - 0.5) > 0.001 || std::fabs(prefs_.cam_cy - 0.5) > 0.001;
    const long long lag = comp_ ? comp_->mixerLagMs() : -1;
    if (!zoomed || lag < cfg_.cam_zoom_lag_reset_ms) { zoom_lag_since_ms_ = 0; return; }
    if (zoom_lag_since_ms_ == 0) { zoom_lag_since_ms_ = now; return; }
    if (now - zoom_lag_since_ms_ < (int64_t)cfg_.cam_zoom_lag_reset_hold_s * 1000) return;
    std::cout << "[Ui] camera view reset: the picture was " << lag << " ms behind for " << cfg_.cam_zoom_lag_reset_hold_s << " s with the camera zoomed (zoom "
              << prefs_.cam_zoom << ", centre " << prefs_.cam_cx << "," << prefs_.cam_cy << "): zoom cleared and saved so the pipeline (or its restart) runs unzoomed" << std::endl;
    prefs_.cam_zoom = 1.0; prefs_.cam_cx = prefs_.cam_cy = 0.5; zoom_lag_since_ms_ = 0; ++zoom_lag_resets_;
    saveLaterLocked(); applyLocked();
}
void UiController::loop() {
    ::mkdir("/run/aa-bridge", 0755);
    ::unlink(cfg_.control_fifo.c_str());
    int fd = -1;
    if (::mkfifo(cfg_.control_fifo.c_str(), 0666) == 0) fd = ::open(cfg_.control_fifo.c_str(), O_RDWR | O_NONBLOCK);   // O_RDWR: no EOF when writers close
    else std::cerr << "[Ui] cannot create control fifo " << cfg_.control_fifo << std::endl;
    std::string buf;
    while (run_) {
        if (fd >= 0) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 50) > 0) {
                char tmp[256]; ssize_t n = ::read(fd, tmp, sizeof tmp);
                if (n > 0) {
                    buf.append(tmp, (size_t)n);
                    size_t nl;
                    while ((nl = buf.find('\n')) != std::string::npos) {
                        std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
                        if (!line.empty()) { std::cout << "[Ui] command: " << line << std::endl; command(line); }
                    }
                }
            }
        } else usleep(50000);
        flushPrefs(false);
        std::lock_guard<std::mutex> l(m_);
        int64_t now = nowMs();
        reshapeTickLocked(now);
        zoomLagTickLocked(now);
        splitCommitLocked(now);
        if (zoom_dirty_ && now - zoom_apply_ms_ >= 60) { zoom_apply_ms_ = now; zoom_dirty_ = false; applyLayoutOnlyLocked(); if (split_drag_) { split_render_ms_ = now; renderOverlayLocked(); } }   // a coalesced divider step must also move the divider
        bool changed = menu_.tick(now);
        if (pipd_dirty_ && pipd_ == PipDrag::Dragging && now - pipd_render_ms_ >= pipd_gap_ms_) {      // trailing refresh after a throttled drag step
            renderOverlayLocked();
            const int64_t end = nowMs(); pipd_gap_ms_ = std::clamp<int64_t>(3 * (end - now), 80, 600); pipd_render_ms_ = end; pipd_dirty_ = false;
        } else if (pipd_ != PipDrag::Dragging) pipd_dirty_ = false;
        if (mode_ == Mode::Energy && obd_have_ && now - energy_render_ms_ >= 1000) { energy_render_ms_ = now; changed = true; }   // live page: 1 Hz refresh
        if (changed && menu_.screen() == Screen::Hidden && !Menu::fullPage(mode_)) std::cout << "[Ui] pill sleep" << std::endl;
        if (changed) { renderOverlayLocked(); writeStatusLocked(); }
        else if (!comp_->uiBlender() && now - last_overlay_push_ > 1000) { repushOverlayLocked(); last_overlay_push_ = now; }                  // 1 Hz keepalive keeps the live compositor pad fed (no re-render)
    }
    if (fd >= 0) ::close(fd);
}

void UiController::writeStatusLocked() {
    ::mkdir("/run/aa-bridge", 0755);
    std::string tmp = cfg_.status_path + ".tmp";
    std::ofstream f(tmp);
    if (!f) return;
    const Prefs& p = prefs_;
    f << "{\"mode\":\"" << modeName(mode_) << "\",\"start_mode\":\"" << modeName(p.start_mode) << "\",\"phone\":" << (phone_ ? "true" : "false")
      << ",\"camera\":" << (camera_ ? "true" : "false") << ",\"moving\":" << (moving_ ? "true" : "false")
      << ",\"menu\":\"" << (menu_.screen() == Screen::Hidden ? "hidden" : menu_.screen() == Screen::Home ? "home" : menu_.screen() == Screen::Settings ? "settings" : menu_.screen() == Screen::Debug ? "debug" : "zone") << "\""
      << ",\"touch\":[" << touch_w_ << "," << touch_h_ << "],\"pip_corner\":\"" << cornerName(p.pip_corner) << "\",\"pip_size\":\"" << sizeName(p.pip_size)
      << "\",\"split_ratio\":" << p.split_ratio << ",\"auto_cam_reverse\":" << (p.auto_cam_reverse ? "true" : "false")
      << ",\"roi\":[" << p.roi.x << "," << p.roi.y << "," << p.roi.w << "," << p.roi.h << "]}\n";
    f.close();
    ::rename(tmp.c_str(), cfg_.status_path.c_str());
}

// A hand-edited, truncated or corrupted prefs file must never put the UI into an undefined state: non-finite numbers are rejected
// (std::stod accepts "nan"/"inf", and std::clamp passes NaN straight through), numbers are range-limited, anything unparsable is ignored.

bool UiController::loadPrefs(const std::string& path, Prefs& p) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "start_mode") parseMode(v, p.start_mode);
            else if (k == "pip_corner") { parseCorner(v, p.pip_corner); cornerPos(p.pip_corner, p.pip_px, p.pip_py); }   // old files: the corner is the position (a later pip_px/pip_py overrides)
            else if (k == "pip_px") p.pip_px = finiteOr(std::stod(v), 0.0, 1.0, p.pip_px);
            else if (k == "pip_py") p.pip_py = finiteOr(std::stod(v), 0.0, 1.0, p.pip_py);
            else if (k == "pip_size") { parseSize(v, p.pip_size); p.pip_frac = pipWidthFraction(p.pip_size); }
            else if (k == "pip_frac") { p.pip_frac = finiteOr(std::stod(v), kPipMinFrac, kPipMaxFrac, p.pip_frac); p.pip_size = nearestPipSize(p.pip_frac); }
            else if (k == "split_h") { double x = std::stod(v); p.split_ratio = (std::isfinite(x) && x <= 0.0) ? 0.5 : finiteOr(x, 0.25, 0.75, p.split_ratio); }   // old files used split_ratio (a width): ignored
            else if (k == "auto_cam_reverse") p.auto_cam_reverse = (v == "1");
            else if (k == "dbg_gear") p.dbg_gear = std::clamp(std::stoi(v), 0, 3);
            else if (k == "dbg_driving") p.dbg_driving = std::clamp(std::stoi(v), 0, 2);
            else if (k == "dbg_night") p.dbg_night = std::clamp(std::stoi(v), 0, 2);
            else if (k == "start_settings") p.start_settings = (v == "1");
            else if (k == "theme") p.theme = std::clamp(std::stoi(v), 0, 2);
            else if (k == "split_drag") p.split_drag = v == "1";
            else if (k == "start_last") p.start_last = (v == "1");
            else if (k == "last_mode") parseMode(v, p.last_mode);
            else if (k == "cam_fill") p.cam_fill = (v == "1");
            else if (k == "cam_mirror") p.cam_mirror = (v == "1");
            else if (k == "cam_flip") p.cam_flip = (v == "1");
            else if (k == "cam_zoom") p.cam_zoom = finiteOr(std::stod(v), 1.0, 4.0, p.cam_zoom);
            else if (k == "cam_center") { std::istringstream r(v); double a = 0.5, b = 0.5; if (r >> a >> b) { p.cam_cx = finiteOr(a, 0, 1, 0.5); p.cam_cy = finiteOr(b, 0, 1, 0.5); } }
            else if (k == "roi") { std::istringstream r(v); Prefs t = p; if ((r >> t.roi.x >> t.roi.y >> t.roi.w >> t.roi.h) && std::isfinite(t.roi.x) && std::isfinite(t.roi.y) && std::isfinite(t.roi.w) && std::isfinite(t.roi.h)) { p.roi = t.roi; p.roi.clamp(); } }
        } catch (...) {}
    }
    return true;
}

bool UiController::savePrefs(const std::string& path, const Prefs& p) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp);
      if (!f) return false;
      f << "# written by aa-bridge UI\nstart_mode=" << modeName(p.start_mode) << "\npip_corner=" << cornerName(p.pip_corner)
        << "\npip_px=" << p.pip_px << "\npip_py=" << p.pip_py << "\npip_size=" << sizeName(p.pip_size) << "\npip_frac=" << p.pip_frac << "\nsplit_h=" << p.split_ratio << "\nauto_cam_reverse=" << (p.auto_cam_reverse ? 1 : 0)
        << "\nroi=" << p.roi.x << " " << p.roi.y << " " << p.roi.w << " " << p.roi.h
        << "\ndbg_gear=" << p.dbg_gear << "\ndbg_driving=" << p.dbg_driving << "\ndbg_night=" << p.dbg_night << "\nstart_settings=" << (p.start_settings ? 1 : 0) << "\nstart_last=" << (p.start_last ? 1 : 0) << "\nlast_mode=" << modeName(p.last_mode) << "\ncam_fill=" << (p.cam_fill ? 1 : 0) << "\ncam_mirror=" << (p.cam_mirror ? 1 : 0) << "\ncam_flip=" << (p.cam_flip ? 1 : 0)
        << "\ncam_zoom=" << p.cam_zoom << "\ncam_center=" << p.cam_cx << " " << p.cam_cy << "\ntheme=" << p.theme << "\nsplit_drag=" << (p.split_drag ? 1 : 0) << "\n"; }
    // Durable replace: the data must be on the card BEFORE the rename, otherwise a power cut right after a settings change can leave a
    // renamed-but-empty prefs file (all settings lost). Then the directory entry is synced too.
    { int fd = ::open(tmp.c_str(), O_RDONLY); if (fd >= 0) { ::fsync(fd); ::close(fd); } }
    if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
    { auto s = path.rfind('/'); int dfd = ::open(s == std::string::npos ? "." : path.substr(0, s ? s : 1).c_str(), O_RDONLY); if (dfd >= 0) { ::fsync(dfd); ::close(dfd); } }
    return true;
}

}  // namespace ui
