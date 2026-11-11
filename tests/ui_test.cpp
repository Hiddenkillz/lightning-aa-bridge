// Host test for the layout engine + touch router: g++ -std=c++17 -I../src/ui ui_test.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include "../src/ui/InputRouter.h"
using namespace ui;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    const int W = 1280, H = 720;
    Prefs p;

    // Full AA: phone covers everything, camera hidden
    auto L = compute(Mode::FullAA, p, W, H, 640, 480, true, true);
    CHECK(L.phone.visible && L.phone.rect.w == W && L.phone.rect.h == H && !L.camera.visible);

    // Full camera: 4:3 source letterboxed (pillarboxed) and centred, phone hidden
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, true);
    CHECK(!L.phone.visible && L.camera.visible && L.camera.rect.h == H && L.camera.rect.w == 960 && L.camera.rect.x == 160);

    // Split (stacked, default 1/2): phone on top, camera in the strip below, nothing overlaps
    L = compute(Mode::Split, p, W, H, 640, 480, true, true);
    CHECK(L.phone.visible && L.camera.visible);
    CHECK(L.phone.rect.y + L.phone.rect.h <= L.camera.rect.y);
    CHECK(L.phone.rect.x == 0 && L.phone.rect.w == W && L.phone.rect.h == 360);        // reshaped phone fills its pane
    CHECK(L.camera.rect.y >= 360 && L.camera.rect.y + L.camera.rect.h <= H);
    L = compute(Mode::Split, p, W, H, 640, 480, true, true, false);                    // phone not reshaped yet: whole picture scaled into the pane
    CHECK(L.phone.rect.h == 360 && L.phone.rect.w == 640 && L.phone.rect.x == 320 && L.phone_pane.w == W);
    { Prefs q; q.split_ratio = 0.67; L = compute(Mode::Split, q, 1042, 1080, 640, 480, true, true); CHECK(L.phone_pane.h == 722 && L.camera.rect.y >= 722); }   // explicit 2/3
    { Prefs q; L = compute(Mode::Split, q, 1042, 1080, 640, 480, true, true); CHECK(L.phone_pane.h == 540 && L.camera.rect.h > 0 && L.camera.rect.h <= 540); }   // the real window, default 1/2

    // PIP: phone full, camera in the TOP-RIGHT corner, large, with margin, inside the frame (the default)
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);
    CHECK(L.phone.rect.w == W && L.camera.visible);
    CHECK(L.camera.rect.x + L.camera.rect.w == W - 16 && L.camera.rect.y == 16);
    CHECK(L.camera.rect.w == 664);          // 52% of 1280
    cornerPos(Corner::TL, p.pip_px, p.pip_py);
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);
    CHECK(L.camera.rect.x == 16 && L.camera.rect.y == 16);
    // free placement: any fraction of the travel is honoured (no snapping), always inside the window with the margin
    p.pip_px = 0.5; p.pip_py = 0.25;
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);
    { const int tx = W - L.camera.rect.w - 32, ty = H - L.camera.rect.h - 32;
      CHECK(L.camera.rect.x == 16 + (int)std::lround(tx * 0.5) && L.camera.rect.y == 16 + (int)std::lround(ty * 0.25)); }
    p.pip_px = 7.0; p.pip_py = -3.0; L = compute(Mode::Pip, p, W, H, 640, 480, true, true);     // garbage never leaves the window
    CHECK(L.camera.rect.x + L.camera.rect.w <= W - 16 && L.camera.rect.y >= 16);
    p.pip_px = std::nan(""); L = compute(Mode::Pip, p, W, H, 640, 480, true, true); CHECK(L.camera.rect.x >= 16 && L.camera.rect.x + L.camera.rect.w <= W);
    { double fx, fy; Rect r{16 + 100, 16 + 40, 384, 288}; pipFractions(r, W, H, fx, fy); Prefs q; q.pip_frac = 0.30; q.pip_px = fx; q.pip_py = fy; Layout M = compute(Mode::Pip, q, W, H, 640, 480, true, true);
      CHECK(std::abs(M.camera.rect.x - r.x) <= 1 && std::abs(M.camera.rect.y - r.y) <= 1); }   // fractions round-trip a dropped position
    // minimum size = the start size
    { Prefs q; q.pip_frac = 0.05; Layout M = compute(Mode::Pip, q, W, H, 640, 480, true, true); CHECK(M.camera.rect.w == 384);
      CHECK(Prefs().pip_frac == 0.52 && clampPipFrac(0.1) == kPipMinFrac && clampPipFrac(5) == kPipMaxFrac);
      CHECK(pipWidthFraction(PipSize::S) == kPipMinFrac && nearestPipSize(0.30) == PipSize::S && nearestPipSize(0.40) == PipSize::M && nearestPipSize(0.52) == PipSize::L); }

    // Safe zone: a narrow tall-ish crop makes a smaller PIP and a correct videocrop
    p = Prefs(); p.roi = {0.25, 0.0, 0.5, 1.0};                      // centre half of the width
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);
    CHECK(L.camera_crop.left == 160 && L.camera_crop.right == 160 && L.camera_crop.top == 0 && L.camera_crop.bottom == 0);
    {   // crop alignment: the cropped frame's width/height are multiples of camCropAlign() (16) for any zoom/pan; offsets even; crop stays inside the frame
        Prefs z; z.cam_zoom = 2.37; z.cam_cx = 0.71; z.cam_cy = 0.33;
        for (int cw : {1280, 1920}) for (int chh : {720, 1080}) {
            Layout Z = compute(Mode::FullCam, z, W, H, cw, chh, false, true, true);
            const int w = cw - Z.camera_crop.left - Z.camera_crop.right, h = chh - Z.camera_crop.top - Z.camera_crop.bottom;
            CHECK(w % 16 == 0 && h % 16 == 0 && w >= 16 && h >= 16 && Z.camera_crop.left % 2 == 0 && Z.camera_crop.top % 2 == 0 && Z.camera_crop.left >= 0 && Z.camera_crop.top >= 0);
        }
        camCropAlign() = 2; Layout Z2 = compute(Mode::FullCam, z, W, H, 1280, 720, false, true, true); camCropAlign() = 16;
        CHECK((1280 - Z2.camera_crop.left - Z2.camera_crop.right) % 2 == 0);
    }
    CHECK(L.camera.rect.h > L.camera.rect.w * 0.9);                  // crop is 320x480 -> portrait aspect
    CHECK(L.camera.rect.h % 2 == 0 && L.camera.rect.w % 2 == 0);

    // No fallbacks: the chosen layout is always laid out. A pane whose source is missing keeps its place and is a placeholder (!live).
    p = Prefs();
    L = compute(Mode::Pip, p, W, H, 640, 480, /*phone*/false, true);
    CHECK(L.phone.visible && !L.phone.live && L.camera.visible && L.camera.live && L.camera.rect.w == 664);
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, /*camera*/false);
    CHECK(!L.phone.visible && L.camera.visible && !L.camera.live && L.camera.rect.h == H);
    L = compute(Mode::FullAA, p, W, H, 640, 480, false, false);
    CHECK(L.phone.visible && !L.phone.live && L.phone.rect.w == W && !L.camera.visible);
    L = compute(Mode::Split, p, W, H, 640, 480, false, false);
    CHECK(L.phone.visible && !L.phone.live && L.camera.visible && !L.camera.live);
    { Layout lo = liveOnly(L); CHECK(!lo.phone.visible && !lo.camera.visible); }                 // the video compositor gets nothing for placeholders
    { Layout lo = liveOnly(compute(Mode::Pip, p, W, H, 640, 480, true, false)); CHECK(lo.phone.visible && lo.phone.live && !lo.camera.visible); }

    // ---- camera view: Fill, zoom/pan, mirror/flip do not touch the layout ----
    p = Prefs(); p.cam_fill = true;
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, true);                                   // 4:3 source into 16:9: crop top/bottom, cover the window
    CHECK(L.camera.rect.w == W && L.camera.rect.h == H && L.camera_crop.top == 60 && L.camera_crop.bottom == 68 && L.camera_crop.left == 0 && L.camera_crop.right == 0);   // 360 -> 352 (16-aligned height)
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);                                       // Fill is a full-screen option only
    CHECK(L.camera.rect.w == 664 && L.camera_crop.top == 0);
    p = Prefs(); p.cam_zoom = 2.0; p.cam_cx = 0.5; p.cam_cy = 0.5;
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, true);
    CHECK(L.camera_crop.left == 160 && L.camera_crop.right == 160 && L.camera_crop.top == 120 && L.camera_crop.bottom == 120);
    p.cam_cx = 0.0; p.cam_cy = 1.0;                                                              // panned to the top-right... clamped to the edge of the picture
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, true);
    CHECK(L.camera_crop.left == 0 && L.camera_crop.right == 320 && L.camera_crop.bottom == 0 && L.camera_crop.top == 240);
    p = Prefs(); p.roi = {0.0, 0.0, 0.5, 1.0}; p.cam_zoom = 2.0; p.cam_cx = 1.0; p.cam_cy = 0.5;   // zoom lives INSIDE the safe zone
    { Roi v = viewRoi(p); CHECK(std::abs(v.x - 0.25) < 1e-9 && std::abs(v.w - 0.25) < 1e-9 && std::abs(v.h - 0.5) < 1e-9); }
    p = Prefs(); p.cam_mirror = true; p.cam_flip = true;
    L = compute(Mode::FullCam, p, W, H, 640, 480, true, true);
    CHECK(L.camera.rect.w == 960 && L.camera_crop.left == 0);                                    // flips are applied by the video element, not the layout

    // ---- touch routing ----
    InputRouter r;
    r.configure(/*touch*/2560, 1440, W, H, /*phone*/1280, 720);       // truck touch space is 2x the video
    p = Prefs();
    L = compute(Mode::Pip, p, W, H, 640, 480, true, true);
    r.setLayout(L);
    r.setButtons({{7, {0, 0, 100, 60}}});

    // tap on phone area: remapped 1:1 (touch 2x -> output 1x -> phone 1x)
    auto a = r.route(1000, 600, Action::Down); CHECK(a.target == Target::Phone && a.phone_x == 500 && a.phone_y == 300 && a.gesture_start);
    a = r.route(1010, 610, Action::Move);      CHECK(a.target == Target::Phone && !a.gesture_start);
    a = r.route(1010, 610, Action::Up);        CHECK(a.target == Target::Phone);
    // tap on UI button wins; gesture dragged onto phone still belongs to the button
    a = r.route(50, 50, Action::Down);         CHECK(a.target == Target::Ui && a.ui_id == 7);
    a = r.route(1500, 800, Action::Move);      CHECK(a.target == Target::Ui);
    a = r.route(1500, 800, Action::Up);        CHECK(a.target == Target::Ui);
    // tap on the camera window is consumed
    int cx = (L.camera.rect.x + 10) * 2, cy = (L.camera.rect.y + 10) * 2;
    a = r.route(cx, cy, Action::Down);         CHECK(a.target == Target::Camera);
    a = r.route(cx, cy, Action::Up);

    // split: touch in the phone pane maps into full phone frame
    L = compute(Mode::Split, Prefs(), W, H, 640, 480, true, true);
    r.setLayout(L); r.setButtons({});
    // phone pane is {0,0,1280,360}; its centre is output (640,180) (touch space is 2x) -> phone frame (640,360)
    a = r.route(1280, 360, Action::Down);       CHECK(a.target == Target::Phone && a.phone_x == 640 && a.phone_y == 360);
    r.route(1280, 360, Action::Up);
    // the strip below the phone belongs to the camera; its empty corners belong to nobody
    a = r.route(1280, 1400, Action::Down);       CHECK(a.target == Target::Camera);
    r.route(1280, 1400, Action::Up);
    a = r.route(200, 1400, Action::Down);       CHECK(a.target == Target::None);
    r.route(200, 1400, Action::Up);

    // placeholder panes take no touches (nothing to forward the touch to); a placeholder camera window is still a camera target
    L = compute(Mode::FullAA, Prefs(), W, H, 640, 480, /*phone*/false, true);
    r.setLayout(L); r.setButtons({});
    a = r.route(640, 720, Action::Down);       CHECK(a.target == Target::None);
    r.route(640, 720, Action::Up);
    L = compute(Mode::Pip, Prefs(), W, H, 640, 480, false, /*camera*/false);
    r.setLayout(L);
    cx = (L.camera.rect.x + 10) * 2; cy = (L.camera.rect.y + 10) * 2;
    a = r.route(cx, cy, Action::Down);         CHECK(a.target == Target::Camera);
    r.route(cx, cy, Action::Up);

    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
