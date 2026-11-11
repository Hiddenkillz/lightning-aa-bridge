// Prefs robustness: corrupt / truncated / hostile prefs files must leave valid, usable values; save/load must round-trip.
// g++ -std=c++17 -I src tests/prefs_test.cpp src/ui/UiController.cpp ... (built by CMake as prefs-test)
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <unistd.h>
#include <string>
#include "../src/ui/UiController.h"
using namespace ui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }
static bool sane(const Prefs& p) {
    return std::isfinite(p.pip_frac) && p.pip_frac >= 0.15 && p.pip_frac <= 0.85 && std::isfinite(p.split_ratio) && (p.split_ratio == 0.0 || (p.split_ratio >= 0.25 && p.split_ratio <= 0.75)) &&
           std::isfinite(p.cam_zoom) && p.cam_zoom >= 1 && p.cam_zoom <= 4 && std::isfinite(p.cam_cx) && p.cam_cx >= 0 && p.cam_cx <= 1 && std::isfinite(p.cam_cy) && p.cam_cy >= 0 && p.cam_cy <= 1 &&
           std::isfinite(p.roi.x) && std::isfinite(p.roi.y) && std::isfinite(p.roi.w) && std::isfinite(p.roi.h) && p.roi.w > 0 && p.roi.h > 0 &&
           p.dbg_gear >= 0 && p.dbg_gear <= 3 && p.dbg_driving >= 0 && p.dbg_driving <= 2 && p.dbg_night >= 0 && p.dbg_night <= 2;
}
int main() {
    const std::string f = "/tmp/prefs-test.conf";
    // 0. defaults: sensors follow the vehicle (Parked/Day only until it reports), no override
    { Prefs d; CHECK(d.dbg_gear == 0 && d.dbg_driving == 0 && d.dbg_night == 0); }
    // 1. missing file: false, defaults untouched
    std::remove(f.c_str()); { Prefs p; CHECK(!UiController::loadPrefs(f, p)); CHECK(sane(p)); }
    // 2. empty file (power cut after a non-durable rename): loads, defaults kept
    put(f, ""); { Prefs p; CHECK(UiController::loadPrefs(f, p)); CHECK(sane(p)); }
    // 3. hostile numbers
    put(f, "pip_frac=nan\nsplit_h=inf\ncam_zoom=-nan\ncam_center=nan 5\nroi=nan nan nan nan\ndbg_gear=99999999999999999999\ndbg_night=-5\nsplit_h=1e400\n");
    { Prefs p; CHECK(UiController::loadPrefs(f, p)); CHECK(sane(p)); }
    put(f, "pip_frac=999\nsplit_h=-3\ncam_zoom=1e9\ncam_center=-7 9\nroi=-1 -1 5 5\n"); { Prefs p; UiController::loadPrefs(f, p); CHECK(sane(p)); CHECK(p.cam_zoom == 4 && p.split_ratio == 0.5); }
    // 4. truncated in the middle of a value / line, CRLF, garbage, no newline at the end, embedded NULs
    put(f, "start_mode=pip\r\npip_corner=tl\r\nsplit_h=0.6"); { Prefs p; UiController::loadPrefs(f, p); CHECK(sane(p)); CHECK(std::fabs(p.split_ratio - 0.6) < 1e-9); }
    put(f, std::string("start_mode=\0pip\npip_frac=\0\n=\n==\nzzz\n", 36)); { Prefs p; UiController::loadPrefs(f, p); CHECK(sane(p)); }
    put(f, "start_mode=bogus\npip_corner=xx\npip_size=huge\ncam_center=0.3\n"); { Prefs p; UiController::loadPrefs(f, p); CHECK(sane(p)); }
    // 5. fuzz: 3000 random files (random bytes, and random mutations of a valid file) must always yield sane prefs and never crash
    { Prefs a; a.start_mode = Mode::Pip; a.cam_zoom = 2.5; a.cam_cx = 0.3; a.split_ratio = 0.4; UiController::savePrefs(f, a);
      std::ifstream in(f, std::ios::binary); std::string good((std::istreambuf_iterator<char>(in)), {});
      std::mt19937 rng(12345); int bad = 0;
      for (int i = 0; i < 3000; ++i) {
          std::string s = (i % 2) ? good : std::string();
          int n = 1 + rng() % 40;
          if (i % 2 == 0) { int len = rng() % 400; for (int k = 0; k < len; ++k) s.push_back((char)(rng() % 256)); }
          else for (int k = 0; k < n && !s.empty(); ++k) { size_t at = rng() % s.size(); int op = rng() % 3; if (op == 0) s[at] = (char)(rng() % 256); else if (op == 1) s.erase(at, 1 + rng() % 8); else s.insert(at, 1, "0123456789.-+eEnaif =\n"[rng() % 22]); }
          if (rng() % 5 == 0 && !s.empty()) s.resize(rng() % s.size());           // truncate
          put(f, s); Prefs p; UiController::loadPrefs(f, p); if (!sane(p)) ++bad;
      }
      CHECK(bad == 0); std::printf("fuzz: 3000 files, %d insane results\n", bad); }
    // 5b. free PiP position: old files (corner only) become a position, new keys round-trip, a size below the minimum never loads
    { put(f, "pip_corner=tl\n"); Prefs p; UiController::loadPrefs(f, p); CHECK(p.pip_px == 0.0 && p.pip_py == 0.0);                       // old file: corner = position
      put(f, "pip_corner=bl\npip_px=0.37\npip_py=0.62\n"); Prefs q; UiController::loadPrefs(f, q); CHECK(std::fabs(q.pip_px - 0.37) < 1e-9 && std::fabs(q.pip_py - 0.62) < 1e-9);
      put(f, "pip_px=nan\npip_py=9\npip_frac=0.05\n"); Prefs r; UiController::loadPrefs(f, r); CHECK(sane(r) && r.pip_py == 1.0 && r.pip_frac >= kPipMinFrac);
      put(f, "pip_frac=0.22\n"); Prefs s2; UiController::loadPrefs(f, s2); CHECK(s2.pip_frac >= kPipMinFrac);                              // the old Small (22%) is raised
      Prefs a; a.pip_px = 0.123; a.pip_py = 0.456; CHECK(UiController::savePrefs(f, a)); Prefs b; CHECK(UiController::loadPrefs(f, b) && std::fabs(b.pip_px - 0.123) < 1e-6 && std::fabs(b.pip_py - 0.456) < 1e-6); }
    // 6. round trip incl. every field, and the temp file is gone
    { Prefs a; a.start_mode = Mode::Split; a.pip_corner = Corner::TL; a.pip_frac = 0.4; a.split_ratio = 0.33; a.auto_cam_reverse = false; a.cam_fill = true; a.cam_mirror = true; a.cam_flip = true;
      a.cam_zoom = 2.25; a.cam_cx = 0.2; a.cam_cy = 0.8; a.dbg_gear = 2; a.dbg_driving = 1; a.dbg_night = 2; a.start_settings = true;
      CHECK(UiController::savePrefs(f, a)); Prefs b; CHECK(UiController::loadPrefs(f, b));
      CHECK(b.start_mode == a.start_mode && b.pip_corner == a.pip_corner && std::fabs(b.pip_frac - 0.4) < 1e-6 && std::fabs(b.split_ratio - 0.33) < 1e-6 && !b.auto_cam_reverse &&
            b.cam_fill && b.cam_mirror && b.cam_flip && std::fabs(b.cam_zoom - 2.25) < 1e-6 && std::fabs(b.cam_cx - 0.2) < 1e-6 && std::fabs(b.cam_cy - 0.8) < 1e-6 &&
            b.dbg_gear == 2 && b.dbg_driving == 1 && b.dbg_night == 2 && b.start_settings);
      CHECK(::access((f + ".tmp").c_str(), F_OK) != 0); }
    // 7. unwritable location: reports failure instead of crashing
    { Prefs a; CHECK(!UiController::savePrefs("/nonexistent-dir/x/prefs.conf", a)); }
    std::remove(f.c_str());
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails); return fails != 0;
}
