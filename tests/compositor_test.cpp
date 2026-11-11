// Runs the real Compositor + UiController with test patterns (phone=ball, camera=smpte),
// cycles through every layout, and dumps each mode's encoded H.264 to /tmp/layout-<mode>.h264.
#include <execinfo.h>
#include "../src/ui/Placeholder.h"
#include "../src/compositor/UiBlend.h"
#include <signal.h>
#include <unistd.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <atomic>
#include <map>
#include <cstdint>
#include <cmath>
#include <random>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <iostream>
#include <thread>
#include <vector>
#include <iterator>
#include "../src/compositor/Compositor.h"
#include "../src/ui/UiController.h"
static void crash(int s) { void* bt[40]; int n = backtrace(bt, 40); dprintf(2, "FATAL signal %d\n", s); backtrace_symbols_fd(bt, n, 2); _exit(128 + s); }
std::atomic<long long> g_feed_start_ms{0};
std::atomic<int> g_frames_at_start{-1};
int main(int argc, char** argv) {
    signal(SIGSEGV, crash); signal(SIGABRT, crash);
    gst_init(&argc, &argv);
    // 4th arg "alpha": does a fully transparent RGBA layer over I420 video leave the video untouched? (compositor blend sanity check)
    if (argc > 4 && std::string(argv[4]) == "alpha") {
        auto grab = [&](bool overlay, int fmtAlpha) {
            std::string d = "compositor name=mix sink_1::xpos=100 sink_1::ypos=20 sink_1::width=84 sink_1::height=310 sink_1::alpha=1 ! videoconvert ! video/x-raw,format=I420 ! appsink name=o max-buffers=1 "
                "videotestsrc num-buffers=3 pattern=smpte ! video/x-raw,format=I420,width=640,height=360 ! mix.sink_0 ";
            if (overlay) d += std::string("videotestsrc num-buffers=3 pattern=solid-color foreground-color=") + (fmtAlpha ? "0" : "0xff000000") + " ! video/x-raw,format=RGBA,width=84,height=310 ! mix.sink_1 ";
            GError* e = nullptr; GstElement* pl = gst_parse_launch(d.c_str(), &e); if (!pl) { std::cout << "[alpha] parse error " << (e ? e->message : "?") << std::endl; return std::vector<uint8_t>(); }
            GstElement* sink = gst_bin_get_by_name(GST_BIN(pl), "o"); gst_element_set_state(pl, GST_STATE_PLAYING);
            GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND); std::vector<uint8_t> out;
            if (s) { GstBuffer* b = gst_sample_get_buffer(s); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_READ); out.assign(m.data, m.data + std::min<size_t>(m.size, 640 * 360)); gst_buffer_unmap(b, &m); gst_sample_unref(s); }
            gst_element_set_state(pl, GST_STATE_NULL); gst_object_unref(pl); return out; };
        auto base = grab(false, 0), clear = grab(true, 1), opaque = grab(true, 0);
        if (base.empty() || clear.empty() || opaque.empty()) { std::cout << "[alpha] no frame" << std::endl; return 2; }
        const int x = 140, y = 150; auto Y = [&](const std::vector<uint8_t>& v) { return (int)v[(size_t)y * 640 + x]; };
        std::cout << "[alpha] Y at strip: background " << Y(base) << "  transparent RGBA layer " << Y(clear) << "  opaque black layer " << Y(opaque) << std::endl;
        std::cout << (std::abs(Y(base) - Y(clear)) <= 6 && Y(opaque) != Y(base) ? "[alpha] ALL PASSED (transparent layer leaves the video alone)" : "[alpha] FAILED (transparent layer changes the video)") << std::endl;
        return std::abs(Y(base) - Y(clear)) <= 6 ? 0 : 1;
    }
    // 4th arg "uvc": USB camera + hot-plug, driven through the real CameraFeeder with the "fake:<file>" hook (a camera "exists" while the file exists).
    if (argc > 4 && std::string(argv[4]) == "uvc") {
        int bad = 0; auto CHK = [&](bool ok, const char* w) { std::cout << "[uvc] " << (ok ? "ok   " : "FAIL ") << w << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        { GError* e = nullptr; GstElement* p = gst_parse_launch("v4l2src device=/dev/video99 ! image/jpeg,width=1280,height=720,framerate=15/1 ! jpegdec ! videoconvert ! videoscale "
              "! video/x-raw,format=I420,width=1280,height=720 ! appsink name=cam_out emit-signals=false sync=false max-buffers=2 drop=true", &e);
          CHK(p && !e, "USB camera sub-pipeline fragment parses and links (v4l2src ! image/jpeg ! jpegdec ! convert ! scale ! I420 ! appsink)"); if (e) { std::cout << "      " << e->message << std::endl; g_error_free(e); } if (p) gst_object_unref(p); }
        struct Rig {
            Config s; std::shared_ptr<Compositor> comp; std::unique_ptr<ui::UiController> ctl;
            std::atomic<int> pres{-1}, online_events{0}, offline_events{0}; std::atomic<int> out_frames{0};
            Rig(Config cfg, const char* tag) : s(cfg) {
                s.camera_rtsp = ""; s.phone_test = false; s.video_width = 640; s.video_height = 360; s.video_fps = 15; s.video_bitrate = 1000000; s.camera_width = 320; s.camera_height = 240; s.camera_fps = 15;
                s.prefs_path = std::string("/tmp/aa-prefs-") + tag + ".conf"; s.control_fifo = std::string("/tmp/aa-control-") + tag; s.status_path = std::string("/tmp/aa-status-") + tag + ".json"; std::remove(s.prefs_path.c_str());
                comp = std::make_shared<Compositor>(s); ctl = std::make_unique<ui::UiController>(s, comp);
                comp->setOutputCallback([this](const uint8_t*, size_t, uint64_t) { ++out_frames; });
                comp->setCameraPresenceCallback([this](bool on) { pres = on ? 1 : 0; if (on) ++online_events; else ++offline_events; ctl->setCameraPresent(on); });
            }
            void go() { comp->start(); ctl->start(); ctl->command("mode cam"); }
            void end() { ctl->stop(); comp->stop(); }
            bool waitFor(std::function<bool()> f, int ms) { for (int t = 0; t < ms; t += 250) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(250)); } return f(); }
        };
        {   // A: a configured camera that is not there: starts instantly, no camera, placeholder, video keeps flowing
            Config c; c.camera_test = false; c.camera_device = "/dev/v4l/by-id/*-video-index0";
            Rig r(c, "uvcA"); const auto t0 = std::chrono::steady_clock::now(); bool started = true;
            try { r.go(); } catch (const std::exception& e) { started = false; std::cout << "      " << e.what() << std::endl; }
            CHK(started && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3), "no camera attached: starts at once (no boot delay)");
            wait(3500); CHK(r.pres != 1 && !r.ctl->layoutCameraLive(), "no camera attached: offline, placeholder");
            int f0 = r.out_frames; wait(1500); CHK(r.out_frames > f0 + 5, "no camera attached: the video output keeps flowing");
            r.end();
        }
        {   // B: HOT-PLUG: absent -> plug -> online -> unplug -> offline -> replug -> online (all without restarting anything)
            const std::string f = "/tmp/fake-cam-B"; std::remove(f.c_str());
            Config c; c.camera_test = false; c.camera_device = "fake:" + f;
            Rig r(c, "uvcB"); r.go();
            wait(2500); CHK(r.pres != 1 && !r.ctl->layoutCameraLive(), "hot-plug: before plugging: offline placeholder");
            { std::ofstream(f) << "x"; }
            CHK(r.waitFor([&] { return r.pres == 1 && r.ctl->layoutCameraLive(); }, 15000), "hot-plug: camera plugged in AFTER boot -> online, pane live");
            int off0 = r.offline_events; std::remove(f.c_str());
            CHK(r.waitFor([&] { return r.pres == 0 && !r.ctl->layoutCameraLive(); }, 20000), "hot-plug: unplugged -> offline within seconds, placeholder back");
            CHK(r.offline_events == off0 + 1, "hot-plug: exactly one offline event");
            int f1 = r.out_frames; wait(1500); CHK(r.out_frames > f1 + 5, "hot-plug: the main video output never stopped");
            { std::ofstream(f) << "x"; }
            CHK(r.waitFor([&] { return r.pres == 1 && r.ctl->layoutCameraLive(); }, 15000), "hot-plug: replugged -> online again");
            std::remove(f.c_str()); r.end();
        }
        {   // C: a camera that cannot be opened (broken device / bad extender): retried, never fatal, video output unaffected
            const std::string f = "/tmp/fake-cam-C"; { std::ofstream(f) << "x"; }
            Config c; c.camera_test = false; c.camera_device = "fakebad:" + f;
            Rig r(c, "uvcC"); bool started = true; try { r.go(); } catch (...) { started = false; }
            CHK(started, "broken camera: main pipeline starts");
            wait(8000); CHK(r.pres != 1 && !r.ctl->layoutCameraLive(), "broken camera: offline, placeholder");
            int f0 = r.out_frames; wait(2000); CHK(r.out_frames > f0 + 5, "broken camera: video output keeps flowing (camera failure is isolated)");
            r.end(); std::remove(f.c_str());
        }
        {   // D: camera_test + a USB device spec: synthetic until a camera exists, real while it does, synthetic again after unplug (no offline gap)
            const std::string f = "/tmp/fake-cam-D"; std::remove(f.c_str());
            Config c; c.camera_test = true; c.camera_device = "fake:" + f;
            Rig r(c, "uvcD"); r.go();
            CHK(r.waitFor([&] { return r.pres == 1 && r.ctl->layoutCameraLive(); }, 15000), "synthetic fallback: online with no camera attached");
            { std::ofstream(f) << "x"; } wait(3000); CHK(r.pres == 1, "synthetic fallback: plugging a camera in keeps it online");
            std::remove(f.c_str()); wait(5000); CHK(r.pres == 1 && r.offline_events == 0, "synthetic fallback: unplug falls back to synthetic, never offline");
            r.end();
        }
        std::cout << (bad ? "[uvc] FAILED" : "[uvc] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "alpha2": closer replica of the real pipeline: black base + phone-like I420 pad + LIVE appsrc RGBA layer of zeros on top.
    if (argc > 4 && std::string(argv[4]) == "alpha2") {
        auto run = [&](bool overlay, const char* opts) {
            std::string d = std::string("compositor name=mix latency=50000000 sink_0::zorder=0 sink_1::zorder=1 sink_1::xpos=50 sink_1::width=500 sink_1::height=360 ") +
                (overlay ? std::string("sink_3::zorder=3 sink_3::alpha=" + std::string(std::string(opts).find("a1") != std::string::npos ? "1" : "0") + " sink_3::xpos=400 sink_3::ypos=20 sink_3::width=84 sink_3::height=310 ") : std::string()) +
                "! video/x-raw,format=" + std::string(std::string(opts).find("outA420") != std::string::npos ? "A420" : std::string(opts).find("outAYUV") != std::string::npos ? "AYUV" : "I420") + ",width=640,height=360 ! videoconvert ! video/x-raw,format=I420 ! appsink name=o max-buffers=1 sync=false "
                "videotestsrc is-live=true pattern=black ! video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! mix.sink_0 "
                "videotestsrc is-live=true pattern=smpte ! video/x-raw,format=I420,width=500,height=360,framerate=30/1 ! mix.sink_1 ";
            const std::string o = opts;
            if (overlay) d += std::string("appsrc name=ui format=time is-live=") + (o.find("nolive") != std::string::npos ? "false" : "true") + " do-timestamp=" + (o.find("nots") != std::string::npos ? "false" : "true") +
                " caps=video/x-raw,format=RGBA,width=84,height=310,framerate=" + (o.find("fps30") != std::string::npos ? "30/1" : "2/1") + " ! " + (o.find("conv") != std::string::npos ? "videoconvert ! video/x-raw,format=AYUV ! " : "") + "mix.sink_3 ";
            GError* e = nullptr; GstElement* pl = gst_parse_launch(d.c_str(), &e); if (!pl) { std::cout << "[alpha2] parse " << (e ? e->message : "?") << std::endl; return -1; }
            GstElement* sink = gst_bin_get_by_name(GST_BIN(pl), "o"); gst_element_set_state(pl, GST_STATE_PLAYING);
            GstPad* pad = nullptr; if (overlay) { GstElement* mix = gst_bin_get_by_name(GST_BIN(pl), "mix"); pad = gst_element_get_static_pad(mix, "sink_3"); if (!pad) { GstIterator* it = gst_element_iterate_sink_pads(mix); GValue v = G_VALUE_INIT; while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) { GstPad* p = GST_PAD(g_value_get_object(&v)); gchar* n = gst_pad_get_name(p); if (std::string(n) == "sink_3") pad = (GstPad*)gst_object_ref(p); g_free(n); g_value_reset(&v); } } }
            if (overlay) {
                GstElement* src = gst_bin_get_by_name(GST_BIN(pl), "ui");
                auto push = [&] { GstBuffer* b = gst_buffer_new_allocate(nullptr, 84 * 310 * 4, nullptr); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_WRITE); if (o.find("half") != std::string::npos) { for (size_t i = 0; i < m.size; i += 4) { m.data[i] = m.data[i+1] = m.data[i+2] = 0; m.data[i+3] = 128; } } else if (o.find("white0") != std::string::npos) { for (size_t i = 0; i < m.size; i += 4) { m.data[i] = m.data[i+1] = m.data[i+2] = 255; m.data[i+3] = 0; } } else memset(m.data, 0, m.size); gst_buffer_unmap(b, &m); gst_app_src_push_buffer(GST_APP_SRC(src), b); };
                push(); if (pad) g_object_set(pad, "alpha", 1.0, nullptr);
                for (int k = 0; k < 6; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); push(); }
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
            int y = -1; GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
            for (int k = 0; k < 40 && s; ++k) { GstSample* n = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND); if (!n) break; gst_sample_unref(s); s = n; }
            if (s) { GstBuffer* b = gst_sample_get_buffer(s); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_READ); y = m.data[(size_t)150 * 640 + 440]; gst_buffer_unmap(b, &m); gst_sample_unref(s); }
            gst_element_set_state(pl, GST_STATE_NULL); gst_object_unref(pl); (void)opts; return y; };
        // The production pipeline (A420 mixer output): a transparent RGBA layer must not change the picture, half alpha must blend 50/50.
        int base = run(false, "outA420"), withLayer = run(true, "outA420");
        std::cout << "[alpha2] Y at strip: without layer " << base << "  with transparent live layer " << withLayer << std::endl;
        { int half = run(true, "half,outA420"); std::cout << "[alpha2] half-alpha black over Y " << base << " -> " << half << std::endl; if (std::abs(half - (base + 16) / 2) > 6) { std::cout << "[alpha2] FAILED: half alpha did not blend" << std::endl; return 1; } }
        for (const char* v : {"white0,outA420", "white0"}) std::cout << "[alpha2] variant " << v << " -> Y " << run(true, v) << std::endl;
        std::cout << ((base > 0 && std::abs(base - withLayer) <= 6) ? "[alpha2] ALL PASSED" : "[alpha2] FAILED: a transparent live RGBA layer changes the picture") << std::endl;
        return (base > 0 && std::abs(base - withLayer) <= 6) ? 0 : 1;
    }
    // 4th arg "basecheck": the Android Auto splash is VIDEO under the camera (no hole in an overlay). Real Compositor + UI, NO phone, live (synthetic) camera in PiP mode; the
    // ENCODED output is decoded again and the pixels are checked: splash where there is no PiP, camera inside the PiP, and after the PiP is moved the old spot shows the splash again.
    // 4th arg "cornerlive": rounded PiP corners over a LIVE phone picture (a plain white test picture). With pip_corner_restore the encoded output must show the
    // phone's white in the four corners (the video path copies the phone's pixels back); with it off the overlay's dark fill shows (the old behaviour).
    if (argc > 4 && std::string(argv[4]) == "cornerlive") {
        setenv("AA_PHONE_PATTERN", "white", 1);
        int bad = 0; auto CHK = [&](bool ok, const char* w) { std::cout << "[corner] " << (ok ? "ok   " : "FAIL ") << w << std::endl; if (!ok) ++bad; };
        for (int pass = 0; pass < 2; ++pass) {
            Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 6; s.video_bitrate = 4000000;
            s.camera_width = 640; s.camera_height = 360; s.camera_fps = 10; s.pip_corner_restore = (pass == 0); s.pip_corners = true;
            s.prefs_path = "/tmp/aa-prefs-corner.conf"; s.control_fifo = "/tmp/aa-control-corner"; s.status_path = "/tmp/aa-status-corner.json"; std::remove(s.prefs_path.c_str());
            auto comp = std::make_shared<Compositor>(s); ui::UiController ctl(s, comp);
            std::vector<std::vector<uint8_t>> stream; std::mutex sm;
            comp->setOutputCallback([&](const uint8_t* d, size_t sz, uint64_t) { std::lock_guard<std::mutex> l(sm); stream.emplace_back(d, d + sz); });
            comp->start(); ctl.start(); ctl.setTouchGeometry(1280, 720);
            auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
            wait(2500); ctl.command("size 0.45"); ctl.command("corner br"); ctl.command("mode pip"); wait(5000);
            int kA; { std::lock_guard<std::mutex> l(sm); kA = (int)stream.size(); } const ui::Rect rA = ctl.cameraRect();
            ctl.stop(); comp->stop();
            GError* e = nullptr; GstElement* dec = gst_parse_launch("appsrc name=d format=time caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ! openh264dec ! videoconvert ! video/x-raw,format=I420 ! appsink name=o sync=false", &e);
            if (!dec) { std::cout << "[corner] decoder: " << (e ? e->message : "?") << std::endl; return 2; }
            GstElement* dsrc = gst_bin_get_by_name(GST_BIN(dec), "d"); GstElement* dsink = gst_bin_get_by_name(GST_BIN(dec), "o"); gst_element_set_state(dec, GST_STATE_PLAYING);
            std::vector<uint8_t> luma; int W = 0, H = 0;
            std::thread puller([&] { int n = 0; while (GstSample* sp = gst_app_sink_pull_sample(GST_APP_SINK(dsink))) { ++n;
                if (n == kA) { GstStructure* st = gst_caps_get_structure(gst_sample_get_caps(sp), 0); gst_structure_get_int(st, "width", &W); gst_structure_get_int(st, "height", &H);
                    GstBuffer* b = gst_sample_get_buffer(sp); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_READ); luma.assign(m.data, m.data + (size_t)W * H); gst_buffer_unmap(b, &m); }
                gst_sample_unref(sp); } });
            for (auto& f : stream) { GstBuffer* b = gst_buffer_new_allocate(nullptr, f.size(), nullptr); gst_buffer_fill(b, 0, f.data(), f.size()); gst_app_src_push_buffer(GST_APP_SRC(dsrc), b); }
            gst_app_src_end_of_stream(GST_APP_SRC(dsrc)); puller.join(); gst_element_set_state(dec, GST_STATE_NULL);
            if (luma.empty()) { std::cout << "[corner] FAILED: checkpoint frame missing" << std::endl; return 1; }
            const int d = 2; const int px[4][2] = {{rA.x + d, rA.y + d}, {rA.x + rA.w - 1 - d, rA.y + d}, {rA.x + d, rA.y + rA.h - 1 - d}, {rA.x + rA.w - 1 - d, rA.y + rA.h - 1 - d}};
            int bright = 0, dark = 0; for (auto& p : px) { const int y = luma[(size_t)p[1] * W + p[0]]; if (y > 190) ++bright; if (y < 90) ++dark; }
            int camMid = luma[(size_t)(rA.y + rA.h / 2) * W + rA.x + rA.w / 2];
            std::cout << "[corner] pass " << pass << (pass == 0 ? " (restore ON)" : " (restore OFF)") << ": pip " << rA.x << "," << rA.y << " " << rA.w << "x" << rA.h << "; corners bright " << bright << "/4 dark " << dark << "/4; camera centre luma " << camMid << std::endl;
            if (pass == 0) { CHK(bright >= 4, "restore ON: all four corners show the phone's picture (white), not a dark fill"); CHK(camMid < 235, "the PiP itself still shows the camera"); }
            else CHK(dark >= 3, "restore OFF: the corners are the dark fill (the old behaviour is still reachable)");
        }
        std::cout << (bad ? "[corner] FAILED" : "[corner] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "idleskip": with idle_skip=1 a screen that is not changing is encoded at the idle heartbeat only; the same pipeline with a live camera keeps (much) more.
    if (argc > 4 && std::string(argv[4]) == "idleskip") {
        auto measure = [&](bool camera, bool skip, long* skipped) {
            Config s; s.phone_test = false; s.camera_test = camera; s.camera_rtsp = ""; s.video_width = 640; s.video_height = 360; s.video_fps = 10; s.video_bitrate = 1000000;
            s.camera_width = 320; s.camera_height = 180; s.camera_fps = 10; s.idle_skip = skip; s.idle_fps = 2; s.idle_hold_ms = 400;
            s.prefs_path = "/tmp/aa-prefs-idle.conf"; s.control_fifo = "/tmp/aa-control-idle"; s.status_path = "/tmp/aa-status-idle.json"; std::remove(s.prefs_path.c_str());
            auto comp = std::make_shared<Compositor>(s); ui::UiController ctl(s, comp);
            std::atomic<int> n{0}; comp->setOutputCallback([&](const uint8_t*, size_t, uint64_t) { ++n; });
            comp->start(); ctl.start(); ctl.setTouchGeometry(640, 360);
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            const int a = n; std::this_thread::sleep_for(std::chrono::milliseconds(6000)); const int b = n;
            ctl.stop(); comp->stop(); if (skipped) *skipped = 0; return b - a; };
        const int idle_on = measure(false, true, nullptr), idle_off = measure(false, false, nullptr), cam_on = measure(true, true, nullptr);
        std::cout << "[idle] frames in a 6 s window: static screen skip ON " << idle_on << ", skip OFF " << idle_off << ", live camera skip ON " << cam_on << std::endl;
        int bad = 0; auto CHK = [&](bool ok, const char* w) { std::cout << "[idle] " << (ok ? "ok   " : "FAIL ") << w << std::endl; if (!ok) ++bad; };
        CHK(idle_off >= 20, "baseline: without the skip a static screen is encoded at the full rate");
        CHK(idle_on >= 8 && idle_on <= 20, "static screen with the skip: the 2 fps heartbeat keeps frames flowing (never stalls)");
        CHK(idle_on * 2 < idle_off, "static screen with the skip: less than half the frames of the baseline");
        CHK(cam_on * 10 >= idle_off * 7, "live camera visible: the skip does not throttle (>= 70% of the baseline)");
        std::cout << (bad ? "[idle] FAILED" : "[idle] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    if (argc > 4 && std::string(argv[4]) == "basecheck") {
        Config s; s.phone_test = false; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = argc > 5 ? std::stoi(argv[5]) : 6; s.video_bitrate = 4000000;
        s.camera_width = 640; s.camera_height = 360; s.camera_fps = 10; if (const char* v = getenv("AA_BASEART")) s.base_art = atoi(v) != 0;
        s.prefs_path = "/tmp/aa-prefs-base.conf"; s.control_fifo = "/tmp/aa-control-base"; s.status_path = "/tmp/aa-status-base.json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s); ui::UiController ctl(s, comp);
        std::vector<std::vector<uint8_t>> stream; std::mutex sm;
        comp->setOutputCallback([&](const uint8_t* d, size_t sz, uint64_t) { std::lock_guard<std::mutex> l(sm); stream.emplace_back(d, d + sz); });
        comp->start(); ctl.start(); ctl.setTouchGeometry(1280, 720);
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        auto count = [&]() { std::lock_guard<std::mutex> l(sm); return (int)stream.size(); };
        wait(2500); ctl.command("mode pip"); ctl.command("corner br"); wait(4000);
        const int kA = count(); const ui::Rect rA = ctl.cameraRect(); const int alphaArt = ctl.overlayAlphaAt(40, 40);
        ctl.command("corner tl"); wait(4000);
        const int kB = count(); const ui::Rect rB = ctl.cameraRect();
        ctl.stop(); comp->stop();
        std::cout << "[base] encoded " << stream.size() << " frames; A=" << kA << " pip " << rA.x << "," << rA.y << " " << rA.w << "x" << rA.h << "  B=" << kB << " pip " << rB.x << "," << rB.y << std::endl;
        struct Img { std::vector<uint8_t> y; int w = 0, h = 0; }; std::map<int, Img> got;
        GError* e = nullptr; GstElement* dec = gst_parse_launch("appsrc name=d format=time caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ! openh264dec ! videoconvert ! video/x-raw,format=I420 ! appsink name=o sync=false", &e);
        if (!dec) { std::cout << "[base] decoder pipeline: " << (e ? e->message : "?") << std::endl; return 2; }
        GstElement* dsrc = gst_bin_get_by_name(GST_BIN(dec), "d"); GstElement* dsink = gst_bin_get_by_name(GST_BIN(dec), "o"); gst_element_set_state(dec, GST_STATE_PLAYING);
        std::thread puller([&] { int n = 0; while (GstSample* sp = gst_app_sink_pull_sample(GST_APP_SINK(dsink))) { ++n;
            if (n == kA || n == kB) { Img im; GstStructure* st = gst_caps_get_structure(gst_sample_get_caps(sp), 0); gst_structure_get_int(st, "width", &im.w); gst_structure_get_int(st, "height", &im.h);
                GstBuffer* b = gst_sample_get_buffer(sp); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_READ); im.y.assign(m.data, m.data + (size_t)im.w * im.h); gst_buffer_unmap(b, &m); got[n] = std::move(im); }
            gst_sample_unref(sp); } });
        for (auto& f : stream) { GstBuffer* b = gst_buffer_new_allocate(nullptr, f.size(), nullptr); gst_buffer_fill(b, 0, f.data(), f.size()); gst_app_src_push_buffer(GST_APP_SRC(dsrc), b); }
        gst_app_src_end_of_stream(GST_APP_SRC(dsrc)); puller.join(); gst_element_set_state(dec, GST_STATE_NULL);
        int bad = 0; auto CHK = [&](bool ok, const char* w) { std::cout << "[base] " << (ok ? "ok   " : "FAIL ") << w << std::endl; if (!ok) ++bad; };
        if (!got.count(kA) || !got.count(kB)) { std::cout << "[base] FAILED: checkpoint frames missing" << std::endl; return 1; }
        // expected splash luma (what the base layer should carry), computed the way the UI does
        const int W = 1280, H = 720; std::vector<uint8_t> art = ui::placeholderCover(ui::Art::AndroidAuto, W, H, 0);
        ui::YuvaLayer L = ui::YuvaLayer::fromRgba(art.data(), W, H, 0, 0);
        auto match = [&](const Img& im, ui::Rect r, bool inside, int pad) {           // share of sampled pixels whose luma equals the splash (within 28)
            long n = 0, same = 0; for (int y = r.y; y < r.y + r.h; y += 3) for (int x = r.x; x < r.x + r.w; x += 3) {
                if (inside && (x < r.x + pad || y < r.y + pad || x >= r.x + r.w - pad || y >= r.y + r.h - pad)) continue; ++n;
                if (std::abs((int)im.y[(size_t)y * im.w + x] - (int)L.Y[(size_t)y * W + x]) <= 28) ++same; }
            return n ? (double)same / n : -1.0; };
        const Img &A = got[kA], &B = got[kB];
        CHK(A.w == W && A.h == H, "output decodes to 1280x720");
        if (const char* dump = getenv("AA_BASE_DUMP")) { std::ofstream o(std::string(dump) + "/base-A.pgm", std::ios::binary); o << "P5\n" << A.w << " " << A.h << "\n255\n"; o.write((const char*)A.y.data(), A.y.size()); }
        auto outside = [&](const Img& im, const ui::Rect& pip) { long n = 0, same = 0; for (int y = 0; y < H; y += 5) for (int x = 0; x < W; x += 5) { if (x >= pip.x - 8 && x < pip.x + pip.w + 8 && y >= pip.y - 8 && y < pip.y + pip.h + 8) continue; ++n;
            if (std::abs((int)im.y[(size_t)y * im.w + x] - (int)L.Y[(size_t)y * W + x]) <= 28) ++same; } return (double)same / n; };
        const double aOut = outside(A, rA), bOut = outside(B, rB), aIn = match(A, rA, true, 8), bIn = match(B, rB, true, 8), bOld = match(B, rA, true, 8);
        std::cout << "[base] splash match: A outside " << (int)(aOut * 100) << "%  A inside PiP " << (int)(aIn * 100) << "%  B outside " << (int)(bOut * 100) << "%  B inside new PiP " << (int)(bIn * 100) << "%  B at the OLD PiP spot " << (int)(bOld * 100) << "%" << std::endl;
        CHK(aOut > 0.95, "A: everything outside the PiP is the Android Auto splash");
        CHK(aIn < 0.5, "A: inside the PiP it is the camera picture, not the splash");
        CHK(bOut > 0.95, "B (PiP moved): everything outside the PiP is the splash");
        CHK(bIn < 0.5, "B: the camera is at its new place");
        CHK(bOld > 0.9, "B: the OLD PiP spot shows the splash again (nothing stayed behind)");
        if (comp->baseArt() && (s.pip_round || s.pip_corners)) {      // rounded corners: the corner pixels reveal the splash underneath, the middle of each edge is the camera
            auto cornerSame = [&](const Img& im, ui::Rect r) { int same = 0, n = 0; const int d = 2;
                const int px[4][2] = {{r.x + d, r.y + d}, {r.x + r.w - 1 - d, r.y + d}, {r.x + d, r.y + r.h - 1 - d}, {r.x + r.w - 1 - d, r.y + r.h - 1 - d}};
                for (auto& p : px) { ++n; if (std::abs((int)im.y[(size_t)p[1] * im.w + p[0]] - (int)L.Y[(size_t)p[1] * W + p[0]]) <= 28) ++same; } return same; };
            auto edgeDiff = [&](const Img& im, ui::Rect r) { int diff = 0; long n = 0; for (int x = r.x + r.w / 4; x < r.x + 3 * r.w / 4; x += 4) { for (int yy : {r.y + 4, r.y + r.h - 5}) { ++n;
                if (std::abs((int)im.y[(size_t)yy * im.w + x] - (int)L.Y[(size_t)yy * W + x]) > 28) ++diff; } } return n ? (double)diff / n : 0.0; };
            std::cout << "[base] corners showing the splash: A " << cornerSame(A, rA) << "/4  B " << cornerSame(B, rB) << "/4; edge middles that are camera: A " << (int)(edgeDiff(A, rA) * 100) << "% B " << (int)(edgeDiff(B, rB) * 100) << "%" << std::endl;
            CHK(cornerSame(A, rA) >= 3, "A: the PiP corners are rounded (corner pixels show the splash behind)");
            CHK(cornerSame(B, rB) >= 3, "B: still rounded after the move");
            CHK(edgeDiff(A, rA) > 0.5, "A: the straight edges of the PiP are still the camera picture");
        }
        if (comp->baseArt()) CHK(alphaArt == 0, "the splash is NOT painted on the overlay (overlay is transparent there)");
        std::cout << (bad ? "[base] FAILED" : "[base] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "pillcheck": truck-26 scenario end to end. Real Compositor + UI at the truck configuration (1920x1080, synthetic camera, NO phone, AA placeholder
    // showing), a touch, and the ENCODED output is decoded again (offline, after the run: emulated 1080p decoding is slower than real time) so we look at
    // what Sync 4 would show: does the pill appear on touch, and go away later?
    if (argc > 4 && std::string(argv[4]) == "pillcheck") {
        Config s; s.phone_test = false; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1920; s.video_height = 1080; s.video_fps = argc > 5 ? std::stoi(argv[5]) : 6; s.video_bitrate = 8000000;
        s.camera_width = 1280; s.camera_height = 720; s.camera_fps = 15; if (const char* v = getenv("AA_MIXER_ALPHA")) s.mixer_alpha = atoi(v); if (const char* v = getenv("AA_UIBLEND")) s.ui_blend = atoi(v);
        if (const char* v = getenv("AA_CAMTEST")) s.camera_test = atoi(v) != 0;
        s.prefs_path = "/tmp/aa-prefs-pill.conf"; s.control_fifo = "/tmp/aa-control-pill"; s.status_path = "/tmp/aa-status-pill.json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s); ui::UiController ctl(s, comp);
        std::vector<std::vector<uint8_t>> stream; std::mutex sm;
        comp->setOutputCallback([&](const uint8_t* d, size_t sz, uint64_t) { std::lock_guard<std::mutex> l(sm); stream.emplace_back(d, d + sz); });
        comp->start(); ctl.start(); ctl.setTouchGeometry(1200, 1244); ctl.setVisibleWindow(438, 0, 1042, 1080);
        ctl.command("mode aa");                               // the card's prefs say start_mode=aa (fresh prefs would start in the camera)
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        auto count = [&]() { std::lock_guard<std::mutex> l(sm); return (int)stream.size(); };
        wait(9000); const int k_before = count();
        { TouchFrame f; f.action = 0; f.index = 0; f.p = {{0, 600, 500}}; ctl.onTouchFrame(f); wait(80); f.action = 1; ctl.onTouchFrame(f); }
        wait(3000); const int k_awake = count();           // 3 s after the tap: the pill must be up (4 s timeout)
        wait(8000); const int k_asleep = count();          // 11 s after: gone
        ctl.stop(); comp->stop();
        std::cout << "[pill] encoded " << stream.size() << " frames; checkpoints before=" << k_before << " awake=" << k_awake << " asleep=" << k_asleep << std::endl;
        // offline decode of the whole stream, keeping the luma of the checkpoint frames
        struct Img { std::vector<uint8_t> y; int w = 0, h = 0; }; std::map<int, Img> got;
        GError* e = nullptr; GstElement* dec = gst_parse_launch("appsrc name=d format=time caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ! openh264dec ! videoconvert ! video/x-raw,format=I420 ! appsink name=o sync=false max-buffers=4 drop=false", &e);
        if (!dec) { std::cout << "[pill] decoder pipeline: " << (e ? e->message : "?") << std::endl; return 2; }
        GstElement* dsrc = gst_bin_get_by_name(GST_BIN(dec), "d"); GstElement* dsink = gst_bin_get_by_name(GST_BIN(dec), "o"); gst_element_set_state(dec, GST_STATE_PLAYING);
        std::thread puller([&] { int n = 0; while (GstSample* sp = gst_app_sink_pull_sample(GST_APP_SINK(dsink))) { ++n;
            if (n == k_before || n == k_awake || n == k_asleep) { Img im; GstStructure* st = gst_caps_get_structure(gst_sample_get_caps(sp), 0); gst_structure_get_int(st, "width", &im.w); gst_structure_get_int(st, "height", &im.h);
                GstBuffer* b = gst_sample_get_buffer(sp); GstMapInfo m; gst_buffer_map(b, &m, GST_MAP_READ); im.y.assign(m.data, m.data + (size_t)im.w * im.h); gst_buffer_unmap(b, &m); got[n] = std::move(im); }
            gst_sample_unref(sp); } std::cout << "[pill] decoded " << n << " frames" << std::endl; });
        for (auto& f : stream) { GstBuffer* b = gst_buffer_new_allocate(nullptr, f.size(), nullptr); gst_buffer_fill(b, 0, f.data(), f.size()); gst_app_src_push_buffer(GST_APP_SRC(dsrc), b); }
        gst_app_src_end_of_stream(GST_APP_SRC(dsrc)); puller.join(); gst_element_set_state(dec, GST_STATE_NULL);
        auto diff = [&](const Img& a, const Img& b, int& x0, int& y0, int& x1, int& y1) { long n = 0; x0 = y0 = 1 << 30; x1 = y1 = -1; if (a.w != b.w || a.h != b.h || a.y.empty()) return -1L;
            for (int y = 0; y < a.h; ++y) for (int x = 0; x < a.w; ++x) if (std::abs((int)a.y[(size_t)y * a.w + x] - (int)b.y[(size_t)y * a.w + x]) > 24) { ++n; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); } return n; };
        int bad = 0; auto CHK = [&](bool ok, const char* w) { std::cout << "[pill] " << (ok ? "ok   " : "FAIL ") << w << std::endl; if (!ok) ++bad; };
        if (!got.count(k_before) || !got.count(k_awake) || !got.count(k_asleep)) { std::cout << "[pill] FAILED: checkpoint frames missing from the decode" << std::endl; return 1; }
        const Img &before = got[k_before], &awake = got[k_awake], &asleep = got[k_asleep];
        CHK(before.w == 1920 && before.h == 1080, "output decodes to 1920x1080");
        if (const char* dump = getenv("AA_PILL_DUMP")) for (auto& kv : std::vector<std::pair<const char*, const Img*>>{{"before", &before}, {"awake", &awake}, {"asleep", &asleep}}) {   // luma as PGM, for looking at it
            std::ofstream o(std::string(dump) + "/pill-" + kv.first + ".pgm", std::ios::binary); o << "P5\n" << kv.second->w << " " << kv.second->h << "\n255\n"; o.write((const char*)kv.second->y.data(), kv.second->y.size()); }
        int x0, y0, x1, y1; long n = diff(before, awake, x0, y0, x1, y1);
        std::cout << "[pill] 3 s after the tap: " << n << " pixels changed, bbox x " << x0 << ".." << x1 << " y " << y0 << ".." << y1 << std::endl;
        CHK(n > 3000, "the tap changes the picture (pill appears)"); CHK(n > 0 && x0 >= 1300 && x1 <= 1480, "the change is at the right edge of the visible window (the pill)");
        n = diff(before, asleep, x0, y0, x1, y1); std::cout << "[pill] 11 s after: " << n << " pixels differ from the start picture" << std::endl;
        CHK(n < 600, "the pill is gone again after its timeout");
        std::cout << (bad ? "[pill] FAILED" : "[pill] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "pipe": micro-benchmark. argv[5] = a gst-launch style pipeline that ends by itself (num-buffers=N); prints the wall time to EOS.
    if (argc > 5 && std::string(argv[4]) == "pipe") {
        GError* e = nullptr; GstElement* pl = gst_parse_launch(argv[5], &e);
        if (!pl) { std::cout << "[pipe] parse error: " << (e ? e->message : "?") << std::endl; return 2; }
        const auto t0 = std::chrono::steady_clock::now();
        gst_element_set_state(pl, GST_STATE_PLAYING);
        GstBus* bus = gst_element_get_bus(pl);
        GstMessage* m = gst_bus_timed_pop_filtered(bus, 280 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) { GError* er = nullptr; gst_message_parse_error(m, &er, nullptr); std::cout << "[pipe] ERROR " << (er ? er->message : "?") << std::endl; return 3; }
        std::cout << "[pipe] " << s << " s" << std::endl; gst_element_set_state(pl, GST_STATE_NULL); return 0;
    }
    // 4th arg "fuzz": random touch frames (any action, 0..6 fingers, hostile coordinates, duplicate ids), random/garbage commands, sensor values and
    // presence flips against the REAL compositor+UI. Invariants after every step: layout inside the frame, prefs sane, forwarded touches finite
    // and inside a sane box. A crash/hang fails the run. Seeded: argv[5] (default 1) reproduces a run.
    if (argc > 4 && std::string(argv[4]) == "fuzz") {
        Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
        const std::string fz = "fuzz" + std::string(argc > 5 ? argv[5] : "1"); s.prefs_path = "/tmp/aa-prefs-" + fz + ".conf"; s.control_fifo = "/tmp/aa-control-" + fz; s.status_path = "/tmp/aa-status-" + fz + ".json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s); ui::UiController ctl(s, comp);
        long long fwd_n = 0, fwd_bad = 0; int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30); std::mutex fm;
        ctl.setPhoneTouchForwarder([&](const TouchFrame& f) { std::lock_guard<std::mutex> l(fm); ++fwd_n;
            for (auto& q : f.p) { minx = std::min(minx, q.x); maxx = std::max(maxx, q.x); miny = std::min(miny, q.y); maxy = std::max(maxy, q.y); if (q.x < -2000 || q.x > 6000 || q.y < -2000 || q.y > 6000) ++fwd_bad; } });
        comp->start(); ctl.start(); ctl.setTouchGeometry(1200, 1244);
        const unsigned seed = argc > 5 ? (unsigned)std::stoul(argv[5]) : 1u; std::mt19937 rng(seed);
        const int iters = argc > 6 ? std::stoi(argv[6]) : 6000;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        auto R = [&](int lo, int hi) { return lo + (int)(rng() % (unsigned)(hi - lo + 1)); };
        auto coord = [&](int lim) { switch (rng() % 8) { case 0: return -(int)(rng() % 5000); case 1: return lim + (int)(rng() % 5000); case 2: return 0; case 3: return lim; case 4: return INT32_MAX; case 5: return INT32_MIN; default: return R(0, lim); } };
        const char* cmds[] = {"touch","mode","start","camfill","mirror","flip","zoom","zoomreset","corner","size","split","autocam","roi","phone","camera","moving","reverse","dbg","truck","menu","save","bogus","","  ","mode ","zoom nan","split inf","roi a b c d","size 99999999999999999999","truck 99 -1","truck 5 2147483647","menu \xff\xfe","mode pip x y z"};
        const char* words[] = {"aa","cam","pip","split","settings","debug","zone","hidden","home","nan","inf","-1","0","1","2","3","0.5","9e99","tl","tr","bl","br","s","m","l","xl","on","off","reverse","drive","park","night","day","moving","parked"};
        int bad = 0; long long checks = 0; const auto t0 = std::chrono::steady_clock::now();
        auto invariants = [&](int it, const char* what) {
            ++checks; ui::Prefs p = ctl.prefs(); ui::Rect cr = ctl.cameraRect();
            bool ok = std::isfinite(p.pip_frac) && p.pip_frac >= 0.15 && p.pip_frac <= 0.85 && std::isfinite(p.split_ratio) && std::isfinite(p.cam_zoom) && p.cam_zoom >= 1 && p.cam_zoom <= 4 &&
                      p.cam_cx >= 0 && p.cam_cx <= 1 && p.cam_cy >= 0 && p.cam_cy <= 1 && p.roi.w > 0 && p.roi.h > 0 && p.roi.x >= 0 && p.roi.y >= 0 &&
                      cr.x >= 0 && cr.y >= 0 && cr.x + cr.w <= 1280 && cr.y + cr.h <= 720 && cr.w >= 0 && cr.h >= 0;
            if (!ok && bad++ < 8) std::cout << "[fuzz] INVARIANT BROKEN at iter " << it << " after " << what << " (mode " << (int)ctl.mode() << "): pip_frac=" << p.pip_frac << " split=" << p.split_ratio << " zoom=" << p.cam_zoom << " cam=(" << p.cam_cx << "," << p.cam_cy << ") roi=" << p.roi.x << "," << p.roi.y << "," << p.roi.w << "," << p.roi.h << " camrect=" << cr.x << "," << cr.y << "," << cr.w << "," << cr.h << std::endl;
        };
        for (int it = 0; it < iters; ++it) {
            switch (rng() % 10) {
            case 0: case 1: case 2: case 3: case 4: case 5: {         // touch frame
                TouchFrame f; f.action = (rng() % 7 == 0) ? R(-3, 12) : (int[]){0, 1, 2, 2, 2, 3, 5, 6}[rng() % 8]; int n = R(0, 6);
                for (int k = 0; k < n; ++k) f.p.push_back({(rng() % 4 == 0) ? (int)(rng() % 3) : R(0, 9), coord(1200), coord(1244)});
                f.index = (rng() % 5 == 0) ? R(-3, 9) : (n ? R(0, n - 1) : 0); ctl.onTouchFrame(f); invariants(it, "touch frame"); break; }
            case 6: ctl.onTouch(coord(1200), coord(1244), R(-2, 7)); invariants(it, "onTouch"); break;
            case 7: { std::string l = cmds[rng() % (sizeof cmds / sizeof *cmds)]; int n = R(0, 3); for (int k = 0; k < n; ++k) l += std::string(" ") + words[rng() % (sizeof words / sizeof *words)]; ctl.command(l); invariants(it, l.c_str()); break; }
            case 8: ctl.setTruckSensor(R(-1, 9), (rng() % 3 == 0) ? (int)rng() : R(-2, 200)); break;
            case 9: switch (rng() % 6) { case 0: ctl.setPhonePresent(rng() & 1); break; case 1: ctl.setCameraPresent(rng() & 1); break; case 2: ctl.onPhoneVideo(); break;
                                         case 3: ctl.setReverse(rng() & 1); break; case 4: ctl.setMoving(rng() & 1); break; default: ctl.setVisibleWindow(R(-50, 300), R(-50, 300), (rng() % 4) ? 1042 : R(-10, 3000), (rng() % 4) ? 1080 : R(-10, 3000)); } invariants(it, "state"); break;
            }
            if (it % 500 == 0) { std::cout << "[fuzz] progress " << it << "/" << iters << " (" << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() << " s)" << std::endl; }
            if (it % 400 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));       // let the 1 Hz / render threads interleave
        }
        std::cout << "[fuzz] seed " << seed << ": " << iters << " steps, " << checks << " invariant checks, " << fwd_n << " frames forwarded to the phone, forwarded x " << minx << ".." << maxx << " y " << miny << ".." << maxy << ", " << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        ctl.stop(); comp->stop();
        bool ok = bad == 0 && fwd_bad == 0;
        std::cout << (ok ? "[fuzz] ALL PASSED" : "[fuzz] FAILED") << " (broken invariants " << bad << ", wild forwarded coords " << fwd_bad << ")" << std::endl; return ok ? 0 : 1;
    }
    // 4th arg "reshape": the Split layout asks the phone to re-lay its UI out for the 16:9 top pane (UpdateUiConfig), shows a scaled fallback until the phone
    // answers, keeps the fallback when it never answers, and restores the full window when the layout is left or a new phone session starts.
    // 4th arg "winchange": the phone connected before the vehicle announced its window (ui51a) and was laid out for the whole frame; when the window
    // arrives the phone must be told the new layout (one UpdateUiConfig with the window's margins), in Split and in a full-window mode alike.
    if (argc > 4 && std::string(argv[4]) == "winchange") {
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[winchange] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
        s.prefs_path = "/tmp/aa-prefs-winchange.conf"; s.control_fifo = "/tmp/aa-control-winchange"; s.status_path = "/tmp/aa-status-winchange.json";
        std::remove("/tmp/aa-prefs-winchange.conf");
        auto comp = std::make_shared<Compositor>(s);
        ui::UiController ctl(s, comp);
        struct Req { int l, r, t, b; }; std::vector<Req> reqs; std::mutex rm;
        ctl.setUiConfigSender([&](int l, int r, int t, int b) { std::lock_guard<std::mutex> g(rm); reqs.push_back({l, r, t, b}); });
        comp->start(); ctl.start();
        ctl.command("mode aa"); ctl.setPhonePresent(true); wait(800);                    // phone up, window still the whole frame (no vehicle yet)
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.empty(), "full-window mode, no vehicle window yet: nothing sent"); }
        ctl.setTouchGeometry(800, 720); wait(400);                                         // the vehicle's touch aspect arrives first (as on the truck)
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 1 && reqs[0].l == 240 && reqs[0].r == 240 && reqs[0].t == 0 && reqs[0].b == 0, "window arrives with the phone live: ONE full-window layout request with the window's margins"); }
        ctl.reshapeReply(); wait(300);
        CHK(ctl.layoutPhoneLive() && ctl.phoneRect().w == 800, "phone answered: live, laid out for the 800 px window");
        ctl.setVisibleWindow(240, 0, 800, 720); wait(300);                                 // then the margins say the same window
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 1, "same window again: nothing sent"); }
        // Split: the pane request is re-issued for the new window
        ctl.command("split 0.5"); ctl.command("mode split"); wait(400); ctl.reshapeReply(); wait(300);
        size_t n1; { std::lock_guard<std::mutex> g(rm); n1 = reqs.size(); CHK(n1 == 2 && reqs.back().b == 360, "Split: pane request for the 360 px pane"); }
        ctl.setVisibleWindow(200, 0, 880, 720); wait(500);
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == n1 + 1 && reqs.back().l == 200 && reqs.back().r == 200 && reqs.back().b == 360, "window changes in Split: the pane request is re-issued with the new margins"); }
        ctl.reshapeReply(); wait(300);
        CHK(ctl.phoneRect().w == 880, "...and the pane follows the new window");
        ctl.stop(); comp->stop();
        std::cout << (bad ? "[winchange] FAIL" : "[winchange] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    if (argc > 4 && std::string(argv[4]) == "reshape") {
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[reshape] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        for (int pass = 0; pass < 2; ++pass) {          // 0: the phone answers   1: the phone never answers
            Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
            s.prefs_path = "/tmp/aa-prefs-reshape.conf"; s.control_fifo = "/tmp/aa-control-reshape"; s.status_path = "/tmp/aa-status-reshape.json";
            std::remove("/tmp/aa-prefs-reshape.conf");
            auto comp = std::make_shared<Compositor>(s);
            ui::UiController ctl(s, comp);
            struct Req { int l, r, t, b; }; std::vector<Req> reqs; std::mutex rm;
            ctl.setUiConfigSender([&](int l, int r, int t, int b) { std::lock_guard<std::mutex> g(rm); reqs.push_back({l, r, t, b}); });
            comp->start(); ctl.start(); ctl.setTouchGeometry(800, 720);
            ctl.setVisibleWindow(240, 0, 800, 720); ctl.setPhonePresent(true);
            wait(1200); ctl.command("mode pip"); wait(300);
            std::cout << "[reshape] --- pass " << pass << (pass == 0 ? " (phone answers)" : " (phone silent)") << std::endl;
            { std::lock_guard<std::mutex> g(rm); CHK(reqs.empty(), "no request outside Split"); }
            ctl.command("split 0.625"); ctl.command("mode split"); wait(300);
            { std::lock_guard<std::mutex> g(rm);
              CHK(reqs.size() == 1, "entering Split sends exactly one UpdateUiConfig request");
              if (!reqs.empty()) CHK(reqs[0].l == 240 && reqs[0].r == 240 && reqs[0].t == 0 && reqs[0].b == 720 - 450, "margins = window offset + the 16:9 pane (800x450)"); }
            ui::Rect pr = ctl.phoneRect();
            CHK(pr.w < 800 && pr.h == 450, "before the phone answers: scaled fallback inside the pane");
            if (pass == 0) {
                ctl.reshapeReply(); wait(300);
                pr = ctl.phoneRect();
                CHK(pr.w == 800 && pr.h == 450 && pr.x == 0 && pr.y == 0, "after the answer: the phone's UI fills the 16:9 pane");
                ui::Rect cr = ctl.cameraRect(); CHK(cr.y >= 450 && cr.y + cr.h <= 720, "camera sits in the strip below");
                ctl.command("split 0.5"); wait(300);
                { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 2 && reqs[1].b == 720 - 360, "changing the split height asks the phone again"); }
                ctl.reshapeReply(); wait(200);
                ctl.command("mode aa"); wait(300);
                { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 3 && reqs[2].b == 0 && reqs[2].t == 0 && reqs[2].l == 240, "leaving Split restores the full window layout"); }
                pr = ctl.phoneRect(); CHK(pr.w == 800 && pr.h == 720, "full-screen AA again fills the window");
                ctl.setPhonePresent(false); ctl.setPhonePresent(true); ctl.command("mode split"); wait(300);
                { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 4, "a new phone session in Split asks again"); }
            } else {
                wait(2300);
                pr = ctl.phoneRect(); CHK(pr.w < 800 && pr.h == 450, "no answer in 2 s: the fallback stays");
                { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 1, "no repeat requests while waiting/after failing"); }
                ctl.reshapeReply(); wait(200); pr = ctl.phoneRect(); CHK(pr.w < 800, "a stray late answer does not flip the layout after the failure");
                ctl.command("mode aa"); wait(300);
                { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == 1, "leaving Split after a failure sends nothing"); }
            }
            ctl.stop(); comp->stop();
        }
        std::cout << (bad ? "[reshape] FAILED" : "[reshape] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "splitdrag": the Split divider is dragged. While dragging the phone pane shows its placeholder (never a stretched picture); release commits the
    // size after 700 ms with ONE layout request and the placeholder stays until the phone answers; releasing in the top / bottom 10% switches to the
    // full-screen camera / full-screen Android Auto.
    if (argc > 4 && std::string(argv[4]) == "splitdrag") {
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[splitdrag] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
        s.prefs_path = "/tmp/aa-prefs-splitdrag.conf"; s.control_fifo = "/tmp/aa-control-splitdrag"; s.status_path = "/tmp/aa-status-splitdrag.json"; s.split_drag = true;
        std::remove("/tmp/aa-prefs-splitdrag.conf");
        auto comp = std::make_shared<Compositor>(s);
        ui::UiController ctl(s, comp);
        struct Req { int l, r, t, b; }; std::vector<Req> reqs; std::mutex rm;
        ctl.setUiConfigSender([&](int l, int r, int t, int b) { std::lock_guard<std::mutex> g(rm); reqs.push_back({l, r, t, b}); });
        comp->start(); ctl.start(); ctl.setTouchGeometry(800, 720);
        ctl.setVisibleWindow(240, 0, 800, 720); ctl.setPhonePresent(true);
        wait(1200); ctl.command("split 0.625"); ctl.command("mode split"); wait(300); ctl.reshapeReply(); wait(300);
        size_t n0; { std::lock_guard<std::mutex> g(rm); n0 = reqs.size(); }
        CHK(n0 == 1 && ctl.layoutPhoneLive() && ctl.phoneRect().h == 450, "start: Split, phone laid out for the 450 px pane, live");
        ui::Rect hb = ctl.buttonRect(ui::ID_SPLIT_HANDLE);
        CHK(!hb.empty() && hb.y < 450 && hb.y + hb.h > 450 && hb.w == 800, "the divider grab band spans the width and straddles the pane boundary");
        ctl.onTouch(400, 450, 0); wait(120);
        CHK(!ctl.layoutPhoneLive(), "finger down on the divider: the phone pane shows its placeholder");
        for (int y = 440; y >= 360; y -= 10) { ctl.onTouch(400, y, 2); wait(30); }
        wait(120);
        CHK(ctl.phoneRect().h == 360 && !ctl.layoutPhoneLive(), "while dragging: the pane follows the finger (360 px), still the placeholder");
        CHK(!ctl.layoutCameraLive(), "while dragging: the camera pane is hidden too (no per-step resize of the live camera)");
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == n0, "nothing is sent to the phone while dragging"); }
        ctl.onTouch(400, 360, 1); wait(300);
        CHK(!ctl.layoutPhoneLive() && std::fabs(ctl.prefs().split_ratio - 0.5) < 0.01, "released: size saved (50%), placeholder stays during the 700 ms settle");
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == n0, "no request yet (700 ms settle)"); }
        wait(700);
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == n0 + 1 && reqs.back().b == 720 - 360, "after the settle: exactly one layout request for the 360 px pane"); }
        CHK(!ctl.layoutPhoneLive(), "still the placeholder until the phone answers");
        ctl.reshapeReply(); wait(300);
        CHK(ctl.layoutPhoneLive() && ctl.phoneRect().h == 360 && ctl.phoneRect().w == 800, "phone answered: live picture back, laid out for 360 px");
        CHK(ctl.layoutCameraLive() && ctl.cameraRect().y == 360, "...and the camera is live again, placed under the 360 px pane");
        // a second grab inside the settle window continues the drag without a request in between
        ctl.onTouch(400, 360, 0); wait(60); ctl.onTouch(400, 400, 2); wait(60); ctl.onTouch(400, 400, 1); wait(200);
        ctl.onTouch(400, 400, 0); wait(60); ctl.onTouch(400, 420, 2); wait(60); ctl.onTouch(400, 420, 1); wait(1000);
        { std::lock_guard<std::mutex> g(rm); CHK(reqs.size() == n0 + 2 && reqs.back().b == 720 - 420, "two grabs inside 700 ms: one request, for the final size (420 px)"); }
        ctl.reshapeReply(); wait(200);
        // edge zones
        ctl.onTouch(400, 420, 0); wait(60); for (int y = 400; y >= 40; y -= 40) { ctl.onTouch(400, y, 2); wait(30); } ctl.onTouch(400, 40, 1); wait(300);
        CHK(ctl.mode() == ui::Mode::FullCam, "released in the top 10%: full-screen camera");
        ctl.command("mode split"); wait(300); ctl.reshapeReply(); wait(200);
        ui::Rect hb2 = ctl.buttonRect(ui::ID_SPLIT_HANDLE); const int hy = hb2.y + hb2.h / 2;
        ctl.onTouch(400, hy, 0); wait(60); for (int y = hy + 40; y <= 690; y += 40) { ctl.onTouch(400, y, 2); wait(30); } ctl.onTouch(400, 690, 1); wait(300);
        CHK(ctl.mode() == ui::Mode::FullAA && ctl.layoutPhoneLive() && ctl.phoneRect().h == 720, "released in the bottom 10%: full-screen Android Auto, live");
        ctl.stop(); comp->stop();
        std::cout << (bad ? "[splitdrag] FAILED" : "[splitdrag] ALL PASSED") << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "precond": the staged battery-preconditioning toggle on the pill. Hidden unless precondition_button=1; a real tap toggles it ON/OFF, the sink hears each user
    // change, the state is never saved and is OFF again after a restart, and the other pill buttons still work with it present.
    if (argc > 4 && std::string(argv[4]) == "precond") {
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[precond] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        auto mk = [&](bool enabled) { Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
            s.prefs_path = "/tmp/aa-prefs-precond.conf"; s.control_fifo = "/tmp/aa-control-precond"; s.status_path = "/tmp/aa-status-precond.json"; s.precondition_button = enabled; return s; };
        std::remove("/tmp/aa-prefs-precond.conf");
        for (int pass = 0; pass < 3; ++pass) {          // 0: feature off   1: on   2: restart with the feature on (state must not survive)
            Config s = mk(pass != 0);
            auto comp = std::make_shared<Compositor>(s);
            ui::UiController ctl(s, comp);
            std::vector<int> sunk; std::mutex sm;
            ctl.setPreconditionSink([&](bool on) { std::lock_guard<std::mutex> l(sm); sunk.push_back(on ? 1 : 0); });
            comp->start(); ctl.start(); ctl.setTouchGeometry(1200, 1244);
            wait(1200); ctl.command("mode pip"); ctl.command("menu open"); wait(300);
            auto tap = [&](ui::Rect r) { const int x = r.x + r.w / 2, y = r.y + r.h / 2; ctl.onTouch(x, y, 0); ctl.onTouch(x, y, 1); wait(250); };
            std::cout << "[precond] --- pass " << pass << (pass == 0 ? " (feature off)" : pass == 1 ? " (feature on)" : " (restart, feature on)") << std::endl;
            const ui::Rect pb = ctl.buttonRect(ui::ID_PRECOND), gear = ctl.buttonRect(ui::ID_SETTINGS), pip = ctl.buttonRect(ui::ID_MODE_PIP), aa = ctl.buttonRect(ui::ID_MODE_AA);
            CHK(gear.w > 0 && pip.w > 0 && aa.w > 0, "the normal pill buttons are present");
            if (pass == 0) { CHK(pb.w == 0, "no preconditioning button when the feature is off (default)"); }
            else {
                CHK(pb.w > 0, "preconditioning button present when enabled");
                CHK(pb.w > 0 && pb.y > pip.y && pb.y < gear.y, "it sits between the layout buttons and the gear");
                CHK(!pb.intersects(gear) && !pb.intersects(pip), "it does not overlap its neighbours");
                CHK(pb.x + pb.w <= 1042 + 2 && gear.y + gear.h <= 720, "every pill button is inside the window/strip");
                CHK(!ctl.preconditionOn(), "starts OFF" );
                tap(pb); CHK(ctl.preconditionOn(), "tap -> ON");
                tap(ctl.buttonRect(ui::ID_PRECOND)); CHK(!ctl.preconditionOn(), "tap again -> OFF");
                tap(ctl.buttonRect(ui::ID_PRECOND)); CHK(ctl.preconditionOn(), "tap -> ON again");
                { std::lock_guard<std::mutex> l(sm); CHK(sunk.size() == 3 && sunk[0] == 1 && sunk[1] == 0 && sunk[2] == 1, "the sink heard on, off, on (one call per user change)"); }
                tap(ctl.buttonRect(ui::ID_MODE_AA)); CHK(ctl.mode() == ui::Mode::FullAA, "other pill buttons still work with it ON");
                CHK(ctl.preconditionOn(), "changing the layout does not change the preconditioning state");
                ctl.setPreconditionState(false); CHK(!ctl.preconditionOn(), "state can be set from outside (vehicle/Maps reported)");
                ctl.setPreconditionState(true);
                wait(600);
                { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("precond") == std::string::npos, "the state is never written to the prefs file"); }
            }
            wait(300); ctl.stop(); comp->stop();
        }
        std::cout << (bad ? "[precond] FAILED" : "[precond] ALL PASSED") << " (" << bad << " failures)" << std::endl; return bad ? 1 : 0;
    }
    // 4th arg "pipdrag": truck 29 bug. NO phone connected: the Android Auto placeholder covers the window (opaque art) with a hole cut where the live camera PiP is.
    // While the PiP is dragged the hole must follow it, otherwise the art covers the camera and the "frame" stays behind. Also: free placement (no corner snap),
    // and the PiP can never be made smaller than the size it starts at.
    if (argc > 4 && std::string(argv[4]) == "pipdrag") {
        Config s; s.phone_test = false; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
        s.prefs_path = "/tmp/aa-prefs-pipdrag.conf"; s.control_fifo = "/tmp/aa-control-pipdrag"; s.status_path = "/tmp/aa-status-pipdrag.json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s);
        ui::UiController ctl(s, comp);
        comp->start(); ctl.start(); ctl.setTouchGeometry(1200, 1244);
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[pipdrag] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        auto fr = [&](int act, int idx, std::vector<TouchPointer> p) { TouchFrame f; f.action = act; f.index = idx; f.p = std::move(p); ctl.onTouchFrame(f); };
        wait(1500); ctl.command("size 0.30"); ctl.command("corner br"); ctl.command("mode pip"); wait(400);       // these tests were written for a small PiP bottom-right (the defaults are now large, top-right)
        CHK(!ctl.layoutPhoneLive() && ctl.layoutCameraLive(), "setup: phone is a placeholder, camera is live");
        ui::Rect r0 = ctl.cameraRect(); const int sx = r0.x + r0.w / 2, sy = r0.y + r0.h / 2;
        CHK(ctl.overlayAlphaAt(sx, sy) == 0, "before the drag: the camera sits in a hole of the placeholder art");
        fr(0, 0, {{0, sx, sy}});
        const int tx = 200, ty = 220;                                             // somewhere in the upper left, NOT a corner
        for (int k = 1; k <= 10; ++k) { fr(2, 0, {{0, sx + (tx - sx) * k / 10, sy + (ty - sy) * k / 10}}); wait(40); }
        wait(300);                                                                 // the overlay may lag a little behind the finger, not forever
        ui::Rect mid = ctl.cameraRect();
        CHK(mid.x < r0.x - 100, "setup: the camera followed the finger");
        CHK(ctl.overlayAlphaAt(mid.x + mid.w / 2, mid.y + mid.h / 2) == 0, "DURING the drag the hole moved with the camera (art does not cover it)");
        if (comp->baseArt()) CHK(ctl.overlayAlphaAt(10, 10) == 0 && ctl.overlayAlphaAt(r0.x + r0.w / 2, r0.y + r0.h / 2) == 0, "...the splash is video underneath: no art on the overlay, so no hole that could trail the camera");
        else CHK(ctl.overlayAlphaAt(r0.x + r0.w / 2, r0.y + r0.h / 2) != 0 || (mid.x + mid.w > r0.x + r0.w / 2), "...and the old position is covered by the art again");
        if (s.pip_corners) CHK(ctl.overlayAlphaAt(mid.x + 1, mid.y + 1) == 0 && ctl.overlayAlphaAt(mid.x + mid.w - 2, mid.y + mid.h - 2) == 0, "DURING the drag there are no rounded-corner covers on the overlay (they would trail behind the video)");
        fr(1, 0, {{0, tx, ty}}); wait(500);
        ui::Rect fin = ctl.cameraRect();
        if (s.pip_corners) CHK(ctl.overlayAlphaAt(fin.x + 1, fin.y + 1) > 0 && ctl.overlayAlphaAt(fin.x + fin.w - 2, fin.y + fin.h - 2) > 0, "after the drop the rounded-corner covers are drawn at the new place");
        CHK(std::abs(fin.x - mid.x) <= 4 && std::abs(fin.y - mid.y) <= 4, "dropping it leaves it where it was dropped (no corner snap)");
        CHK(fin.x > 40 && fin.y > 40, "it is not sitting in a corner");
        CHK(ctl.overlayAlphaAt(fin.x + fin.w / 2, fin.y + fin.h / 2) == 0, "after the drop the hole is where the camera is");
        { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("pip_px=") != std::string::npos && all.find("pip_py=") != std::string::npos, "the dropped position is saved"); }
        // minimum size: the start size is the floor (pinch in cannot go below it)
        const double start_frac = ctl.prefs().pip_frac;
        CHK(start_frac >= 0.299 && start_frac <= 0.301, "the PiP starts at 30% of the window width");
        { ui::Rect cr = ctl.cameraRect(); const int cx = cr.x + cr.w / 2, cy = cr.y + cr.h / 2;
          fr(0, 0, {{0, cx - 30, cy}}); fr(5, 1, {{0, cx - 30, cy}, {1, cx + 30, cy}});
          for (int k = 1; k <= 6; ++k) fr(2, 0, {{0, cx - 30 + 4 * k, cy}, {1, cx + 30 - 4 * k, cy}});         // fingers 60 px -> 12 px apart: a big shrink
          fr(6, 1, {{0, cx - 6, cy}, {1, cx + 6, cy}}); fr(1, 0, {{0, cx - 6, cy}}); wait(300); }
        CHK(ctl.prefs().pip_frac >= 0.299, "pinching in cannot make the PiP smaller than its start size");
        ctl.command("size 0.1"); wait(200);
        CHK(ctl.prefs().pip_frac >= 0.299, "even a command asking for 10% is raised to the start size");
        // the Settings corner buttons are shortcuts to a position
        ctl.command("corner tl"); wait(300);
        { ui::Rect cr = ctl.cameraRect(); CHK(cr.x <= 20 && cr.y <= 20, "'corner tl' puts it in the top-left corner"); }
        ctl.stop(); comp->stop();
        std::cout << (bad ? "[pipdrag] FAILED" : "[pipdrag] ALL PASSED") << " (" << bad << " failures)" << std::endl; return bad ? 1 : 0;
    }
    // 4th arg: nophone | jpegcam
    // 4th arg "gest": multi-finger routing. PiP pinch resizes the PiP (nothing reaches the phone); two fingers on the phone pass through intact.
    if (argc > 4 && std::string(argv[4]) == "gest") {
        Config s; s.phone_test = true; s.camera_test = true; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 15; s.video_bitrate = 1000000;
        s.prefs_path = "/tmp/aa-prefs-gest.conf"; s.control_fifo = "/tmp/aa-control-gest"; s.status_path = "/tmp/aa-status-gest.json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s);
        ui::UiController ctl(s, comp);
        std::vector<TouchFrame> fwd; std::mutex fm;
        ctl.setPhoneTouchForwarder([&](const TouchFrame& f) { std::lock_guard<std::mutex> l(fm); fwd.push_back(f); });
        comp->start(); ctl.start(); ctl.setTouchGeometry(1200, 1244);             // window 694x720, touch space = window pixels
        int bad = 0; auto CHK = [&](bool ok, const char* what) { std::cout << "[gest] " << (ok ? "ok   " : "FAIL ") << what << std::endl; if (!ok) ++bad; };
        auto wait = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        auto fr = [&](int act, int idx, std::vector<TouchPointer> p) { TouchFrame f; f.action = act; f.index = idx; f.p = std::move(p); ctl.onTouchFrame(f); };
        wait(1500); ctl.command("size 0.30"); ctl.command("corner br"); ctl.command("mode pip"); wait(300);
        // REAL-USE case (truck 28): the pill is open (menu state Home) when the pinch starts, e.g. right after choosing PiP in the pill, and every touch
        // restarts its timer so it never closes while the user keeps touching. The pinch must work then too.
        ctl.command("menu open"); wait(200); fwd.clear();
        fr(0, 0, {{0, 520, 600}}); fr(5, 1, {{0, 520, 600}, {1, 600, 650}});
        for (int k = 1; k <= 6; ++k) fr(2, 0, {{0, 520 - 8 * k, 600 - 6 * k}, {1, 600 + 8 * k, 650 + 6 * k}});
        fr(6, 1, {{0, 472, 564}, {1, 648, 686}}); fr(1, 0, {{0, 472, 564}}); wait(300);
        CHK(ctl.prefs().pip_frac > 0.5 && ctl.prefs().pip_frac < 0.7, "PiP pinch works while the pill is open (menu state Home)");
        ctl.command("size 0.3"); wait(300);
        // pinch on the PiP (bottom-right, ~x470..678 y548..704 in window pixels): fingers 94 px apart -> 188 px apart = x2
        fwd.clear();
        fr(0, 0, {{0, 520, 600}});
        fr(5, 1, {{0, 520, 600}, {1, 600, 650}});
        for (int k = 1; k <= 6; ++k) fr(2, 0, {{0, 520 - 8 * k, 600 - 6 * k}, {1, 600 + 8 * k, 650 + 6 * k}});
        fr(6, 1, {{0, 472, 564}, {1, 648, 686}});
        fr(1, 0, {{0, 472, 564}});
        wait(300);
        CHK(ctl.mode() == ui::Mode::Pip, "pinch on PiP did not change the mode");
        CHK(ctl.prefs().pip_frac > 0.5 && ctl.prefs().pip_frac < 0.7, "PiP width roughly doubled (0.30 -> ~0.6)");
        { std::lock_guard<std::mutex> l(fm); CHK(fwd.empty(), "nothing from the PiP pinch reached the phone"); }
        { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("pip_frac=") != std::string::npos, "pip_frac saved to the prefs file"); }
        // dragging the PiP: it follows the finger, snaps to the nearest corner on release (saved), and nothing reaches the phone
        { std::lock_guard<std::mutex> l(fm); fwd.clear(); }
        { ui::Rect cr = ctl.cameraRect(); const int cx = cr.x + cr.w / 2, cy = cr.y + cr.h / 2; const int tx = 150, ty = 150;   // finger path to the top-left area
          fr(0, 0, {{0, cx, cy}});
          for (int k = 1; k <= 8; ++k) fr(2, 0, {{0, cx + (tx - cx) * k / 8, cy + (ty - cy) * k / 8}});
          wait(250);
          ui::Rect mid = ctl.cameraRect(); CHK(mid.x < cr.x - 100 && mid.y < cr.y - 100, "PiP follows the finger while dragging");
          fr(1, 0, {{0, tx, ty}}); wait(300);
          CHK(ctl.mode() == ui::Mode::Pip, "dragging the PiP does not change the layout mode");
          { ui::Rect fin0 = ctl.cameraRect(); CHK(std::abs(fin0.x - mid.x) <= 4 && std::abs(fin0.y - mid.y) <= 4, "PiP stays where it was dropped (no corner snap)"); }
          ui::Rect fin = ctl.cameraRect(); CHK(fin.x >= 16 && fin.y >= 16, "PiP is inside the window margin after the drag");
          { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("pip_px=") != std::string::npos, "dropped position saved"); }
          { std::lock_guard<std::mutex> l(fm); CHK(fwd.empty(), "PiP drag never reached the phone"); }
          // a small wobble (< 14 px) is a tap: nothing moves
          ui::Rect before = ctl.cameraRect(); const int bx = before.x + before.w / 2, by = before.y + before.h / 2;
          fr(0, 0, {{0, bx, by}}); fr(2, 0, {{0, bx + 6, by + 5}}); fr(1, 0, {{0, bx + 6, by + 5}}); wait(200);
          ui::Rect after = ctl.cameraRect(); CHK(after.x == before.x && after.y == before.y, "a 6 px wobble does not move the PiP"); }
        // a plain tap on the PiP BODY does nothing (no maximize) and never reaches the phone; the window buttons do the work
        { std::lock_guard<std::mutex> l(fm); fwd.clear(); }
        { ui::Rect cr = ctl.cameraRect(); int tx = cr.x + cr.w / 2, ty = cr.y + cr.h / 2;
          fr(0, 0, {{0, tx, ty}}); fr(1, 0, {{0, tx, ty}}); wait(200);
          CHK(ctl.mode() == ui::Mode::Pip, "tap on the PiP body does NOT maximize");
          std::lock_guard<std::mutex> l(fm); CHK(fwd.empty(), "tap on the PiP body is swallowed (not sent to the phone)"); }
        { ui::Rect cr = ctl.cameraRect(); wait(4600);                              // idle: the window buttons are gone, only a touch brings them back
          CHK(ctl.buttonRect(ui::ID_PANE_MAX).w == 0 && ctl.buttonRect(ui::ID_PANE_CLOSE).w == 0, "PiP window buttons disappear a few seconds after the last touch");
          std::lock_guard<std::mutex> l0(fm); fwd.clear(); }
        { ui::Rect cr = ctl.cameraRect(); fr(0, 0, {{0, cr.x + cr.w / 2, cr.y + cr.h / 2}}); fr(1, 0, {{0, cr.x + cr.w / 2, cr.y + cr.h / 2}}); wait(300);
          CHK(ctl.buttonRect(ui::ID_PANE_MAX).w > 0, "a touch on the PiP brings the window buttons back");
          CHK(ctl.mode() == ui::Mode::Pip, "...and that touch did not maximize"); }
        { ui::Rect mb = ctl.buttonRect(ui::ID_PANE_MAX); CHK(mb.w > 0, "PiP has a Max button");
          int tx = mb.x + mb.w / 2, ty = mb.y + mb.h / 2; fr(0, 0, {{0, tx, ty}}); fr(1, 0, {{0, tx, ty}}); wait(300);
          CHK(ctl.mode() == ui::Mode::FullCam, "Max button -> full-screen camera"); }
        { // full-screen camera: the pill is persistent, the bar is there, a two-finger pinch zooms (saved), drag pans, Reset restores
          CHK(ctl.buttonRect(ui::ID_MODE_AA).w > 0 && ctl.buttonRect(ui::ID_CAM_FIT).w > 0, "pill + camera bar present in full-screen camera");
          ui::Rect cr = ctl.cameraRect(); int cx = cr.x + cr.w / 2, cy = cr.y + cr.h / 2;
          { std::lock_guard<std::mutex> l(fm); fwd.clear(); }
          fr(0, 0, {{0, cx - 20, cy}}); fr(5, 1, {{0, cx - 20, cy}, {1, cx + 20, cy}});
          for (int k = 1; k <= 6; ++k) fr(2, 0, {{0, cx - 20 - 10 * k, cy}, {1, cx + 20 + 10 * k, cy}});
          fr(6, 1, {{0, cx - 80, cy}, {1, cx + 80, cy}}); fr(1, 0, {{0, cx - 80, cy}}); wait(300);
          CHK(ctl.prefs().cam_zoom > 2.5 && ctl.prefs().cam_zoom <= 4.0, "pinch in full-screen camera zooms (40px -> 160px apart = ~4x)");
          { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("cam_zoom=") != std::string::npos, "zoom saved to the prefs file"); }
          { std::lock_guard<std::mutex> l(fm); CHK(fwd.empty(), "camera pinch never reaches the phone"); }
          double cx0 = ctl.prefs().cam_cx; fr(0, 0, {{0, cx, cy}}); for (int k = 1; k <= 5; ++k) fr(2, 0, {{0, cx - 20 * k, cy}}); fr(1, 0, {{0, cx - 100, cy}}); wait(300);
          CHK(ctl.prefs().cam_cx > cx0 + 0.02, "dragging left pans the zoomed view to the right");
          ui::Rect rb = ctl.buttonRect(ui::ID_CAM_RESET); fr(0, 0, {{0, rb.x + 5, rb.y + 5}}); fr(1, 0, {{0, rb.x + 5, rb.y + 5}}); wait(300);
          CHK(ctl.prefs().cam_zoom == 1.0, "Reset returns to 1x");
          ui::Rect mb = ctl.buttonRect(ui::ID_CAM_MIRROR); fr(0, 0, {{0, mb.x + 5, mb.y + 5}}); fr(1, 0, {{0, mb.x + 5, mb.y + 5}}); wait(300);
          CHK(ctl.prefs().cam_mirror, "Mirror button toggles and applies"); }
        ctl.command("mode pip"); wait(200);
        { ui::Rect cb = ctl.buttonRect(ui::ID_PANE_CLOSE); CHK(cb.w > 0, "PiP has a Close button");
          fr(0, 0, {{0, cb.x + cb.w / 2, cb.y + cb.h / 2}}); fr(1, 0, {{0, cb.x + cb.w / 2, cb.y + cb.h / 2}}); wait(300);
          std::cout << "[gest] after Close: mode=" << (int)ctl.mode() << " btn " << cb.x << "," << cb.y << " " << cb.w << "x" << cb.h << " screen=" << (int)ctl.screen() << std::endl;
          CHK(ctl.mode() == ui::Mode::FullAA, "Close button -> full-screen Android Auto"); }
        ctl.command("mode pip"); wait(200);
        ctl.command("mode aa"); wait(4600);
        CHK(ctl.buttonRect(ui::ID_MODE_AA).w == 0, "idle over live AA: no pill, no tab");
        { std::lock_guard<std::mutex> l(fm); fwd.clear(); }
        fr(0, 0, {{0, 100, 100}}); fr(1, 0, {{0, 100, 100}}); wait(300);
        { std::lock_guard<std::mutex> l(fm); CHK(fwd.size() == 2, "a tap on the phone still reaches the phone"); }
        CHK(ctl.buttonRect(ui::ID_MODE_AA).w > 0 && ctl.buttonRect(ui::ID_SETTINGS).w > 0, "...and brought the pill up");
        wait(4600); CHK(ctl.buttonRect(ui::ID_MODE_AA).w == 0, "pill disappears again after the last touch");
        ctl.command("mode pip"); ctl.command("corner br"); wait(200);                      // the drag test left the PiP top-left, exactly where this gesture lands
        // two fingers on the phone area (top-left): the whole gesture must be forwarded, all fingers, in phone coordinates
        { std::lock_guard<std::mutex> l(fm); fwd.clear(); }
        fr(0, 0, {{0, 100, 100}});
        fr(5, 1, {{0, 100, 100}, {1, 200, 150}});
        fr(2, 0, {{0, 90, 95}, {1, 230, 160}});
        fr(6, 1, {{0, 90, 95}, {1, 230, 160}});
        fr(1, 0, {{0, 90, 95}});
        wait(200);
        std::lock_guard<std::mutex> l(fm);
        CHK(fwd.size() == 5, "all 5 events of the two-finger phone gesture were forwarded");
        if (fwd.size() == 5) {
            CHK(fwd[1].action == 5 && fwd[1].index == 1 && fwd[1].p.size() == 2 && fwd[1].p[1].id == 1, "second finger arrives as pointer-down with its id");
            CHK(fwd[2].p.size() == 2 && fwd[2].p[0].x == 90 && fwd[2].p[1].x == 230, "move carries both fingers; phone UI area == window so pixels map 1:1");
            std::cout << "[gest] phone coords of the second finger: " << fwd[1].p[1].x << "," << fwd[1].p[1].y << std::endl;
        }
        {   // Debug overrides: gear override drives the reverse-camera rule; driving/night overrides drive what the phone is told; truck readings flow through
            std::atomic<int> sd{-1}, sn{-1}; ctl.setPhoneSensorSink([&](int d, bool n) { sd = d; sn = n ? 1 : 0; });
            wait(100); CHK(sd == 0 && sn == 0, "sensor sink starts at parked/daytime (the behaviour so far)");
            ctl.command("mode aa"); ctl.command("autocam 0"); ctl.command("reverse 0"); wait(200);
            ctl.command("dbg gear 2"); wait(300); CHK(ctl.mode() == ui::Mode::FullAA, "gear override Reverse alone does nothing while 'camera in reverse' is off");
            ctl.command("dbg gear 0"); ctl.command("autocam 1"); ctl.command("dbg gear 2"); wait(300);
            CHK(ctl.mode() == ui::Mode::FullCam, "gear override Reverse switches to the camera (auto-camera on)");
            ctl.command("dbg gear 3"); wait(300); CHK(ctl.mode() == ui::Mode::FullAA, "gear override Drive returns to the previous layout");
            ctl.command("dbg gear 0"); ctl.command("reverse 1"); wait(300); CHK(ctl.mode() == ui::Mode::FullCam, "truck reverse (gear = Truck) switches to the camera");
            ctl.command("reverse 0"); wait(300); CHK(ctl.mode() == ui::Mode::FullAA, "truck out of reverse returns");
            ctl.command("dbg driving 2"); wait(200); CHK(sd == 26, "driving override Moving -> phone told 26");
            ctl.command("dbg driving 0"); ctl.command("truck 6 2"); wait(200); CHK(sd == 2, "driving = Truck follows the truck's own status mask");
            ctl.command("dbg driving 1"); wait(200); CHK(sd == 0, "driving override Parked -> phone told 0");
            ctl.command("dbg night 2"); wait(200); CHK(sn == 1, "night override Night -> phone told night");
            ctl.command("dbg night 0"); ctl.command("truck 7 1"); wait(200); CHK(sn == 1, "night = Truck follows the truck");
            ctl.command("truck 7 0"); wait(200); CHK(sn == 0, "truck says day -> phone told day");
            ctl.command("dbg night 1"); wait(200); CHK(sn == 0, "night override Day -> phone told day");
            { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("dbg_driving=1") != std::string::npos && all.find("dbg_night=1") != std::string::npos, "Debug overrides persist in the prefs file"); }
        }
        // prefs are written asynchronously (never on the touch path): a change made right before shutdown must still reach the file
        ctl.command("size 0.41"); ctl.stop();
        { std::ifstream pf(s.prefs_path); std::string all((std::istreambuf_iterator<char>(pf)), {}); CHK(all.find("pip_frac=0.41") != std::string::npos, "a change made just before shutdown is flushed to the prefs file"); }
        std::cout << (bad ? "[gest] FAILED" : "[gest] ALL PASSED") << std::endl;
        return bad ? 1 : 0;
    }
    // 4th arg "static": exactly the truck-test-6 situation (no phone, no camera, 30 fps, 720p): print per-frame NAL types
    if (argc > 4 && std::string(argv[4]) == "static") {
        Config s; s.calib = (argc > 5 && std::string(argv[5]) == "calib"); s.phone_test = false; s.camera_test = false; s.camera_rtsp = ""; s.video_width = 1280; s.video_height = 720; s.video_fps = 30; s.video_bitrate = 4000000;
        if (const char* ml = getenv("AA_MIX_LATENCY")) s.mix_latency_ms = atoi(ml);
        // bench overrides: replay the truck configuration (AA_W/AA_H, AA_BITRATE, AA_CAMTEST=1 synthetic camera, AA_CAMW/H/FPS)
        if (const char* v = getenv("AA_W")) s.video_width = atoi(v); if (const char* v = getenv("AA_H")) s.video_height = atoi(v);
        if (const char* v = getenv("AA_BITRATE")) s.video_bitrate = atoi(v); if (const char* v = getenv("AA_CAMTEST")) s.camera_test = atoi(v) != 0;
        if (const char* v = getenv("AA_CAMW")) s.camera_width = atoi(v); if (const char* v = getenv("AA_CAMH")) s.camera_height = atoi(v); if (const char* v = getenv("AA_CAMFPS")) s.camera_fps = atoi(v);
        if (const char* v = getenv("AA_UIBLEND")) s.ui_blend = atoi(v);
        if (const char* v = getenv("AA_MIXER_ALPHA")) s.mixer_alpha = atoi(v); if (const char* v = getenv("AA_ENC_QUEUE")) s.enc_queue = atoi(v); if (const char* v = getenv("AA_PHONE_QUEUE")) s.phone_queue = atoi(v);
        s.prefs_path = "/tmp/aa-prefs-test.conf"; s.control_fifo = "/tmp/aa-control-test"; s.status_path = "/tmp/aa-status-test.json"; std::remove(s.prefs_path.c_str());
        auto comp = std::make_shared<Compositor>(s);
        std::mutex m; int n = 0, idr = 0, spsn = 0; std::string line; std::ofstream sout("/tmp/static.h264", std::ios::binary);
        comp->setOutputCallback([&](const uint8_t* d, size_t sz, uint64_t) {
            std::lock_guard<std::mutex> l(m); ++n; sout.write((const char*)d, sz); bool hasI = false, hasS = false;
            for (size_t i = 0; i + 4 < sz; ++i) if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) { int t = d[i+3] & 31; if (t == 5) hasI = true; if (t == 7) hasS = true; }
            if (hasI) ++idr; if (hasS) ++spsn;
            if (g_feed_start_ms.load() > 0 && g_frames_at_start.load() < 0) { g_frames_at_start = n; std::cout << "[static] output frame count when the phone session started: " << n << std::endl; }
            static size_t big = 0; if (sz > 1500) ++big; if (n % 100 == 0) std::cout << "[static] n=" << n << " cumulative frames >1500B: " << big << std::endl;
            if (n == 1 || n == 2) { std::cout << "[static] first bytes:"; for (size_t i = 0; i < 16 && i < sz; ++i) printf(" %02x", d[i]); std::cout << std::endl; }
            if (n <= 3 || hasI || hasS) std::cout << "[static] frame " << n << " size " << sz << (hasS ? " SPS/PPS" : "") << (hasI ? " IDR" : "") << std::endl;
        });
        ui::UiController ctl(s, comp); comp->start(); ctl.start();
        ctl.setTouchGeometry(1200, 1244);
        if (argc > 7) ctl.command(std::string("mode ") + argv[7]);     // argv[7] = aa|cam|pip|split: start in that layout
        // argv[6] = "feed:/path/file.h264": push a REAL Annex-B H.264 stream (AUD-delimited access units) through pushPhoneFrame, with a
        // phone-style absolute timestamp, like HuSession does. Reproduces the truck-12 bug (phone frames never reached the output).
        std::thread feeder;
        if (argc > 6 && std::string(argv[6]).rfind("feed:", 0) == 0) {
            std::ifstream f(std::string(argv[6]).substr(5), std::ios::binary); std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), {});
            std::vector<size_t> st; for (size_t i = 0; i + 5 < all.size(); ++i) if (all[i]==0&&all[i+1]==0&&all[i+2]==0&&all[i+3]==1&&(all[i+4]&31)==9) st.push_back(i);
            std::cout << "[feed] " << all.size() << " bytes, " << st.size() << " access units" << std::endl;
            s.phone_test = false;
            feeder = std::thread([&, all, st] {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                g_feed_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                ctl.setPhonePresent(true);
                const uint64_t base = 987654321000000ull;      // phone clock: arbitrary huge absolute microseconds
                for (size_t k = 0; k < st.size(); ++k) {
                    size_t e = (k + 1 < st.size()) ? st[k+1] : all.size();
                    comp->pushPhoneFrame(all.data() + st[k], e - st[k], base + k * 33333ull);
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (s.calib) { auto tp = [&](int x, int y) { ctl.onTouch(x, y, 0); std::this_thread::sleep_for(std::chrono::milliseconds(150)); ctl.onTouch(x, y, 1); std::this_thread::sleep_for(std::chrono::milliseconds(700)); }; tp(300, 400); tp(650, 700); tp(100, 60); }
        std::this_thread::sleep_for(std::chrono::seconds(argc > 3 ? atoi(argv[3]) : 6));
        if (feeder.joinable()) feeder.join();
        { std::lock_guard<std::mutex> l(m); std::cout << "[static] total frames " << n << ", IDR " << idr << ", with SPS " << spsn << std::endl; }
        return 0;
    }
    Config c; c.phone_test = !(argc > 4 && std::string(argv[4]) == "nophone"); c.camera_test = true; c.camera_rtsp = "";
    c.video_width = argc > 1 ? atoi(argv[1]) : 640; c.video_height = argc > 2 ? atoi(argv[2]) : 360; c.video_fps = 15; c.video_bitrate = 1000000;
    c.prefs_path = "/tmp/aa-prefs-test.conf"; c.control_fifo = "/tmp/aa-control-test"; c.status_path = "/tmp/aa-status-test.json";
    GstElement* sender = nullptr;
    if (argc > 4 && std::string(argv[4]) == "jpegcam") {      // simulate the ESP32-CAM: JPEG over RTP on udp:5600
        c.camera_test = false; c.camera_udp_port = 5600; c.camera_width = 640; c.camera_height = 480; c.camera_codec = "mjpeg";
        sender = gst_parse_launch("videotestsrc pattern=smpte is-live=true ! video/x-raw,format=Y42B,width=640,height=480,framerate=10/1 ! jpegenc quality=70 ! rtpjpegpay ! udpsink host=127.0.0.1 port=5600", nullptr);
        if (!sender) std::cerr << "[test] SENDER PIPELINE FAILED TO PARSE" << std::endl;
        else {
            GstStateChangeReturn r = gst_element_set_state(sender, GST_STATE_PLAYING);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            GstMessage* m;
            std::cout << "[test] sender state change=" << r << std::endl;
            while ((m = gst_bus_pop_filtered(GST_ELEMENT_BUS(sender), (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)))) {
                GError* e = nullptr; gchar* d = nullptr;
                if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) gst_message_parse_error(m, &e, &d); else gst_message_parse_warning(m, &e, &d);
                std::cout << "[test] SENDER " << (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR ? "ERROR" : "WARN") << " from " << GST_OBJECT_NAME(m->src) << ": " << (e ? e->message : "?") << " | " << (d ? d : "") << std::endl;
            }
        }
    }
    std::remove(c.prefs_path.c_str());   // start from default prefs
    auto comp = std::make_shared<Compositor>(c);
    std::ofstream out("/tmp/layout-all.h264", std::ios::binary); std::mutex om; size_t frames = 0, bytes = 0;
    comp->setOutputCallback([&](const uint8_t* d, size_t n, uint64_t) { std::lock_guard<std::mutex> l(om); ++frames; bytes += n; out.write((const char*)d, n); });
    ui::UiController ctl(c, comp);
    comp->start(); ctl.start();
    // Scripted session, driven through the real touch path (UiController::onTouch), logging frame index at each step.
    auto mark = [&](const char* what) { std::lock_guard<std::mutex> l(om); std::cout << "[test] STEP " << what << " at frame " << frames << std::endl; };
    int secs = argc > 3 ? atoi(argv[3]) : 3;
    auto wait = [&](double s) { std::this_thread::sleep_for(std::chrono::milliseconds((int)(s * 1000))); };
    auto tap = [&](int x, int y) { ctl.onTouch(x, y, 0); wait(0.15); ctl.onTouch(x, y, 1); wait(0.4); };
    const bool truck = (argc > 5 && std::string(argv[5]) == "truck");     // simulate Sync 4: 1200x1244 touch space -> centred visible window
    if (truck) ctl.setTouchGeometry(1200, 1244);
    const int H = c.video_height;
    const int W = truck ? ((int)((long long)H * 1200 / 1244) & ~1) : c.video_width;                  // touch positions are in WINDOW pixels
    ctl.command("mode aa"); wait(secs);          mark("aa-with-handle");
    tap(W - 20, H / 2); wait(secs);              mark("home-open");               // tap the edge handle
    tap(W / 2 - 20, H / 2 - 60 + 0);             // dead centre-ish of the button row (hit test decides)
    wait(secs);                                   mark("after-center-tap");
    ctl.command("mode pip"); ctl.command("menu open"); wait(secs); mark("pip-with-home");
    ctl.command("menu close"); ctl.command("mode cam"); wait(1.0); mark("cam");
    tap(W - 20, H / 2); wait(1.0);               // open home over camera
    // tap Settings (computed by the menu itself: card centred, bottom row)
    tap(W / 2 - 200, H / 2 + 120); wait(secs);   mark("settings-open");
    ctl.command("menu close"); ctl.command("mode pip"); wait(1.0);
    ctl.command("menu open"); wait(0.5); tap(W / 2 - 200, H / 2 + 120);   // settings again
    wait(0.5);
    // Safe-zone: Edit... button position is deterministic at 640x360? use command path for the editor instead:
    ctl.command("roi 0.2 0.1 0.6 0.8"); ctl.command("menu close"); ctl.command("mode pip"); wait(secs); mark("pip-with-roi");
    ctl.command("moving 1"); ctl.command("menu open"); wait(1.0); mark("home-moving");
    ctl.command("moving 0"); ctl.command("menu close"); ctl.command("mode split"); wait(secs); mark("split");
    { std::lock_guard<std::mutex> l(om); out.flush(); std::cout << "[test] total frames " << frames << " bytes " << bytes << std::endl; }
    ctl.stop(); comp->stop(); if (sender) gst_element_set_state(sender, GST_STATE_NULL);
    return frames > 0 ? 0 : 1;
}
