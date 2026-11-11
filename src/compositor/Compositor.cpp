#include "Compositor.h"
#include "../ui/RoundGeom.h"
#include "../Watchdog.h"
#include "../LatencyProbe.h"
#include "UiBlend.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <iostream>
#include <cmath>
#include <chrono>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <functional>
#include <thread>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// Compositor::Impl
// ---------------------------------------------------------------------------
// GStreamer pipeline (simplified description):
//
//  [phone_src appsrc]
//      → h264parse → decodebin(v4l2h264dec/avdec_h264)
//          → videoconvert → video/x-raw,format=I420
//              → compositor_element (videomixer)
//                  ← videoconvert ← decodebin(v4l2h264dec/avdec_h264)
//                      ← h264parse ← rtph264depay ← rtspsrc [camera]
//          → v4l2h264enc/x264enc → h264parse → appsink [out_sink]
//
// compositor_element pads:
//   sink_0 = phone video (full frame, z=0)
//   sink_1 = camera video (PiP, z=1, position/size from Config)
//
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// USB camera: hot-plug safe by construction.
// The camera is NOT part of the main pipeline. A separate small pipeline (v4l2src ! jpegdec ! scale ! I420 ! appsink) is created, torn down and
// recreated by a feeder thread as the device appears, vanishes or fails; its frames are copied into the main pipeline's `cam_in` appsrc, which
// always exists. So: plugging a camera in after boot works, unplugging it (or a flaky extender) only stops frames (placeholder + "offline"),
// and a camera that cannot be opened/negotiated can never take the main pipeline (= Android Auto) down.
// camera_device: a node, or a glob such as /dev/v4l/by-id/*-video-index0. If no USB camera is present and camera_test=1 a synthetic source
// feeds the pane instead (switches over automatically both ways). Test hooks (never used in production): "fake:<file>" = pretend a camera is
// present while <file> exists (videotestsrc ball), "fakebad:<file>" = same but the source fails to open.
// ---------------------------------------------------------------------------
// videocrop / videoflip read their properties while transforming a frame. Changing them with g_object_set from another thread while a buffer is
// inside the element segfaulted videocrop under the fuzz test (a pinch/pan changes the camera crop up to ~30x/s on live frames). So the change is
// executed from a blocking pad probe on the element's sink pad: it runs in the streaming thread BETWEEN buffers, and the element reconfigures
// itself before the next frame. Bursts coalesce: only the latest requested change is applied.
class BufferBoundaryProps {
public:
    void attach(GstElement* el) { el_ = el; }
    void post(std::function<void()> fn) {
        if (!el_) return;
        { std::lock_guard<std::mutex> l(m_); latest_ = std::move(fn); if (armed_) return; armed_ = true; }
        GstPad* sink = gst_element_get_static_pad(el_, "sink");
        if (!sink) { std::lock_guard<std::mutex> l(m_); armed_ = false; return; }
        gst_pad_add_probe(sink, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER), [](GstPad*, GstPadProbeInfo*, gpointer self) -> GstPadProbeReturn {
            auto* s = static_cast<BufferBoundaryProps*>(self); std::function<void()> fn;
            { std::lock_guard<std::mutex> l(s->m_); fn = std::move(s->latest_); s->latest_ = nullptr; s->armed_ = false; }
            if (fn) fn();
            return GST_PAD_PROBE_REMOVE; }, this, nullptr);
        gst_object_unref(sink);
    }
private:
    GstElement* el_ = nullptr; std::mutex m_; std::function<void()> latest_; bool armed_ = false;
};

static bool v4l2CaptureNode(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    v4l2_capability cap{};
    bool ok = ::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
              (((cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE);   // not the metadata node
    ::close(fd);
    return ok;
}
// "" = no usable camera right now.
static std::string resolveCameraDevice(const std::string& spec) {
    if (spec.empty()) return "";
    if (spec.find_first_of("*?[") == std::string::npos) return v4l2CaptureNode(spec) ? spec : "";
    glob_t g{}; std::string found;
    if (::glob(spec.c_str(), 0, nullptr, &g) == 0) { for (size_t i = 0; i < g.gl_pathc && found.empty(); ++i) if (v4l2CaptureNode(g.gl_pathv[i])) found = g.gl_pathv[i]; }
    globfree(&g);
    return found;
}
static long long steadyMs() { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

class CameraFeeder {
public:
    CameraFeeder(const Config& cfg, GstElement* cam_in) : cfg_(cfg), cam_in_(GST_ELEMENT(gst_object_ref(cam_in))) {}
    ~CameraFeeder() { stop(); gst_object_unref(cam_in_); }
    void start() { run_ = true; th_ = std::thread([this] { loop(); }); }
    void stop() { run_ = false; if (th_.joinable()) th_.join(); teardown(); }
    static std::string usbDesc(const Config& c, const std::string& dev) {      // also used by the test that checks the fragment links
        const std::string wh = ",width=" + std::to_string(c.camera_width) + ",height=" + std::to_string(c.camera_height);
        return "v4l2src device=" + dev + " ! image/jpeg" + wh + ",framerate=" + std::to_string(c.camera_fps) + "/1 ! jpegdec ! videoconvert ! videoscale "
               "! video/x-raw,format=I420" + wh + " ";
    }
private:
    enum class Kind { None, Usb, Test, Fake, FakeBad };
    struct Want { Kind kind = Kind::None; std::string dev; bool operator==(const Want& o) const { return kind == o.kind && dev == o.dev; } };

    Want want() const {
        const std::string& d = cfg_.camera_device;
        if (d.rfind("fakebad:", 0) == 0) { if (::access(d.substr(8).c_str(), F_OK) == 0) return {Kind::FakeBad, d}; }
        else if (d.rfind("fake:", 0) == 0) { if (::access(d.substr(5).c_str(), F_OK) == 0) return {Kind::Fake, d}; }
        else { std::string r = resolveCameraDevice(d); if (!r.empty()) return {Kind::Usb, r}; }
        if (cfg_.camera_test) return {Kind::Test, "synthetic"};
        return {};
    }
    std::string describe(const Want& w) const {
        const std::string wh = ",width=" + std::to_string(cfg_.camera_width) + ",height=" + std::to_string(cfg_.camera_height);
        const std::string tail = "! appsink name=cam_out emit-signals=false sync=false max-buffers=2 drop=true";
        switch (w.kind) {
        case Kind::Usb: return usbDesc(cfg_, w.dev) + tail;
        case Kind::Test: return "videotestsrc pattern=smpte is-live=true ! video/x-raw,format=I420" + wh + ",framerate=" + std::to_string(cfg_.camera_fps) + "/1 " + tail;
        case Kind::Fake: return "videotestsrc pattern=ball is-live=true ! video/x-raw,format=I420" + wh + ",framerate=" + std::to_string(cfg_.camera_fps) + "/1 " + tail;
        case Kind::FakeBad: return "filesrc location=/nonexistent/camera ! jpegdec ! videoconvert ! video/x-raw,format=I420" + wh + " " + tail;
        default: return "";
        }
    }
    void loop() {
        long long last_attempt = 0;
        while (run_) {
            const Want w = want(); const long long now = steadyMs();
            const bool stalled = cur_.kind != Kind::None && cur_.kind != Kind::Test && now - started_ms_ > 3500 && now - last_frame_ms_.load() > 3000;
            const bool failed = err_.load();
            const bool changed = !(w == cur_);
            if (changed || ((stalled || failed) && now - last_attempt >= 3000)) {
                if (changed) std::cout << "[Camera] source change: " << label(cur_) << " -> " << label(w) << std::endl;
                else std::cout << "[Camera] " << (failed ? "source failed" : "no frames for 3 s") << ": restarting " << label(cur_) << std::endl;
                teardown();
                last_attempt = now; cur_ = w;
                if (w.kind != Kind::None) startPipe(w);
            }
            for (int i = 0; i < 5 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    static std::string label(const Want& w) {
        switch (w.kind) { case Kind::Usb: return "USB " + w.dev; case Kind::Test: return "synthetic"; case Kind::Fake: return "fake camera";
                          case Kind::FakeBad: return "fake (broken) camera"; default: return "none"; }
    }
    void startPipe(const Want& w) {
        err_ = false; started_ms_ = steadyMs(); last_frame_ms_ = 0;
        GError* e = nullptr; GstElement* p = gst_parse_launch(describe(w).c_str(), &e);
        if (!p || e) { std::cerr << "[Camera] pipeline parse error: " << (e ? e->message : "?") << std::endl; if (e) g_error_free(e); if (p) gst_object_unref(p); err_ = true; return; }
        GstBus* bus = gst_element_get_bus(p);
        gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* m, gpointer self) -> GstBusSyncReply {
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                GError* e = nullptr; gchar* dbg = nullptr; gst_message_parse_error(m, &e, &dbg);
                std::cerr << "[Camera] ERROR from " << GST_OBJECT_NAME(m->src) << ": " << (e ? e->message : "?") << std::endl;
                if (e) g_error_free(e); g_free(dbg);
                static_cast<CameraFeeder*>(self)->err_ = true;
            }
            gst_message_unref(m); return GST_BUS_DROP; }, this, nullptr);
        gst_object_unref(bus);
        GstElement* sink = gst_bin_get_by_name(GST_BIN(p), "cam_out");
        GstAppSinkCallbacks cbs = {};
        cbs.new_sample = [](GstAppSink* s, gpointer self) -> GstFlowReturn {
            auto* f = static_cast<CameraFeeder*>(self);
            GstSample* smp = gst_app_sink_pull_sample(s); if (!smp) return GST_FLOW_OK;
            GstBuffer* b = gst_buffer_copy_deep(gst_sample_get_buffer(smp));      // own copy: the sink's buffer is recycled
            gst_sample_unref(smp);
            GST_BUFFER_PTS(b) = GST_CLOCK_TIME_NONE; GST_BUFFER_DTS(b) = GST_CLOCK_TIME_NONE;   // appsrc (do-timestamp) stamps arrival time
            f->last_frame_ms_ = steadyMs();
            gst_app_src_push_buffer(GST_APP_SRC(f->cam_in_), b);
            return GST_FLOW_OK; };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, this, nullptr);
        gst_object_unref(sink);
        if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { std::cerr << "[Camera] could not start " << label(w) << std::endl; err_ = true; }
        pipe_ = p;
    }
    void teardown() {
        if (!pipe_) return;
        gst_element_set_state(pipe_, GST_STATE_NULL);
        gst_object_unref(pipe_); pipe_ = nullptr;
    }

    const Config cfg_;
    GstElement* cam_in_;
    GstElement* pipe_ = nullptr;
    Want cur_;
    std::thread th_; std::atomic<bool> run_{false}, err_{false};
    std::atomic<long long> last_frame_ms_{0}; long long started_ms_ = 0;
};

class Compositor::Impl {
public:
    explicit Impl(const Config& config)
        : config_(config)
        , pipeline_(nullptr)
        , phone_src_(nullptr)
        , out_sink_(nullptr)
        , camera_visible_(true)
    { use_blend_ = config.ui_blend != 0 && !getenv("AA_NO_UIBLEND"); base_art_ = use_blend_ && config.base_art && !getenv("AA_NO_BASEART"); round_ = base_art_ && config.pip_round && !getenv("AA_NO_PIPROUND"); }

    ~Impl() {
        stop();
    }

    void start() {
        buildPipeline();
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) throw std::runtime_error("[Compositor] Failed to set pipeline to PLAYING");
        std::cout << "[Compositor] Pipeline started" << std::endl;
        if (cam_in_) { feeder_ = std::make_unique<CameraFeeder>(config_, cam_in_); feeder_->start(); }
        wd_run_ = true; wd_ = std::thread([this] { cameraWatchdog(); });
        if (base_art_) { base_src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "base_in"); base_run_ = true; base_thr_ = std::thread([this] { baseLoop(); }); }
    }

    // ---- base layer (see Compositor.h setBaseI420) ----
    bool baseArt() const { return base_art_; }
    void setBaseI420(const uint8_t* i420) {
        if (!base_art_) return;
        const size_t n = (size_t)config_.video_width * config_.video_height * 3 / 2;
        { std::lock_guard<std::mutex> l(base_m_);
          if (i420) base_frame_.assign(i420, i420 + n);
          else { base_frame_.assign(n, 128); std::fill(base_frame_.begin(), base_frame_.begin() + (size_t)config_.video_width * config_.video_height, 16); }   // black in limited range
          base_dirty_ = true; }
    }
    // The picture only changes when the layout does; it is pushed on a change and every second after that (a buffer is valid for 5 s: a newer one supersedes it).
    void baseLoop() {
        while (base_run_) {
            bool push = false; std::vector<uint8_t> frame;
            { std::lock_guard<std::mutex> l(base_m_);
              const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
              if (base_frame_.empty()) { const size_t n = (size_t)config_.video_width * config_.video_height * 3 / 2; base_frame_.assign(n, 128); std::fill(base_frame_.begin(), base_frame_.begin() + (size_t)config_.video_width * config_.video_height, 16); base_dirty_ = true; }
              if (base_dirty_ || now - base_push_ms_ >= 1000) { push = true; frame = base_frame_; base_dirty_ = false; base_push_ms_ = now; } }
            if (push && base_src_) {
                GstBuffer* buf = gst_buffer_new_allocate(nullptr, frame.size(), nullptr);
                gst_buffer_fill(buf, 0, frame.data(), frame.size());
                GST_BUFFER_DURATION(buf) = 5 * GST_SECOND;
                gst_app_src_push_buffer(GST_APP_SRC(base_src_), buf);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // Camera presence = frames arriving at the mixer branch. Reported to the UI (placeholder when absent). (Restarting a dead USB source is the CameraFeeder's job.)
    void cameraWatchdog() {
        auto nowMs = [] { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        while (wd_run_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const long long last = cam_last_ms_.load();
            const bool online = last > 0 && nowMs() - last < 2500;
            if (online != cam_online_) {
                cam_online_ = online;
                std::cout << "[Compositor] camera " << (online ? "ONLINE (frames arriving)" : "OFFLINE (no frames for 2.5 s)") << std::endl;
                std::function<void(bool)> cb; { std::lock_guard<std::mutex> l(cam_m_); cb = cam_cb_; }
                if (cb) cb(online);
            }
        }
    }
    void setCameraPresenceCallback(std::function<void(bool)> cb) { std::lock_guard<std::mutex> l(cam_m_); cam_cb_ = std::move(cb); }

    void stop() {
        base_run_ = false; if (base_thr_.joinable()) base_thr_.join();
        if (base_src_) { gst_object_unref(base_src_); base_src_ = nullptr; }
        wd_run_ = false; if (wd_.joinable()) wd_.join();
        feeder_.reset();                                   // before the main pipeline (it pushes into cam_in)
        if (cam_in_) { gst_object_unref(cam_in_); cam_in_ = nullptr; }
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            phone_src_ = nullptr;
            out_sink_ = nullptr;
        }
    }

    size_t phoneQueueBytes() const { return phone_src_ ? (size_t)gst_app_src_get_current_level_bytes(GST_APP_SRC(phone_src_)) : 0; }

    void pushPhoneFrame(const uint8_t* data, size_t size, uint64_t pts_us) { markActive();
        if (!phone_src_) return;

        GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);
        memcpy(map.data, data, size);
        gst_buffer_unmap(buf, &map);

        // The phone's timestamps are on ITS clock (arbitrary, huge): never use them. The live appsrc (do-timestamp=true) stamps each
        // buffer with the pipeline running time on arrival, which is what the compositor needs. (truck test 12: phone frames with absolute
        // phone timestamps stalled the pipeline, "Internal data stream error", output stayed black.)
        (void)pts_us;
        GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;

        GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(phone_src_), buf);
        if (flow != GST_FLOW_OK) {
            std::cerr << "[Compositor] appsrc push error: " << flow << std::endl;
        }
    }

    void armPhoneVideoNotify(std::function<void()> cb) {
        { std::lock_guard<std::mutex> l(notify_m_); notify_cb_ = std::move(cb); }
        notify_armed_.store(true);
        if (!phone_q_ || probe_installed_) return;
        GstPad* pad = gst_element_get_static_pad(phone_q_, "src");
        if (!pad) return;
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer self) -> GstPadProbeReturn {
            auto* s = static_cast<Impl*>(self);
            if (s->notify_armed_.exchange(false)) {
                std::function<void()> cb; { std::lock_guard<std::mutex> l(s->notify_m_); cb = s->notify_cb_; }
                if (cb) std::thread(cb).detach();                     // never run UI code (which takes its own lock) on a streaming thread
            }
            return GST_PAD_PROBE_OK;
        }, this, nullptr);
        gst_object_unref(pad);
        probe_installed_ = true;
    }

    // GstVideoOrientationMethod: identity 0, 180 2, horiz 4, vert 5.
    // The requested direction is remembered and CHECKED against the element while camera frames flow (flipHeal): a camera setting that "cancels itself" (ui51a drive: mirror+flip
    // saved, picture unflipped until the toggle was re-enabled) is then corrected within a second and logged with the element's actual value, instead of staying wrong.
    void setCameraFlip(bool mirror, bool flip) { markActive();
        const int want = (mirror && flip) ? 2 : mirror ? 4 : flip ? 5 : 0;
        want_dir_ = want; heal_tries_ = 0;
        std::cout << "[Compositor] camera orientation requested: mirror=" << mirror << " flip=" << flip << " (video-direction " << want << ")" << (flip_ ? "" : "  NO flip element: the request is kept and applied when there is one") << std::endl;
        postFlip(want);
    }
    void postFlip(int want) {
        if (!flip_) return;
        const char* nick = want == 2 ? "180" : want == 4 ? "horiz" : want == 5 ? "vert" : "identity";
        GstElement* e = flip_; const std::string n = nick;
        flip_props_.post([e, n] { gst_util_set_object_arg(G_OBJECT(e), "video-direction", n.c_str()); });
    }
    // Camera layer self-check (same cadence as flipHeal): the mixer pad's actual alpha/xpos/ypos/width/height against what the UI last asked for.
    // User report (ui54b): a camera picture sometimes shows over the AA placeholder before the phone connects, not movable; the UI's layouts in the log
    // said camera=0. If the layer disagrees with the UI, this logs the pad's real values and re-applies the UI's request.
    void layerHeal() {
        if (!camera_pad_) return;
        gdouble a = -1; gint x = 0, y = 0, w = 0, h = 0;
        g_object_get(G_OBJECT(camera_pad_), "alpha", &a, "xpos", &x, "ypos", &y, "width", &w, "height", &h, nullptr);
        const bool want_vis = camera_visible_.load(); const double want_a = want_vis ? 1.0 : 0.0;
        const int wx = want_cam_x_.load(), wy = want_cam_y_.load(), ww = want_cam_w_.load(), wh = want_cam_h_.load();
        if (want_vis && ww <= 0) return;
        const bool bad = std::fabs(a - want_a) > 0.01 || (want_vis && (x != wx || y != wy || w != ww || h != wh));
        if (!bad) { layer_heal_tries_ = 0; return; }
        if (layer_heal_tries_ >= 5) return;
        ++layer_heal_tries_;
        std::cout << "[Compositor] camera layer MISMATCH: pad alpha=" << a << " [" << x << "," << y << " " << w << "x" << h << "] but the UI asked for alpha=" << want_a
                  << " [" << wx << "," << wy << " " << ww << "x" << wh << "] -> re-applying (try " << layer_heal_tries_ << ")" << std::endl;
        g_object_set(camera_pad_, "alpha", want_a, "xpos", wx, "ypos", wy, "width", ww, "height", wh, nullptr);
    }
    void flipHeal() {       // called from the camera frame probe; ~once a second at 30 fps
        const int want = want_dir_.load(); if (want < 0 || !flip_) return;
        gint have = -1; g_object_get(G_OBJECT(flip_), "video-direction", &have, nullptr);
        if (have == want) { heal_tries_ = 0; return; }
        if (heal_tries_ >= 5) return;                      // do not fight forever; the log line below is the evidence
        ++heal_tries_;
        std::cout << "[Compositor] camera orientation MISMATCH: element has video-direction " << have << ", requested " << want << " -> re-applying (try " << heal_tries_ << ")" << std::endl;
        postFlip(want);
    }

    void setOutputCallback(Compositor::FrameCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        output_cb_ = cb;
    }

    void applyLayout(const ui::Layout& L) { markActive();
        { std::lock_guard<std::mutex> g(last_layout_m_); last_layout_ = L; have_layout_ = true; }
        if (!camera_pad_) { std::cout << "[Compositor] layout before the mixer pads exist: kept, applied when the pipeline is up" << std::endl; }
        auto setPad = [](GstPad* pad, const ui::Pane& pn) {
            if (!pad) return;
            if (pn.visible && !pn.rect.empty())
                g_object_set(pad, "xpos", pn.rect.x, "ypos", pn.rect.y, "width", pn.rect.w, "height", pn.rect.h, "alpha", 1.0, nullptr);
            else g_object_set(pad, "alpha", 0.0, nullptr);    // keep the branch flowing so the phone session stays healthy
        };
        setPad(phone_pad_, L.phone);
        setPad(camera_pad_, L.camera);
        { const bool v = L.camera.visible && !L.camera.rect.empty(); camera_visible_ = v;          // what layerHeal checks the pad against
          if (v) { want_cam_x_ = L.camera.rect.x; want_cam_y_ = L.camera.rect.y; want_cam_w_ = L.camera.rect.w; want_cam_h_ = L.camera.rect.h; } }
        cam_dst_w_ = L.camera.visible ? L.camera.rect.w : 0; cam_radius_ = (round_ && L.camera.visible) ? L.camera_radius : 0;
        if (crop_) { const auto cc = L.camera_crop; GstElement* e = crop_;
            crop_props_.post([e, cc] { g_object_set(e, "left", cc.left, "right", cc.right, "top", cc.top, "bottom", cc.bottom, nullptr); }); }
        std::cout << "[Compositor] layout applied: phone=" << (L.phone.visible ? 1 : 0) << " [" << L.phone.rect.x << "," << L.phone.rect.y
                  << " " << L.phone.rect.w << "x" << L.phone.rect.h << "] camera=" << (L.camera.visible ? 1 : 0) << " [" << L.camera.rect.x
                  << "," << L.camera.rect.y << " " << L.camera.rect.w << "x" << L.camera.rect.h << "] crop l" << L.camera_crop.left
                  << " r" << L.camera_crop.right << " t" << L.camera_crop.top << " b" << L.camera_crop.bottom << std::endl;
    }

    void pushUi(GstElement* src, const uint8_t* rgba, size_t bytes) {
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, bytes, nullptr);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);
        if (rgba) memcpy(map.data, rgba, bytes); else memset(map.data, 0, bytes);   // null = transparent
        gst_buffer_unmap(buf, &map);
        // UI layers are refreshed at ~1 Hz but their caps say 2 fps: without an explicit duration the mixer treats a buffer as expired after 500 ms and
        // drops the layer from the picture until the next one arrives. A newer buffer always supersedes an older one, so a long validity is harmless.
        GST_BUFFER_DURATION(buf) = 5 * GST_SECOND;
        gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    }

    // ---- UI blender (see UiBlend.h): layers are converted once per change and blended into every frame just before the encoder ----
    bool blend() const { return use_blend_; }
    void setBlendLayer(std::shared_ptr<const ui::YuvaLayer>& slot, std::shared_ptr<const ui::YuvaLayer> v) { markActive(); std::lock_guard<std::mutex> l(blend_m_); slot = std::move(v); }
    void setPanelRegion(const uint8_t* rgba, int x, int y, int w, int h, bool visible) { markActive();
        if (!use_blend_) return;
        if (!visible || !rgba || w <= 0 || h <= 0) { setBlendLayer(blend_panel_, nullptr); return; }
        int x0, y0, x1, y1;
        if (!ui::YuvaLayer::opaqueBounds(rgba, w, h, x0, y0, x1, y1)) { setBlendLayer(blend_panel_, nullptr); return; }
        x0 &= ~1; y0 &= ~1; x1 = std::min(w, (x1 + 1) & ~1); y1 = std::min(h, (y1 + 1) & ~1);            // even-aligned (4:2:0)
        setBlendLayer(blend_panel_, std::make_shared<const ui::YuvaLayer>(ui::YuvaLayer::fromRgba(rgba + ((size_t)y0 * w + x0) * 4, x1 - x0, y1 - y0, x + x0, y + y0, w)));
    }
    // ---- rounded PiP corners over a live phone picture ----------------------------------------------------------------------
    struct CornerState {
        bool on = false; ui::CornerPatches patches; int pox = 0, poy = 0, pw = 0, ph = 0;      // phone picture origin / size in the output frame
        std::vector<uint8_t> Y, U, V;                                                             // 4 patches of k*k (Y) and (k/2)*(k/2) (U,V), copied from the latest phone frame
        bool have = false; long frames = 0;
    };
    void setCornerRestore(bool on, int x, int y, int w, int h, double radius, int pox, int poy, int pw, int ph) { markActive();
        std::lock_guard<std::mutex> l(cr_m_);
        if (!on || !use_blend_) { cr_.on = false; cr_.have = false; return; }
        ui::CornerPatches p = ui::CornerPatches::make(ui::Rect{x, y, w, h}, radius);
        if (!p.valid()) { cr_.on = false; cr_.have = false; return; }
        cr_.patches = std::move(p); cr_.pox = pox; cr_.poy = poy; cr_.pw = pw; cr_.ph = ph; cr_.on = true; cr_.have = false;
        const size_t k = (size_t)cr_.patches.k; cr_.Y.assign(4 * k * k, 0); cr_.U.assign(4 * (k / 2) * (k / 2), 128); cr_.V = cr_.U;
    }
    static GstPadProbeReturn phoneCornerProbe(GstPad* pad, GstPadProbeInfo* info, gpointer self) {       // newest phone frame -> the four corner patches
        auto* s = static_cast<Impl*>(self);
        { std::lock_guard<std::mutex> l(s->cr_m_); if (!s->cr_.on) return GST_PAD_PROBE_OK; }
        GstCaps* caps = gst_pad_get_current_caps(pad); GstVideoInfo vi; const bool ok = caps && gst_video_info_from_caps(&vi, caps) && GST_VIDEO_INFO_FORMAT(&vi) == GST_VIDEO_FORMAT_I420;
        if (caps) gst_caps_unref(caps);
        if (!ok) return GST_PAD_PROBE_OK;
        GstVideoFrame vf; if (!gst_video_frame_map(&vf, &vi, GST_PAD_PROBE_INFO_BUFFER(info), GST_MAP_READ)) return GST_PAD_PROBE_OK;
        const int fw = GST_VIDEO_INFO_WIDTH(&vi), fh = GST_VIDEO_INFO_HEIGHT(&vi);
        const uint8_t* Y = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 0); const uint8_t* U = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 1); const uint8_t* V = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 2);
        const int ys = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0), us = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 1), vs = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 2);
        {   std::lock_guard<std::mutex> l(s->cr_m_);
            if (s->cr_.on && fw == s->cr_.pw && fh == s->cr_.ph) {         // the phone frame is the size the layout assumed: its pixel (x,y) lands at (pox+x, poy+y) in the output
                const int k = s->cr_.patches.k, kc = k / 2; bool all = true;
                for (int c = 0; c < 4; ++c) {
                    const int sx = s->cr_.patches.ox[c] - s->cr_.pox, sy = s->cr_.patches.oy[c] - s->cr_.poy;
                    if (sx < 0 || sy < 0 || sx + k > fw || sy + k > fh) { all = false; continue; }            // patch partly outside the phone picture: leave it as it is
                    for (int y = 0; y < k; ++y) std::memcpy(&s->cr_.Y[((size_t)c * k + y) * k], Y + (size_t)(sy + y) * ys + sx, (size_t)k);
                    for (int y = 0; y < kc; ++y) { std::memcpy(&s->cr_.U[((size_t)c * kc + y) * kc], U + (size_t)(sy / 2 + y) * us + sx / 2, (size_t)kc); std::memcpy(&s->cr_.V[((size_t)c * kc + y) * kc], V + (size_t)(sy / 2 + y) * vs + sx / 2, (size_t)kc); }
                }
                s->cr_.have = all; ++s->cr_.frames;
            } else s->cr_.have = false;
        }
        gst_video_frame_unmap(&vf);
        return GST_PAD_PROBE_OK;
    }
    // called by blendProbe on the mixed frame (camera already over the phone): put the phone's pixels back in the four corners
    void restoreCorners(uint8_t* Y, int ys, uint8_t* U, int us, uint8_t* V, int vs, int fw, int fh) {
        std::lock_guard<std::mutex> l(cr_m_);
        if (!cr_.on || !cr_.have) return;
        const int k = cr_.patches.k, kc = k / 2;
        for (int c = 0; c < 4; ++c) {
            const int ox = cr_.patches.ox[c], oy = cr_.patches.oy[c]; if (ox < 0 || oy < 0 || ox + k > fw || oy + k > fh) continue;
            const uint8_t* cov = &cr_.patches.cov[(size_t)c * k * k];
            for (int y = 0; y < k; ++y) { uint8_t* d = Y + (size_t)(oy + y) * ys + ox; const uint8_t* sY = &cr_.Y[((size_t)c * k + y) * k]; const uint8_t* a = cov + (size_t)y * k;
                for (int x = 0; x < k; ++x) { const int al = a[x]; if (al == 255) d[x] = sY[x]; else if (al) d[x] = (uint8_t)((d[x] * (255 - al) + sY[x] * al + 127) / 255); } }
            for (int y = 0; y < kc; ++y) { uint8_t* du = U + (size_t)(oy / 2 + y) * us + ox / 2; uint8_t* dv = V + (size_t)(oy / 2 + y) * vs + ox / 2; const uint8_t* sU = &cr_.U[((size_t)c * kc + y) * kc]; const uint8_t* sV = &cr_.V[((size_t)c * kc + y) * kc];
                for (int x = 0; x < kc; ++x) {
                    const int a = (cov[(size_t)(2 * y) * k + 2 * x] + cov[(size_t)(2 * y) * k + 2 * x + 1] + cov[(size_t)(2 * y + 1) * k + 2 * x] + cov[(size_t)(2 * y + 1) * k + 2 * x + 1] + 2) / 4;
                    if (a == 255) { du[x] = sU[x]; dv[x] = sV[x]; } else if (a) { du[x] = (uint8_t)((du[x] * (255 - a) + sU[x] * a + 127) / 255); dv[x] = (uint8_t)((dv[x] * (255 - a) + sV[x] * a + 127) / 255); } }
            }
        }
    }
    static GstPadProbeReturn blendProbe(GstPad* pad, GstPadProbeInfo* info, gpointer self) {
        auto* s = static_cast<Impl*>(self);
        std::shared_ptr<const ui::YuvaLayer> a, b; { std::lock_guard<std::mutex> l(s->blend_m_); a = s->blend_panel_; b = s->blend_handle_; }
        bool cr; { std::lock_guard<std::mutex> l(s->cr_m_); cr = s->cr_.on && s->cr_.have; }
        if (!a && !b && !cr) return GST_PAD_PROBE_OK;
        GstCaps* caps = gst_pad_get_current_caps(pad); GstVideoInfo vi; const bool ok = caps && gst_video_info_from_caps(&vi, caps) && GST_VIDEO_INFO_FORMAT(&vi) == GST_VIDEO_FORMAT_I420;
        if (caps) gst_caps_unref(caps);
        if (!ok) return GST_PAD_PROBE_OK;
        GstBuffer* buf = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info)); GST_PAD_PROBE_INFO_DATA(info) = buf;
        GstVideoFrame vf; if (!gst_video_frame_map(&vf, &vi, buf, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;
        uint8_t* Y = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 0); uint8_t* U = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 1); uint8_t* V = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vf, 2);
        const int ys = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0), us = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 1), vs = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 2), fw = GST_VIDEO_INFO_WIDTH(&vi), fh = GST_VIDEO_INFO_HEIGHT(&vi);
        if (cr) s->restoreCorners(Y, ys, U, us, V, vs, fw, fh);
        if (a) a->blendInto(Y, ys, U, us, V, vs, fw, fh);
        if (b) b->blendInto(Y, ys, U, us, V, vs, fw, fh);
        gst_video_frame_unmap(&vf);
        return GST_PAD_PROBE_OK;
    }

    void setHandle(const uint8_t* rgba, bool visible) { markActive();
        if (use_blend_) {
            const ui::Rect hr = ui::handleRect(config_.video_width, config_.video_height);
            if (!visible || !rgba) setBlendLayer(blend_handle_, nullptr);
            else setBlendLayer(blend_handle_, std::make_shared<const ui::YuvaLayer>(ui::YuvaLayer::fromRgba(rgba, hr.w, hr.h, handle_x_, handle_y_)));
            return;
        }
        if (!handle_src_ || !handle_pad_) return;
        const ui::Rect hr = ui::handleRect(config_.video_width, config_.video_height);
        if (visible && rgba) pushUi(handle_src_, rgba, (size_t)hr.w * hr.h * 4);
        else if (!handle_primed_) pushUi(handle_src_, nullptr, (size_t)hr.w * hr.h * 4);   // a live pad needs at least one buffer
        handle_primed_ = true;
        g_object_set(handle_pad_, "alpha", visible ? 1.0 : 0.0, nullptr);
    }

    // Crop the phone's decoded frame to the region the head unit actually shows (AAP UI resolution): the phone lays its UI out there.
    void setPhoneCrop(int l, int r, int t, int b) { markActive();
        if (!phone_crop_) return;
        // videocrop on planar 4:2:0 must get even values (odd ones crashed it inside gst_video_crop under the fuzz test: window 1280x600 at y=119)
        l = std::max(0, l) & ~1; r = std::max(0, r) & ~1; t = std::max(0, t) & ~1; b = std::max(0, b) & ~1;
        GstElement* e = phone_crop_;
        phone_crop_props_.post([e, l, r, t, b] { g_object_set(e, "left", l, "right", r, "top", t, "bottom", b, nullptr); });
        std::cout << "[Compositor] phone crop l" << l << " r" << r << " t" << t << " b" << b << std::endl;
    }

    void setHandleOrigin(int x, int y) { markActive(); handle_x_ = x; handle_y_ = y; if (handle_pad_) g_object_set(handle_pad_, "xpos", x, "ypos", y, nullptr); }

    void setPanel(const uint8_t* rgba, bool visible) { markActive();
        if (use_blend_) { setPanelRegion(rgba, 0, 0, config_.video_width, config_.video_height, visible); return; }
        if (!panel_src_ || !panel_pad_) return;
        const size_t bytes = (size_t)config_.video_width * config_.video_height * 4;
        if (visible && rgba) pushUi(panel_src_, rgba, bytes);
        else if (!panel_primed_) pushUi(panel_src_, nullptr, bytes);
        panel_primed_ = true;
        g_object_set(panel_pad_, "alpha", visible ? 1.0 : 0.0, nullptr);
    }

    void setCameraVisible(bool visible) { markActive();
        camera_visible_ = visible;
        if (camera_pad_) {
            g_object_set(camera_pad_, "alpha", visible ? 1.0 : 0.0, nullptr);
        }
    }

    void setOverlayGeometry(int x, int y, int w, int h) { markActive();
        config_.camera_overlay_x = x;
        config_.camera_overlay_y = y;
        config_.camera_overlay_w = w;
        config_.camera_overlay_h = h;
        want_cam_x_ = x; want_cam_y_ = y; want_cam_w_ = w; want_cam_h_ = h;
        if (camera_pad_) {
            g_object_set(camera_pad_,
                "xpos", x, "ypos", y,
                "width", w, "height", h,
                nullptr);
        }
    }

private:
    std::string phoneBranch(const std::string& size_caps, const std::string& phone_caps, const std::string& h264dec) {
        if (config_.phone_test)
            return "videotestsrc pattern=" + std::string(getenv("AA_PHONE_PATTERN") ? getenv("AA_PHONE_PATTERN") : "ball") + " is-live=true ! video/x-raw,format=I420" + size_caps + " ! videocrop name=phone_crop ! mix.sink_1 ";
        return "appsrc name=phone_src format=time is-live=true do-timestamp=true " + appsrcLatency() + " caps=\"" + phone_caps + "\" "
               "! h264parse ! " + h264dec + " ! videoconvert ! video/x-raw,format=I420"
               ",width=" + std::to_string(config_.video_width) + ",height=" + std::to_string(config_.video_height) +
               " ! videocrop name=phone_crop "
               // Newest-frame-wins buffer in front of the compositor: the aggregator takes ONE phone frame per output tick (30/s) and never skips, so
               // a burst from the phone (connect, fast drag) backed up the whole decode chain and the picture ran 4-8 s behind (truck test 25: decoder
               // thread idle, 473 KB queued, lag drained over a minute). Decode now runs at full speed; stale decoded frames are dropped here.
               + std::string((getenv("AA_NO_PHONEQ") || !config_.phone_queue) ? "! queue name=phone_q ! mix.sink_1 " : "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=200000000 leaky=downstream name=phone_q ! mix.sink_1 ");
    }

    // A live appsrc with unset latency reports min=max=0, which makes the compositor's latency query fail
    // ("Impossible to configure latency: max 0 < min 66ms") once any other live source has a frame of latency.
    static std::string appsrcLatency() {
        if (getenv("AA_NO_LATENCY_FIX")) return "";
        return "min-latency=0 max-latency=2000000000 ";
    }

    static GstPad* findSinkPad(GstElement* el, const char* name) {   // compositor sink pads are request pads
        GstPad* found = nullptr; GValue v = G_VALUE_INIT;
        GstIterator* it = gst_element_iterate_sink_pads(el);
        while (!found && gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
            GstPad* pad = GST_PAD(g_value_get_object(&v));
            gchar* n = gst_pad_get_name(pad);
            if (g_strcmp0(n, name) == 0) found = GST_PAD(gst_object_ref(pad));
            g_free(n); g_value_reset(&v);
        }
        g_value_unset(&v); gst_iterator_free(it);
        return found;
    }

    void buildPipeline() {
        // Build pipeline description string
        // Using gst_parse_launch for readability; can be migrated to
        // element-by-element construction later if needed.

        // Determine codec elements based on what's available
        // Try V4L2 hardware first (Pi 4), fall back to software
        // Software by default: v4l2h264enc on Pi 4 hits the bcm2835-codec buffer-size bug
        // (sizeimage 4096 < 65536) inside GStreamer's pool and produces no frames.
        // AA_ENCODER=v4l2 re-enables the hardware path for experiments.
        const char* encenv = getenv("AA_ENCODER");
        bool hw = hasV4L2Codec() && encenv && std::string(encenv) == "v4l2";
        std::string h264dec = hw ? "v4l2h264dec" : "openh264dec";
        std::string h264enc = hw ? "v4l2h264enc"
            : "x264enc tune=zerolatency speed-preset=ultrafast bitrate=" + std::to_string(config_.video_bitrate / 1000) +
              " key-int-max=" + std::to_string(config_.video_fps) + " bframes=0 cabac=false ! video/x-h264,profile=baseline";
        std::cout << "[Compositor] ui overlay: " << (use_blend_ ? "pre-encoder blender (ui_blend=1)" : "compositor layers (ui_blend=0)") << std::endl;
        std::cout << "[Compositor] video path: phone_queue=" << config_.phone_queue << " enc_queue=" << config_.enc_queue << " mixer_alpha=" << config_.mixer_alpha << " mix_latency_ms=" << config_.mix_latency_ms << std::endl;
        std::cout << "[Compositor] encoder: " << (hw ? "v4l2 (hardware)" : "x264enc (software, baseline)") << std::endl;

        // Output caps: H.264 at configured resolution/fps
        std::string out_caps =
            "video/x-h264,stream-format=byte-stream,alignment=au"
            ",width=" + std::to_string(config_.video_width) +
            ",height=" + std::to_string(config_.video_height) +
            ",framerate=" + std::to_string(config_.video_fps) + "/1";

        // Phone source caps
        std::string phone_caps =
            "video/x-h264,stream-format=byte-stream,alignment=au"
            ",width=" + std::to_string(config_.video_width) +
            ",height=" + std::to_string(config_.video_height);

        // Layer order (compositor zorder): 0 = black base (always flowing, so
        // the truck gets a picture with NO phone attached), 1 = phone AA video
        // when present, 2 = camera PiP on top. The base is what makes the
        // standalone/camera-only mode work: compositor only outputs when at
        // least one live pad is producing.
        std::string size_caps =
            ",width=" + std::to_string(config_.video_width) +
            ",height=" + std::to_string(config_.video_height) +
            ",framerate=" + std::to_string(config_.video_fps) + "/1";
        const ui::Rect hr = ui::handleRect(config_.video_width, config_.video_height);
        const std::string camFmt = round_ ? "A420" : "I420";
        std::string desc =
            // --- Base: black (or, with base_art, a pushed picture: the Android Auto splash), live, at output size ---
            (base_art_ ?
                std::string("appsrc name=base_in format=time is-live=true do-timestamp=true ") + appsrcLatency() + " caps=video/x-raw,format=I420,width=" + std::to_string(config_.video_width) +
                    ",height=" + std::to_string(config_.video_height) + ",framerate=2/1 "
              : std::string("videotestsrc pattern=black is-live=true ! video/x-raw,format=I420") + size_caps + " ") +
            "! mix.sink_0 "
            // --- Phone video branch ---
            + phoneBranch(size_caps, phone_caps, h264dec) +
            "compositor name=mix latency=" + std::to_string((long long)config_.mix_latency_ms * 1000000LL) + " "
            "    sink_0::zorder=0 "
            "    sink_1::zorder=1 "
            "    sink_2::zorder=2 "
            "    sink_2::xpos=" + std::to_string(config_.camera_overlay_x) +
            "    sink_2::ypos=" + std::to_string(config_.camera_overlay_y) +
            "    sink_2::width=" + std::to_string(config_.camera_overlay_w) +
            "    sink_2::height=" + std::to_string(config_.camera_overlay_h) + " "
            + std::string(use_blend_ ? "" : "    sink_3::zorder=3 sink_3::alpha=0 sink_3::xpos=" + std::to_string(hr.x) + " sink_3::ypos=" + std::to_string(hr.y) +
            "    sink_3::width=" + std::to_string(hr.w) + " sink_3::height=" + std::to_string(hr.h) +
            "    sink_4::zorder=4 sink_4::alpha=0 ")
            // The mixer MUST output an alpha-capable format: the compositor converts every pad to the output format, so with I420 here the RGBA UI
            // layers lost their alpha channel and drew as opaque (black) rectangles over the video (found 2026-10-15; tests/compositor_test alpha2).
            // A420 keeps alpha through the blend; videoconvert then drops the alpha plane (cheap plane copy) for the I420 encoder.
            + std::string(((use_blend_ && !round_) || getenv("AA_OUT_I420") || (!use_blend_ && !config_.mixer_alpha)) ? "! video/x-raw,format=I420" : "! video/x-raw,format=A420") + size_caps + " "
            // Decouple the mixer from the encoder. The mixer's output timeline must keep pace with real time: if the encoder (software x264) is
            // briefly slower than 30 fps, a blocking push stalls the mixer, its timeline falls behind, and every phone frame (stamped with the
            // real time it arrived) waits in line for its turn: the picture runs seconds behind and only drains when load drops (truck tests
            // 22-25, reproduced under emulation: tests/compositor_test lag mode). A short LEAKY queue drops the OLDEST composed frame instead, so
            // under load we lose frame rate, never freshness. It also moves the A420->I420 conversion + encode onto their own thread.
            + std::string((getenv("AA_NO_ENCQ") || !config_.enc_queue) ? "" : "! queue name=enc_q max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ") +
            "! videoconvert name=enc_conv ! video/x-raw,format=I420" + size_caps + " "
            "! " + h264enc + " "
            "! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "   // AA requires Annex-B; without this x264 gave AVCC
            "! appsink name=out_sink sync=false max-buffers=2 drop=true ";

        // --- Camera branch (RTSP from ESP32), only when configured ---
        if (!config_.camera_device.empty() || config_.camera_test) {
            // The camera enters through an always-present appsrc fed by the CameraFeeder (USB, synthetic or none, hot-pluggable).
            desc += "appsrc name=cam_in format=time is-live=true do-timestamp=true " + appsrcLatency() + " caps=video/x-raw,format=I420,width=" + std::to_string(config_.camera_width) +
                    ",height=" + std::to_string(config_.camera_height) + ",framerate=" + std::to_string(config_.camera_fps) + "/1 "
                    "! videoflip name=cam_flip video-direction=identity ! videocrop name=cam_crop ! videoconvert ! video/x-raw,format=" + camFmt + " "
                    "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=500000000 leaky=downstream ! mix.sink_2 ";
            std::cout << "[Compositor] camera: hot-pluggable feeder, device='" << config_.camera_device << "' synthetic_fallback=" << (config_.camera_test ? 1 : 0)
                      << " " << config_.camera_width << "x" << config_.camera_height << "@" << config_.camera_fps << std::endl;
        } else if (config_.camera_udp_port > 0 || !config_.camera_rtsp.empty()) {
            const bool mjpeg = config_.camera_codec != "h264";
            std::string src = config_.camera_udp_port > 0
                ? "udpsrc port=" + std::to_string(config_.camera_udp_port) + " caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=" +
                  std::string(mjpeg ? "JPEG,payload=26" : "H264,payload=96") + "\" "
                : "rtspsrc location=" + config_.camera_rtsp + " latency=100 protocols=tcp ";
            std::string depay_dec = mjpeg ? "! rtpjpegdepay ! jpegdec " : "! rtph264depay ! h264parse ! " + h264dec + " ";
            desc += src + depay_dec + "! videoflip name=cam_flip video-direction=identity ! videocrop name=cam_crop ! videoconvert ! video/x-raw,format=" + camFmt + " "
                    "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=500000000 leaky=downstream ! mix.sink_2 ";   // bounded queue: gives the aggregator a finite max latency
            std::cout << "[Compositor] camera: " << (config_.camera_udp_port > 0 ? "udp:" + std::to_string(config_.camera_udp_port) : config_.camera_rtsp)
                      << " codec=" << config_.camera_codec << std::endl;
        } else {
            std::cout << "[Compositor] camera_rtsp empty -> no camera overlay (base + phone only)" << std::endl;
        }


        // --- UI layers: fixed caps, only alpha toggles at runtime (renegotiating a live compositor pad is a crash risk) ---
        if (!use_blend_) {
        desc += "appsrc name=ui_handle format=time is-live=true do-timestamp=true " + appsrcLatency() + " caps=video/x-raw,format=RGBA,width=" +
                std::to_string(hr.w) + ",height=" + std::to_string(hr.h) + ",framerate=2/1 ! mix.sink_3 ";
        desc += "appsrc name=ui_panel format=time is-live=true do-timestamp=true " + appsrcLatency() + " caps=video/x-raw,format=RGBA,width=" +
                std::to_string(config_.video_width) + ",height=" + std::to_string(config_.video_height) + ",framerate=2/1 ! mix.sink_4 ";
        }

        GError* err = nullptr;
        pipeline_ = gst_parse_launch(desc.c_str(), &err);
        if (!pipeline_ || err) {
            std::string msg = err ? err->message : "unknown";
            if (err) g_error_free(err);
            throw std::runtime_error("[Compositor] Pipeline parse error: " + msg);
        }

        {   // surface pipeline errors/warnings in the log (previously silent)
            GstBus* bus = gst_element_get_bus(pipeline_);
            gst_bus_set_sync_handler(bus, [](GstBus*, GstMessage* m, gpointer) -> GstBusSyncReply {
                if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
                    GError* e = nullptr; gchar* dbg = nullptr;
                    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) gst_message_parse_error(m, &e, &dbg);
                    else gst_message_parse_warning(m, &e, &dbg);
                    std::cerr << "[Compositor] GST " << (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR ? "ERROR" : "WARN")
                              << " from " << GST_OBJECT_NAME(m->src) << ": " << (e ? e->message : "?")
                              << " | " << (dbg ? dbg : "") << std::endl;
                    if (e) g_error_free(e); g_free(dbg);
                }
                gst_message_unref(m); return GST_BUS_DROP; }, nullptr, nullptr);
            gst_object_unref(bus);
        }
        phone_src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "phone_src");
        out_sink_  = gst_bin_get_by_name(GST_BIN(pipeline_), "out_sink");

        // Hook appsink callback for composed output
        GstAppSinkCallbacks appsink_cbs = {};
        appsink_cbs.new_sample = &Impl::onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(out_sink_), &appsink_cbs, this, nullptr);

        idle_skip_ = config_.idle_skip && !getenv("AA_NO_IDLESKIP");
        if (idle_skip_) {
            if (GstElement* ec = gst_bin_get_by_name(GST_BIN(pipeline_), "enc_conv")) {
                if (GstPad* sp = gst_element_get_static_pad(ec, "sink")) { gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, &Impl::idleProbe, this, nullptr); gst_object_unref(sp); }
                gst_object_unref(ec);
            }
            std::cout << "[Compositor] idle frame skip ON: heartbeat " << config_.idle_fps << " fps when nothing changed for " << config_.idle_hold_ms << " ms" << std::endl;
        }
        if (use_blend_) {                                              // UI overlay: blended into each I420 frame right before the encoder
            if (GstElement* ec = gst_bin_get_by_name(GST_BIN(pipeline_), "enc_conv")) {
                if (GstPad* sp = gst_element_get_static_pad(ec, "src")) { gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, &Impl::blendProbe, this, nullptr); gst_object_unref(sp); }
                gst_object_unref(ec);
            }
        }
        // Get camera compositor pad for runtime geometry changes
        GstElement* mix = gst_bin_get_by_name(GST_BIN(pipeline_), "mix");
        if (mix) {
            camera_pad_ = findSinkPad(mix, "sink_2");
            phone_pad_  = findSinkPad(mix, "sink_1");
            handle_pad_ = findSinkPad(mix, "sink_3");
            panel_pad_  = findSinkPad(mix, "sink_4");
            handle_src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "ui_handle");
            panel_src_  = gst_bin_get_by_name(GST_BIN(pipeline_), "ui_panel");
            // The UI's first layout usually arrives before these pads exist (ui59a: every boot started with the camera pad at its pipeline
            // defaults, alpha 1 at 800,450 480x270, i.e. a phantom camera window over the AA placeholder until layerHeal caught it ~1 s later).
            { ui::Layout L; bool have; { std::lock_guard<std::mutex> g(last_layout_m_); L = last_layout_; have = have_layout_; }
              if (have) { std::cout << "[Compositor] re-applying the UI's layout to the fresh mixer pads" << std::endl; applyLayout(L); } }
            crop_ = gst_bin_get_by_name(GST_BIN(pipeline_), "cam_crop");
            cam_in_ = gst_bin_get_by_name(GST_BIN(pipeline_), "cam_in");
            if (crop_) {                                         // every camera type: note the arrival time of each decoded camera frame
                if (GstPad* cp = gst_element_get_static_pad(crop_, "src")) {
                    gst_pad_add_probe(cp, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer self) -> GstPadProbeReturn {
                        { auto* im = static_cast<Impl*>(self); if ((++im->heal_n_ % 30) == 0) { im->flipHeal(); im->layerHeal(); } }
                        static_cast<Impl*>(self)->cam_last_ms_ = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); if (static_cast<Impl*>(self)->camera_visible_) static_cast<Impl*>(self)->markActive();
                        return GST_PAD_PROBE_OK; }, this, nullptr);
                    gst_object_unref(cp);
                }
            }
            flip_ = gst_bin_get_by_name(GST_BIN(pipeline_), "cam_flip");
            crop_props_.attach(crop_); flip_props_.attach(flip_);
            phone_q_ = gst_bin_get_by_name(GST_BIN(pipeline_), "phone_q");
            phone_crop_ = gst_bin_get_by_name(GST_BIN(pipeline_), "phone_crop");
            phone_crop_props_.attach(phone_crop_);
            if (phone_crop_ && use_blend_) if (GstPad* pp = gst_element_get_static_pad(phone_crop_, "src")) { gst_pad_add_probe(pp, GST_PAD_PROBE_TYPE_BUFFER, &Impl::phoneCornerProbe, this, nullptr); gst_object_unref(pp); }
            if (!camera_pad_) std::cout << "[Compositor] note: no camera pad (no camera source configured)" << std::endl;
            else {
              if (round_) gst_pad_add_probe(camera_pad_, GST_PAD_PROBE_TYPE_BUFFER, &Impl::roundProbe, this, nullptr);
              gst_pad_add_probe(camera_pad_, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer) -> GstPadProbeReturn {
                static std::atomic<unsigned> n{0}; unsigned k = ++n;
                if (k == 1 || k % 150 == 0) std::cout << "[Compositor] camera frames received: " << k << std::endl;
                return GST_PAD_PROBE_OK; }, nullptr, nullptr); }
            gst_object_unref(mix);
        }
    }

    // Rounded PiP corners: every camera picture arrives as A420 (alpha 255); the four corner squares of its alpha plane get an anti-aliased quarter circle. The radius is
    // given in OUTPUT pixels, the picture is at its cropped source size and gets scaled to the window by the mixer, so the radius is converted first.
    static GstPadProbeReturn roundProbe(GstPad* pad, GstPadProbeInfo* info, gpointer self) {
        auto* s = static_cast<Impl*>(self);
        const int dw = s->cam_dst_w_.load(), rd = s->cam_radius_.load();
        if (rd <= 0 || dw <= 0) return GST_PAD_PROBE_OK;
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info); if (!buf) return GST_PAD_PROBE_OK;
        GstCaps* caps = gst_pad_get_current_caps(pad); GstVideoInfo vi; const bool ok = caps && gst_video_info_from_caps(&vi, caps) && GST_VIDEO_INFO_FORMAT(&vi) == GST_VIDEO_FORMAT_A420;
        if (caps) gst_caps_unref(caps);
        if (!ok) return GST_PAD_PROBE_OK;
        buf = gst_buffer_make_writable(buf); GST_PAD_PROBE_INFO_DATA(info) = buf;
        GstVideoFrame fr; if (!gst_video_frame_map(&fr, &vi, buf, GST_MAP_WRITE)) return GST_PAD_PROBE_OK;
        const int w = GST_VIDEO_FRAME_WIDTH(&fr), h = GST_VIDEO_FRAME_HEIGHT(&fr);
        uint8_t* A = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&fr, 3); const int st = GST_VIDEO_FRAME_PLANE_STRIDE(&fr, 3);
        applyRoundMask(A, st, w, h, std::max(1.0, rd * (double)w / dw));
        gst_video_frame_unmap(&fr);
        return GST_PAD_PROBE_OK;
    }
    static void applyRoundMask(uint8_t* A, int stride, int w, int h, double r) {
        const int n = (int)std::ceil(r); if (n <= 0 || w < 2 * n || h < 2 * n) return;
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
            const double cx = r - (i + 0.5), cy = r - (j + 0.5);                          // pixel centre measured from the arc's centre (r in from both edges)
            const double d = std::sqrt(cx * cx + cy * cy);
            double cov = (cx <= 0 || cy <= 0) ? 1.0 : std::clamp(r - d + 0.5, 0.0, 1.0);    // 1 inside the circle, 0 outside, 1 px soft edge
            const uint8_t a = (uint8_t)std::lround(cov * 255.0);
            A[(size_t)j * stride + i] = a; A[(size_t)j * stride + (w - 1 - i)] = a;
            A[(size_t)(h - 1 - j) * stride + i] = a; A[(size_t)(h - 1 - j) * stride + (w - 1 - i)] = a;
        }
    }

    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer user_data) {
        auto* self = reinterpret_cast<Impl*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_ERROR;

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_READ);

        { auto& lp = LatencyProbe::get(); if (lp.out_left > 0) { int64_t d = probeNowMs() - lp.touch_ms; if (d > 1500) lp.out_left = 0; else { --lp.out_left; std::cout << "[Compositor] LAT touch -> encoded frame out: " << d << " ms (" << map.size << " bytes)" << std::endl; } } }
        g_frame_progress.fetch_add(1, std::memory_order_relaxed);
        { static unsigned n = 0; static auto t0 = std::chrono::steady_clock::now(); ++n;
          if (n == 1 || n % 150 == 0) {
              auto t1 = std::chrono::steady_clock::now(); double sec = std::chrono::duration<double>(t1 - t0).count(); t0 = t1;
              std::string la; { std::ifstream f("/proc/loadavg"); std::getline(f, la); }
              // mixer lag = how far the output timeline is behind real time. Healthy: tens of ms. If the mixer cannot sustain the target fps this grows
              // without bound (truck test 27: minutes; every touch then showed up that much later), so it is logged next to the fps.
              long long lag_ms = -1; if (GST_BUFFER_PTS_IS_VALID(buf) && GST_IS_ELEMENT(self->pipeline_)) { GstClockTime rt = gst_element_get_current_running_time(self->pipeline_); if (GST_CLOCK_TIME_IS_VALID(rt)) { lag_ms = (long long)((int64_t)rt - (int64_t)GST_BUFFER_PTS(buf)) / 1000000; if (lag_ms < -1800000000LL) lag_ms += 3600000000LL; } }   // x264enc stamps its output with a +1000 h offset; the value includes the encoder latency
              std::cout << "[Compositor] encoded frames out: " << n << " (last " << map.size << " bytes) "
                        << (n == 1 ? 0.0 : 150.0 / sec) << " fps, loadavg " << la.substr(0, 14) << ", mixer lag " << lag_ms << " ms" << (self->idle_skip_ ? ", idle-skipped " + std::to_string(self->idle_skipped_.load()) : std::string()) << std::endl;
              // A timeline that has fallen far behind real time does not heal by itself (truck 27/32): restart the service, which also re-enumerates USB.
              static LagGuard guard(getenv("AA_NO_LAGGUARD") ? 0 : self->config_.lag_guard_ms, (int64_t)self->config_.lag_guard_hold_s * 1000);   // tests under emulation legitimately lag: they set AA_NO_LAGGUARD
              const int64_t nowms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
              self->last_lag_ms_ = lag_ms;
              if (guard.feed(lag_ms, nowms)) { std::cout << "[Compositor] LAG GUARD: mixer lag " << lag_ms << " ms for more than " << self->config_.lag_guard_hold_s << " s: restarting the service" << std::endl; std::cout.flush(); std::_Exit(4); } } }
        uint64_t pts_us = GST_BUFFER_PTS_IS_VALID(buf)
            ? GST_BUFFER_PTS(buf) / 1000
            : 0;

        {
            std::lock_guard<std::mutex> lock(self->cb_mutex_);
            if (self->output_cb_) {
                self->output_cb_(map.data, map.size, pts_us);
            }
        }

        gst_buffer_unmap(buf, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    static bool hasV4L2Codec() {
        // Check if bcm2835-codec V4L2 M2M device exists (Pi 4 specific)
        return access("/dev/video10", F_OK) == 0;
    }

    Config config_;
    GstElement* pipeline_;
    GstElement* phone_src_;
    GstElement* out_sink_;
    GstPad*     camera_pad_ = nullptr;
    std::mutex  last_layout_m_; ui::Layout last_layout_; bool have_layout_ = false;
    GstPad*     phone_pad_ = nullptr;
    GstPad*     handle_pad_ = nullptr;
    GstPad*     panel_pad_ = nullptr;
    GstElement* handle_src_ = nullptr;
    GstElement* panel_src_ = nullptr;
    bool handle_primed_ = false, panel_primed_ = false;
    GstElement* crop_ = nullptr;
    GstElement* flip_ = nullptr;
    GstElement* phone_q_ = nullptr;
    GstElement* cam_in_ = nullptr;              // appsrc of the camera branch (fed by the CameraFeeder)
    std::unique_ptr<CameraFeeder> feeder_;
    bool cam_online_ = false;
    std::atomic<long long> cam_last_ms_{0}; std::atomic<bool> wd_run_{false}; std::thread wd_; std::mutex cam_m_; std::function<void(bool)> cam_cb_;
    std::atomic<bool> notify_armed_{false}; bool probe_installed_ = false; std::mutex notify_m_; std::function<void()> notify_cb_;
    GstElement* phone_crop_ = nullptr;
    BufferBoundaryProps crop_props_, flip_props_, phone_crop_props_;
    std::atomic<int> want_dir_{-1}; std::atomic<unsigned> heal_n_{0}; std::atomic<int> heal_tries_{0}, layer_heal_tries_{0};   // camera orientation / layer self-checks (streaming thread)
    std::atomic<int> want_cam_x_{0}, want_cam_y_{0}, want_cam_w_{0}, want_cam_h_{0};        // the UI's last camera rectangle, published for layerHeal
    // Idle frame skip (config idle_skip, default OFF): when nothing visible changed (no phone frame, no live camera, no UI change) most mixer frames are dropped BEFORE the
    // convert/blend/encode stage, leaving a heartbeat of idle_fps. A phone only sends frames when its screen changes, so this is what a real phone looks like to the vehicle.
    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void markActive() { last_active_ms_.store(nowMs(), std::memory_order_relaxed); }
    std::atomic<int64_t> last_active_ms_{0}, last_pass_ms_{0}; std::atomic<long> idle_skipped_{0}; bool idle_skip_ = false;
public:
    std::atomic<long long> last_lag_ms_{-1};
private:
    static GstPadProbeReturn idleProbe(GstPad*, GstPadProbeInfo*, gpointer u) {
        auto* s = static_cast<Impl*>(u); const int64_t now = nowMs();
        if (now - s->last_active_ms_.load(std::memory_order_relaxed) < s->config_.idle_hold_ms) { s->last_pass_ms_ = now; return GST_PAD_PROBE_OK; }
        const int64_t every = 1000 / (s->config_.idle_fps > 0 ? s->config_.idle_fps : 5);
        if (now - s->last_pass_ms_.load(std::memory_order_relaxed) >= every) { s->last_pass_ms_ = now; return GST_PAD_PROBE_OK; }
        ++s->idle_skipped_; return GST_PAD_PROBE_DROP;
    }
    bool use_blend_ = true;
    bool round_ = false;                        // PiP corners rounded: camera pad is A420 with a mask, mixer output is A420
    std::atomic<int> cam_dst_w_{0}, cam_radius_{0};   // current camera window width / corner radius in output px (from the layout)
    bool base_art_ = false; GstElement* base_src_ = nullptr; std::atomic<bool> base_run_{false}; std::thread base_thr_;
    std::mutex base_m_; std::vector<uint8_t> base_frame_; bool base_dirty_ = false; int64_t base_push_ms_ = 0;
    std::mutex cr_m_; CornerState cr_;
    std::mutex blend_m_; std::shared_ptr<const ui::YuvaLayer> blend_panel_, blend_handle_; int handle_x_ = 0, handle_y_ = 0;
    std::atomic<bool> camera_visible_;
    std::mutex cb_mutex_;
    Compositor::FrameCallback output_cb_;
};

// ---------------------------------------------------------------------------
// Compositor public interface
// ---------------------------------------------------------------------------

Compositor::Compositor(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

Compositor::~Compositor() = default;

void Compositor::start() { impl_->start(); }
void Compositor::stop()  { impl_->stop(); }

void Compositor::setCameraFlip(bool mirror, bool flip) { impl_->setCameraFlip(mirror, flip); }
void Compositor::setCameraPresenceCallback(std::function<void(bool)> cb) { impl_->setCameraPresenceCallback(std::move(cb)); }
void Compositor::armPhoneVideoNotify(std::function<void()> cb) { impl_->armPhoneVideoNotify(std::move(cb)); }
size_t Compositor::phoneQueueBytes() const { return impl_->phoneQueueBytes(); }
void Compositor::pushPhoneFrame(const uint8_t* data, size_t size, uint64_t pts_us) {
    impl_->pushPhoneFrame(data, size, pts_us);
}

void Compositor::applyLayout(const ui::Layout& l) { impl_->applyLayout(l); }
void Compositor::setHandle(const uint8_t* rgba, bool v) { impl_->setHandle(rgba, v); }
void Compositor::setPanel(const uint8_t* rgba, bool v) { impl_->setPanel(rgba, v); }
bool Compositor::baseArt() const { return impl_->baseArt(); }
void Compositor::setBaseI420(const uint8_t* i420) { impl_->setBaseI420(i420); }
void Compositor::setHandleOrigin(int x, int y) { impl_->setHandleOrigin(x, y); }
bool Compositor::uiBlender() const { return impl_->blend(); }
void Compositor::setPanelRegion(const uint8_t* rgba, int x, int y, int w, int h, bool v) { impl_->setPanelRegion(rgba, x, y, w, h, v); }
void Compositor::setPhoneCrop(int l, int r, int t, int b) { impl_->setPhoneCrop(l, r, t, b); }
void Compositor::setCornerRestore(bool on, int x, int y, int w, int h, double radius, int pox, int poy, int pw, int ph) { impl_->setCornerRestore(on, x, y, w, h, radius, pox, poy, pw, ph); }

long long Compositor::mixerLagMs() const { return impl_->last_lag_ms_.load(); }
void Compositor::setOutputCallback(FrameCallback cb) {
    impl_->setOutputCallback(std::move(cb));
}

void Compositor::setCameraVisible(bool visible) {
    impl_->setCameraVisible(visible);
}

void Compositor::setOverlayGeometry(int x, int y, int w, int h) {
    impl_->setOverlayGeometry(x, y, w, h);
}
