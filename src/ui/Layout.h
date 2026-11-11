#pragma once
// Layout engine: pure geometry, no GStreamer. Decides where the phone video and
// the camera video go in the output frame for each mode. The Compositor applies
// the result to its pads; InputRouter uses the same result for hit-testing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ui {

enum class Mode { FullAA, FullCam, Split, Pip, Energy };   // Energy: full-screen vehicle data page (OBD-II), no video pane; beta of the data view
enum class Corner { TL, TR, BL, BR };
enum class PipSize { S, M, L };

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool intersects(const Rect& o) const { return w > 0 && h > 0 && o.w > 0 && o.h > 0 && x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
    bool contains(int px, int py) const { return w > 0 && h > 0 && px >= x && px < x + w && py >= y && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// Normalised crop of the camera frame ("safe zone"), 0..1 of the source frame.
struct Roi {
    double x = 0, y = 0, w = 1, h = 1;
    bool full() const { return x <= 0 && y <= 0 && w >= 1 && h >= 1; }
    void clamp() {
        x = std::clamp(x, 0.0, 0.95); y = std::clamp(y, 0.0, 0.95);
        w = std::clamp(w, 0.05, 1.0 - x); h = std::clamp(h, 0.05, 1.0 - y);
    }
};

struct Prefs {
    Mode start_mode = Mode::FullCam;     // used when no phone is attached, or as the boot mode
    Corner pip_corner = Corner::TR;      // the last corner PRESET chosen (kept for old prefs files and the Settings buttons); the real position is pip_px/pip_py
    double pip_px = 1.0, pip_py = 0.0;   // where the PiP sits, as a fraction of its free travel: 0 = left/top edge (with margin), 1 = right/bottom. Set by dropping it anywhere (no snapping).
    PipSize pip_size = PipSize::L;       // nearest preset to pip_frac (kept for the Settings buttons)
    double pip_frac = 0.52;              // PiP width as a fraction of the window width (pinch-zoom sets this; persisted)
    double split_ratio = 0.5;            // Split is stacked (phone on top, camera below): fraction of the HEIGHT for the phone
    bool auto_cam_reverse = false;
    Roi roi;                             // camera safe zone
    // transient (never saved): the PiP is being dragged and follows the finger; on release it snaps to the nearest corner
    bool pip_drag = false; int pip_x = 0, pip_y = 0;     // top-left in window pixels
    // Debug overrides (persisted like every other setting). 0 = follow the truck.
    int dbg_gear = 0;                    // 0 follow the vehicle, 1 Park, 2 Reverse, 3 Drive   (drives the auto-camera-in-reverse rule; also lets it be tested on the bench)
    int dbg_driving = 0;                 // driving status sent to the phone: 0 follow the vehicle (Parked until it reports), 1 Parked, 2 Moving
    int dbg_night = 0;                   // night mode sent to the phone: 0 follow the vehicle (Day until it reports), 1 Day, 2 Night
    bool start_last = false;             // "Always start on: Last": boot into the mode that was showing when power went away
    Mode last_mode = Mode::FullCam;      // updated on every mode change
    bool start_settings = false;         // "Always start on: Settings" (start_mode is then ignored at boot; no longer offered in the UI, kept for old prefs files / the FIFO)
    int theme = 0;                       // 0 Auto (follows the night mode the phone is told), 1 Dark, 2 Light
    bool split_drag = false;             // Debug: the Split divider can be dragged (experiment; config split_drag also turns it on)
    // camera view (the full-screen camera bar; applies in every layout, persisted)
    bool cam_fill = false;               // full-screen camera: Fit (whole picture, letterboxed) or Fill (crop to the window)
    bool cam_mirror = false, cam_flip = false;   // horizontal / vertical flip (how the camera is mounted)
    double cam_zoom = 1.0;               // 1..4, pinch in the full-screen camera
    double cam_cx = 0.5, cam_cy = 0.5;   // centre of the zoomed view inside the safe zone, 0..1
};

struct Pane {                            // one video layer as the compositor should draw it
    bool visible = false;                // this mode wants the pane (even when its source is missing: a placeholder is drawn there)
    Rect rect;                           // destination in output pixels
    bool live = false;                   // a real video source feeds it (only live panes are given to the compositor / receive touches)
};

struct CropPx { int left = 0, right = 0, top = 0, bottom = 0; };  // for videocrop on the camera branch

struct Layout {
    Rect phone_pane;                     // Split: the whole top pane the phone is shown in (the phone rect itself is letterboxed inside it until the phone reshaped its UI)
    Pane phone, camera;
    int camera_radius = 0;               // rounded corners of the camera window in output px (PiP only; 0 = square)
    CropPx camera_crop;
    Rect camera_hit;                     // area that consumes touches (== camera.rect when visible)
    Rect split_handle;                   // Split: the divider band between the panes (drag to resize; empty elsewhere)
};

inline int even(int v) { return v & ~1; }
inline int& camCropAlign() { static int a = 16; return a; }           // set from config cam_crop_align (process-wide; the layout is a pure function otherwise)
inline int alignDown(int v, int a) { a = std::max(2, a); return std::max(a, v - v % a); }

// What the compositor may draw: live panes only (placeholders are drawn by the UI overlay).
inline Layout liveOnly(Layout L) {
    if (!L.phone.live) L.phone = {};
    if (!L.camera.live) L.camera = {};
    return L;
}

// The same layout moved by (dx,dy): visible-window coordinates -> output-frame coordinates.
inline Layout shifted(Layout L, int dx, int dy) {
    L.phone.rect.x += dx; L.phone.rect.y += dy;
    L.camera.rect.x += dx; L.camera.rect.y += dy;
    L.camera_hit.x += dx; L.camera_hit.y += dy;
    L.split_handle.x += dx; L.split_handle.y += dy;
    return L;
}

// Right-edge control strip (output px): the collapsed handle tab AND the expanded mode pill are drawn inside this fixed region, so the small overlay
// layer for it never needs renegotiation. Its size depends only on the output HEIGHT (the window and the full frame share it).
inline Rect handleRect(int W, int H) {
    double s = H / 720.0;
    int w = (int)(84 * s + 0.5), h = (int)(340 * s + 0.5);     // 5 buttons of 56 + gaps + two group dividers
    return {W - w, H / 2 - h / 2, w, h};
}

// Largest rectangle with aspect ratio aw:ah that fits inside `box`, centred.
inline Rect fit(const Rect& box, double aw, double ah) {
    if (box.empty() || aw <= 0 || ah <= 0) return {};
    double s = std::min(box.w / aw, box.h / ah);
    int w = even((int)(aw * s)), h = even((int)(ah * s));
    return {box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h};
}

// Corner radius of the PiP window: 2.5% of the window height (18 px at 720, 27 px at 1080).
inline int pipCornerRadius(int H) { return std::clamp((int)std::lround(H * 0.025), 8, 40); }
// The PiP starts at kPipMinFrac of the window width and can never be made smaller (smaller than this the window buttons crowd the picture and there is nothing left to grab).
constexpr double kPipMinFrac = 0.30, kPipMaxFrac = 0.85;
inline double pipWidthFraction(PipSize s) { return s == PipSize::S ? kPipMinFrac : s == PipSize::M ? 0.40 : 0.52; }
inline double clampPipFrac(double f) { return std::clamp(f, kPipMinFrac, kPipMaxFrac); }
inline PipSize nearestPipSize(double f) { return f < 0.35 ? PipSize::S : f < 0.46 ? PipSize::M : PipSize::L; }
inline void cornerPos(Corner c, double& px, double& py) { px = (c == Corner::TL || c == Corner::BL) ? 0.0 : 1.0; py = (c == Corner::TL || c == Corner::TR) ? 0.0 : 1.0; }
inline bool atCorner(const Prefs& p, Corner c) { double x, y; cornerPos(c, x, y); return std::fabs(p.pip_px - x) < 0.02 && std::fabs(p.pip_py - y) < 0.02; }
inline constexpr int kPipMargin = 16;                 // px between the PiP and the window edges

// The safe zone narrowed by the zoom/pan view (zoom 1 = the whole safe zone).
inline Roi viewRoi(const Prefs& p) {
    Roi r = p.roi; r.clamp();
    const double z = std::clamp(p.cam_zoom, 1.0, 4.0);
    if (z <= 1.0001) return r;
    const double vw = r.w / z, vh = r.h / z;
    const double x = r.x + std::clamp(std::clamp(p.cam_cx, 0.0, 1.0) * r.w - vw / 2, 0.0, r.w - vw);
    const double y = r.y + std::clamp(std::clamp(p.cam_cy, 0.0, 1.0) * r.h - vh / 2, 0.0, r.h - vh);
    return {x, y, vw, vh};
}

// cam_w/cam_h: camera source resolution (for crop pixels and aspect).
inline Layout compute(Mode mode, const Prefs& p, int W, int H, int cam_w, int cam_h, bool phone_present, bool camera_present, bool phone_reshaped = true) {
    Layout L;
    Roi roi = viewRoi(p);
    const double sw = cam_w > 0 ? cam_w : 1280, sh = cam_h > 0 ? cam_h : 720;
    // Fill (full-screen camera only): trim the view to the window's aspect so the picture covers the whole window.
    const bool fill = p.cam_fill && mode == Mode::FullCam;
    if (fill && W > 0 && H > 0) {
        const double want = (double)W / H, have = (sw * roi.w) / (sh * roi.h);
        if (have > want) { const double k = want / have; roi.x += roi.w * (1 - k) / 2; roi.w *= k; }
        else if (have < want) { const double k = have / want; roi.y += roi.h * (1 - k) / 2; roi.h *= k; }
    }
    const double cw = sw * roi.w, ch = sh * roi.h;                    // cropped source size
    if (cam_w > 0 && cam_h > 0) {
        // The cropped frame keeps aligned dimensions (multiples of cam_crop_align, default 16): the mixer's scaler has a fast path only for aligned widths;
        // an arbitrary crop (ui49a: 814 and 688 px wide) made every frame cost more than its interval and the picture fell seconds behind. Offsets stay even.
        const int a = camCropAlign();
        int cwp = std::min(cam_w, alignDown((int)(roi.w * cam_w), a)), chp = std::min(cam_h, alignDown((int)(roi.h * cam_h), a));
        L.camera_crop.left = std::clamp(even((int)(roi.x * cam_w)), 0, cam_w - cwp);
        L.camera_crop.top = std::clamp(even((int)(roi.y * cam_h)), 0, cam_h - chp);
        L.camera_crop.right = std::max(0, cam_w - L.camera_crop.left - cwp);
        L.camera_crop.bottom = std::max(0, cam_h - L.camera_crop.top - chp);
    }
    // No fallbacks: the chosen mode is always laid out. A pane whose source is missing keeps its place and is marked !live; the UI draws a
    // placeholder there (Android Auto / camera artwork). Only live panes reach the compositor and receive touches.

    const Rect full{0, 0, W, H};
    switch (mode) {
    case Mode::FullAA:
        L.phone = {true, full};
        break;
    case Mode::FullCam:
        L.camera = {true, fill ? full : fit(full, cw, ch)};
        break;
    case Mode::Split: {
        // Stacked: the phone on top in a 16:9 pane (like the Ottocast), the camera in what is left below.
        int ph = (int)(H * std::clamp(p.split_ratio > 0.0 ? p.split_ratio : 0.5, 0.25, 0.75));
        ph = std::clamp(even(ph), even(H / 4), even(H * 3 / 4));
        const Rect top{0, 0, W, ph}, bottom{0, ph, W, H - ph};
        L.phone_pane = top;
        L.phone = {true, phone_reshaped ? top : fit(top, W, H)};   // reshaped: the phone's UI fills the pane. Otherwise the whole window picture is scaled down into it
        L.camera = {true, fit(bottom, cw, ch)};
        { const int bh = std::max(24, (int)std::lround(H * 0.05)); L.split_handle = {0, ph - bh / 2, W, bh}; }   // grab band centred on the divider (5% of the height)
        break;
    }
    case Mode::Energy:
        break;                                                      // nothing live: the overlay paints the whole page
    case Mode::Pip: {
        L.phone = {true, full};
        int w = even((int)(W * clampPipFrac(p.pip_frac)));
        int h = even((int)(w * ch / cw));
        const int m = kPipMargin;                       // margin from the edges
        if (H > 0 && h > H - 2 * m) { h = even(std::max(2, H - 2 * m)); w = even(std::max(2, (int)(h * cw / ch))); }   // never taller than the window (wide windows, big PiP)
        if (W > 0 && w > W - 2 * m) { w = even(std::max(2, W - 2 * m)); h = even(std::max(2, (int)(w * ch / cw))); }
        const double fx = std::isfinite(p.pip_px) ? std::clamp(p.pip_px, 0.0, 1.0) : 1.0, fy = std::isfinite(p.pip_py) ? std::clamp(p.pip_py, 0.0, 1.0) : 1.0;
        int x = m + (int)std::lround(std::max(0, W - w - 2 * m) * fx);
        int y = m + (int)std::lround(std::max(0, H - h - 2 * m) * fy);
        if (p.pip_drag) { x = std::clamp(p.pip_x, m, std::max(m, W - w - m)); y = std::clamp(p.pip_y, m, std::max(m, H - h - m)); }   // follows the finger, same margin as at rest (no jump on release)
        L.camera = {true, {x, y, w, h}};
        L.camera_radius = pipCornerRadius(H);
        break;
    }
    }
    L.phone.live = L.phone.visible && phone_present;
    L.camera.live = L.camera.visible && camera_present;
    L.camera_hit = L.camera.visible ? L.camera.rect : Rect{};      // a placeholder PiP can be tapped (expand) just like a live one
    return L;
}

// Position of a PiP rectangle as the pip_px/pip_py fractions of its free travel (inverse of what compute() does).
inline void pipFractions(const Rect& r, int W, int H, double& px, double& py) {
    const int tx = W - r.w - 2 * kPipMargin, ty = H - r.h - 2 * kPipMargin;
    px = tx > 0 ? std::clamp((double)(r.x - kPipMargin) / tx, 0.0, 1.0) : 1.0;
    py = ty > 0 ? std::clamp((double)(r.y - kPipMargin) / ty, 0.0, 1.0) : 1.0;
}
// The corner whose PiP position is nearest to a dragged PiP (by its centre: the quadrant it is in).
inline Corner nearestCorner(const Rect& r, int W, int H) {
    const bool right = r.x + r.w / 2 >= W / 2, bottom = r.y + r.h / 2 >= H / 2;
    return right ? (bottom ? Corner::BR : Corner::TR) : (bottom ? Corner::BL : Corner::TL);
}

inline const char* modeName(Mode m) {
    switch (m) { case Mode::FullAA: return "aa"; case Mode::FullCam: return "cam"; case Mode::Split: return "split"; case Mode::Pip: return "pip"; case Mode::Energy: return "energy"; }
    return "?";
}
inline bool parseMode(const std::string& s, Mode& m) {
    if (s == "aa") m = Mode::FullAA; else if (s == "cam") m = Mode::FullCam;
    else if (s == "split") m = Mode::Split; else if (s == "pip") m = Mode::Pip; else if (s == "energy") m = Mode::Energy; else return false;
    return true;
}

}  // namespace ui
