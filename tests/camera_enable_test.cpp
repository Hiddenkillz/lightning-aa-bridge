// camera_enable=0: Config clears every camera source, and the UI never leaves Android Auto / Energy for a camera-bearing mode.
// Built by CMake as camera-enable-test (needs GStreamer only because UiController takes a Compositor; the pipeline is never started).
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <set>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <gst/gst.h>
#include "../src/Config.h"
#include "../src/compositor/Compositor.h"
#include "../src/ui/UiController.h"
using namespace ui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
// How many times the controller re-rendered its overlay in `ms` (each render rewrites the status file).
static int renders(UiController& ui, const std::string& status, int ms) {
    std::set<long long> seen; const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        struct stat st; if (::stat(status.c_str(), &st) == 0) seen.insert((long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    (void)ui; return (int)seen.size();
}
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    const std::string cf = "/tmp/camera-enable-test.conf", pf = "/tmp/camera-enable-test-prefs.conf";

    // Config: default is on; =0 clears every camera source (so the compositor builds no camera branch and never dials the default RTSP address)
    put(cf, "camera_rtsp=rtsp://10.0.0.9/x\ncamera_device=/dev/video0\ncamera_udp_port=5000\ncamera_test=1\n");
    { Config c = Config::load(cf); CHECK(c.camera_enable && c.camera_device == "/dev/video0" && c.camera_udp_port == 5000 && c.camera_test); }
    put(cf, "camera_rtsp=rtsp://10.0.0.9/x\ncamera_device=/dev/video0\ncamera_udp_port=5000\ncamera_test=1\ncamera_enable=0\n");
    Config off = Config::load(cf);
    CHECK(!off.camera_enable && off.camera_device.empty() && off.camera_rtsp.empty() && off.camera_udp_port == 0 && !off.camera_test);
    { Config d; CHECK(d.camera_enable && !d.camera_rtsp.empty()); }     // the default struct is unchanged (existing installs keep working)

    // UI, camera disabled: a prefs file that starts in a camera mode comes up in Android Auto
    for (const char* sm : {"pip", "split", "cam"}) {
        put(pf, std::string("start_mode=") + sm + "\n");
        Config c = off; c.prefs_path = pf;
        auto ui = std::make_shared<UiController>(c, std::make_shared<Compositor>(c));
        CHECK(ui->mode() == Mode::FullAA);
        for (const char* m : {"cam", "split", "pip"}) { ui->command(std::string("mode ") + m); CHECK(ui->mode() == Mode::FullAA); }
        ui->command("mode energy"); CHECK(ui->mode() == Mode::Energy);          // non-camera modes still work
        ui->setMode(Mode::Split); CHECK(ui->mode() == Mode::FullAA);
        ui->setMode(Mode::FullCam); CHECK(ui->mode() == Mode::FullAA);
    }
    put(pf, "start_mode=aa\nstart_last=1\nlast_mode=split\n");          // "start on Last" with a camera mode remembered from before the camera was removed
    { Config c = off; c.prefs_path = pf; UiController ui(c, std::make_shared<Compositor>(c)); CHECK(ui.mode() == Mode::FullAA); }

    // No camera but OBD on: window and split exist (they show the Energy panel), the full-screen "camera" is the Energy page
    put(pf, "start_mode=pip\n");
    { Config c = off; c.obd_enable = true; c.prefs_path = pf; UiController ui(c, std::make_shared<Compositor>(c));
      CHECK(ui.mode() == Mode::Pip);
      ui.command("mode split"); CHECK(ui.mode() == Mode::Split);
      ui.command("mode cam"); CHECK(ui.mode() == Mode::Energy);
      ui.setMode(Mode::FullCam); CHECK(ui.mode() == Mode::Energy);
      ui.setMode(Mode::Pip); CHECK(ui.mode() == Mode::Pip); }

    // The Energy panel in the window / split pane refreshes about once a second while shown; a static window does not re-render at all
    {
        const std::string sp = "/tmp/camera-enable-test-status.json"; std::remove(sp.c_str());
        put(pf, "start_mode=pip\n");
        Config c = off; c.obd_enable = true; c.prefs_path = pf; c.status_path = sp;
        auto comp = std::make_shared<Compositor>(c); UiController ui(c, comp);
        ui.start(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int idle = renders(ui, sp, 2500);                                      // no OBD data yet: nothing to refresh
        obd::Snapshot snap; ui.setEnergy(snap);
        const int live = renders(ui, sp, 10200);
        ui.command("mode aa"); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int aa = renders(ui, sp, 2500);                                        // Android Auto full screen: the panel is not shown, no refresh
        ui.stop();
        std::printf("energy pane refresh: window idle %d, window with data %d, Android Auto %d status writes\n", idle, live, aa);
        CHECK(live >= 8 && live <= 14); CHECK(idle <= 2); CHECK(aa <= 2);
    }

    // UI, camera enabled (default): nothing changes
    put(pf, "start_mode=pip\n");
    { Config c; c.prefs_path = pf; UiController ui(c, std::make_shared<Compositor>(c)); CHECK(ui.mode() == Mode::Pip);
      ui.command("mode split"); CHECK(ui.mode() == Mode::Split); ui.setMode(Mode::FullCam); CHECK(ui.mode() == Mode::FullCam); }

    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
