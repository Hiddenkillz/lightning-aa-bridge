#include "Menu.h"
#include <algorithm>
#include <cmath>
#include <tuple>
#include "Placeholder.h"
#include "IconData.h"

namespace ui {

struct Menu::Pal { Color panel, btn, btnPress, btnOff, accent, text, dim, off, pillBg, pillBorder, pillSel, icon, paneBg, chip, warm, line, readout, warnBg, warnFg; };
static const Menu::Pal kDark{
    {20, 23, 29, 248}, {58, 62, 74, 255}, {92, 98, 114, 255}, {40, 42, 50, 255}, {38, 132, 255, 255}, {255, 255, 255, 255}, {150, 156, 168, 255}, {96, 100, 110, 255},
    {15, 19, 24, 238}, {42, 51, 64, 255}, {60, 69, 84, 255}, {200, 206, 216, 255}, {8, 10, 14, 178}, {16, 20, 26, 232}, {255, 149, 0, 255},
    {48, 54, 63, 255}, {43, 48, 58, 255}, {74, 47, 46, 255}, {255, 180, 176, 255}};
static const Menu::Pal kLight{
    {246, 247, 249, 250}, {223, 226, 232, 255}, {200, 205, 214, 255}, {236, 238, 242, 255}, {31, 111, 232, 255}, {21, 23, 28, 255}, {93, 102, 117, 255}, {160, 166, 178, 255},
    {246, 247, 249, 236}, {201, 206, 216, 255}, {210, 214, 222, 255}, {52, 58, 70, 255}, {246, 247, 249, 190}, {240, 242, 246, 236}, {255, 149, 0, 255},
    {201, 206, 216, 255}, {231, 234, 239, 255}, {250, 226, 224, 255}, {170, 40, 36, 255}};
#define kPanel (P_->panel)
#define kBtn (P_->btn)
#define kBtnPress (P_->btnPress)
#define kBtnOff (P_->btnOff)
#define kAccent (P_->accent)
#define kText (P_->text)
#define kDim (P_->dim)
#define kOff (P_->off)
#define kPillBg (P_->pillBg)
#define kPillBorder (P_->pillBorder)
#define kPillSel (P_->pillSel)
#define kIcon (P_->icon)
#define kPaneBg (P_->paneBg)
#define kChip (P_->chip)
#define kWarm (P_->warm)

enum Icon { ICON_NONE, ICON_AA, ICON_CAM, ICON_SPLIT, ICON_PIP, ICON_GEAR, ICON_MAX, ICON_CLOSE, ICON_FIT, ICON_FILL, ICON_MIRROR, ICON_FLIP, ICON_ZOOM, ICON_ZONE, ICON_PRECOND, ICON_RESET, ICON_ENERGY };
enum Style { ST_BUTTON = 0, ST_SEGMENT = 1, ST_PANE = 2, ST_PILL = 3, ST_CHIP = 4 };

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Which layers carry content
// ---------------------------------------------------------------------------------------------------------------------------------------------
bool Menu::needsPanel(const Model& m) const {
    if (screen_ == Screen::Settings || screen_ == Screen::Debug || screen_ == Screen::ZoneEdit) return true;
    const Layout& L = m.layout;
    if ((L.phone.visible && !L.phone.live && !m.base_art) || (L.camera.visible && !L.camera.live)) return true;   // placeholder art in the overlay somewhere
    return m.mode != Mode::FullAA;                                                                   // camera bar / window buttons
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Build: widgets (hit areas) for the current state
// ---------------------------------------------------------------------------------------------------------------------------------------------
void Menu::build(const Model& m, int64_t now) {
    widgets_.clear(); labels_.clear(); buttons_.clear();
    mode_ = m.mode;
    Canvas tmp(1, 1);
    auto add = [&](int id, Rect r, std::string label, int px, bool sel, bool en, int icon = ICON_NONE, int style = ST_BUTTON) {
        widgets_.push_back({id, r, std::move(label), px, sel, en, icon, style});
    };
    const Layout& L = m.layout;

    if (screen_ == Screen::Hidden || screen_ == Screen::Home) {
        // ---- camera window buttons (PiP: Max, Close.  Split: PiP, Max, Close) ----
        const bool awake = awakeAt(now) || screen_ == Screen::Home;      // window buttons only for a few seconds after a touch
        if (awake && !m.prefs.pip_drag && m.mode == Mode::Pip && L.camera.visible && !L.camera.rect.empty()) {   // hidden while dragging: nothing to redraw per step
            const Rect cr = L.camera.rect;
            const int sz = std::clamp((int)(cr.h * 0.30), S(34), S(48)), mg = S(6), gp = S(5);
            const int nb = m.mode == Mode::Split ? 3 : 2, y = cr.y + mg;
            // The edge tab / pill owns the right edge. If the buttons at the pane's top-right corner would sit under it (a big PiP in a right-hand
            // corner reaches up into the strip), put them at the top-left of the pane instead, so they can never be covered.
            Rect strip = handleRect(W_, H_); strip.x -= S(8);
            const Rect trCorner{cr.x + cr.w - mg - nb * sz - (nb - 1) * gp, y, nb * sz + (nb - 1) * gp, sz};
            const bool left = strip.intersects(trCorner);
            int x = left ? cr.x + mg + (nb - 1) * (sz + gp) : cr.x + cr.w - mg - sz;         // x of the first (rightmost) button
            add(ID_PANE_CLOSE, {x, y, sz, sz}, "", 0, false, true, ICON_CLOSE, ST_PANE); x -= sz + gp;
            add(ID_PANE_MAX, {x, y, sz, sz}, "", 0, false, true, ICON_MAX, ST_PANE); x -= sz + gp;
            if (m.mode == Mode::Split) add(ID_PANE_PIP, {x, y, sz, sz}, "", 0, false, true, ICON_PIP, ST_PANE);
        }
        // ---- full-screen camera bar: three toggles (state dot) | divider | two actions ----
        if (m.mode == Mode::FullCam && !m.energy_shown) {   // (energy_shown only hides nothing: guard kept simple)
            const Prefs& p = m.prefs;
            const int mg = S(12), gp = S(8), div = S(14), n = 5, cw = (W_ - 2 * mg - (n - 1) * gp - div) / n, ch = S(76), y = H_ - S(18) - ch;
            const bool zoomed = p.cam_zoom > 1.01;
            struct C { int id; const char* l; int ic; bool sel, en; } cs[n] = {
                {ID_CAM_FIT, p.cam_fill ? "Fill screen" : "Fit", p.cam_fill ? ICON_FILL : ICON_FIT, p.cam_fill, true},
                {ID_CAM_MIRROR, "Mirror", ICON_MIRROR, p.cam_mirror, true},
                {ID_CAM_FLIP, "Flip", ICON_FLIP, p.cam_flip, true},
                {ID_CAM_RESET, "Reset view", ICON_RESET, false, zoomed},
                {ID_CAM_ZONE, "Safe zone", ICON_ZONE, !p.roi.full(), m.camera}};
            for (int i = 0; i < n; ++i) add(cs[i].id, {mg + i * (cw + gp) + (i >= 3 ? div : 0), y, cw, ch}, cs[i].l, S(18), cs[i].sel, cs[i].en, cs[i].ic, ST_CHIP);
        }
        // ---- split divider grip ----
        if (m.split_drag_enabled && m.mode == Mode::Split && !L.split_handle.empty()) add(ID_SPLIT_HANDLE, L.split_handle, "", 0, m.split_dragging, true, ICON_NONE, ST_BUTTON);
        // ---- right-edge pill / tab ----
        const Rect R = handleRect(W_, H_);
        pill_ = R;
        if (pillOpen(m) && !m.split_dragging) {                           // no strip while the divider is dragged: the overlay must stay tiny
            const bool pc = m.precond_shown, en = m.energy_shown;          // 6-7 buttons instead of 5: the strip height is fixed, so they get a little smaller
            struct B { int id; int ic; Mode md; bool isMode; bool divBefore; };
            std::vector<B> bs = {{ID_MODE_AA, ICON_AA, Mode::FullAA, true, false}, {ID_MODE_CAM, ICON_CAM, Mode::FullCam, true, false},
                {ID_MODE_PIP, ICON_PIP, Mode::Pip, true, true}, {ID_MODE_SPLIT, ICON_SPLIT, Mode::Split, true, false}};
            if (en) bs.push_back({ID_MODE_ENERGY, ICON_ENERGY, Mode::Energy, true, true});
            if (pc) bs.push_back({ID_PRECOND, ICON_PRECOND, Mode::FullAA, false, !en});
            bs.push_back({ID_SETTINGS, ICON_GEAR, Mode::FullAA, false, !pc && !en});
            const int n = (int)bs.size(), bw = S(60), divh = S(12), gp = (pc || en) ? S(5) : S(6);
            int ndiv = 0; for (const auto& b : bs) if (b.divBefore) ++ndiv;
            int bh = (R.h - S(12) - (n - 1) * gp - ndiv * divh) / n; bh = std::min(bh, S(56));
            const int tot = n * bh + (n - 1) * gp + ndiv * divh, x = R.x + S(8); int y = R.y + (R.h - tot) / 2;
            pill_divs_.clear();
            for (int i = 0; i < n; ++i) {
                if (bs[i].divBefore) { pill_divs_.push_back({x + S(10), y + divh / 2 - S(1), bw - S(20), std::max(1, S(2))}); y += divh; }
                add(bs[i].id, {x, y, bw, bh}, "", 0, bs[i].id == ID_PRECOND ? m.precond != 0 : (bs[i].isMode && m.mode == bs[i].md), true, bs[i].ic, ST_PILL);
                y += bh + gp;
            }
        }   // no pill: nothing at all on the edge (a touch anywhere brings it back)
    } else if (screen_ == Screen::Settings || screen_ == Screen::Debug) {
        add(ID_NONE, {0, 0, W_, H_}, "", 0, false, true);                      // opaque page: swallows every touch outside the card
        buildRows(m, screen_ == Screen::Settings);
    } else {  // ZoneEdit
        const int gap = S(16);
        add(ID_ZONE_CANVAS, {0, 0, W_, H_}, "", 0, false, true);
        const int w = std::min(S(220), (W_ - S(24) - 2 * gap) / 3), tot = 3 * w + 2 * gap, x0 = (W_ - tot) / 2, y = H_ - S(100);
        bool valid = zone_has_ && (zx1_ - zx0_) >= S(24) && (zy1_ - zy0_) >= S(24);
        add(ID_ZONE_CANCEL, {x0, y, w, S(76)}, "Cancel", S(28), false, true);
        add(ID_ZONE_RESET, {x0 + w + gap, y, w, S(76)}, "Full frame", S(26), false, true);
        add(ID_ZONE_SAVE, {x0 + 2 * (w + gap), y, w, S(76)}, "Save", S(28), valid, valid);
    }
    for (const auto& wd : widgets_) buttons_.push_back({wd.id, wd.r});
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Settings / Debug pages: a card with a header (X left, title, Debug right), rows of "label + hint | segmented control", sections, a readout box and a footer.
// A row whose controls do not fit beside the label gets its controls on a full-width line below it (equal segments).
// ---------------------------------------------------------------------------------------------------------------------------------------------
void Menu::buildRows(const Model& m, bool settings) {
    Canvas tmp(1, 1);
    auto add = [&](int id, Rect r, std::string label, int px, bool sel, bool en, int icon = ICON_NONE, int style = ST_BUTTON) { widgets_.push_back({id, r, std::move(label), px, sel, en, icon, style}); };
    rows_.clear(); sections_.clear(); readout_ = {}; foot_left_.clear(); foot_right_.clear();
    const int pad = S(12);
    card_ = {pad, pad, W_ - 2 * pad, H_ - 2 * pad};
    const int ix = card_.x + S(30), iw = card_.w - 2 * S(30);                           // inner column
    const int hy = card_.y + S(14), hs = S(56);
    add(settings ? ID_CLOSE : ID_BACK, {card_.x + S(16), hy, hs, hs}, "", 0, false, true, ICON_CLOSE, ST_BUTTON);
    if (settings) add(ID_DEBUG, {card_.x + card_.w - S(16) - S(140), hy + S(4), S(140), hs - S(8)}, "Debug", S(24), false, true, ICON_NONE, ST_BUTTON);
    const Prefs& p = m.prefs;
    const int sh = settings ? S(50) : S(44), spx = S(22), segGap = S(6), labelW = settings ? S(330) : S(180);   // Debug: shorter labels and buttons so all five rows + the readout fit   // Debug labels are short: narrower column so the override rows fit beside them
    int y = card_.y + S(14) + hs + S(18);
    auto section = [&](const char* t) { sections_.push_back({{ix, y, iw, S(22)}, t}); y += settings ? S(30) : S(27); };
    auto row = [&](std::string label, std::string hint, std::vector<std::tuple<int, std::string, bool, bool>> segs) {
        int tot = 0; for (auto& s : segs) tot += std::max(S(76), tmp.textWidth(std::get<1>(s), spx) + S(30)) + segGap; tot -= segGap;
        const bool wide = tot > iw - labelW;
        const int hintLines = hint.empty() ? 0 : (tmp.textWidth(hint, S(15)) > (wide ? iw : labelW) ? 2 : 1);
        const int textH = S(26) + hintLines * S(20);
        const int rh = wide ? textH + S(10) + sh + S(14) : std::max(textH, sh) + S(22);
        RowSpec r{label, hint, segs, wide, y, rh};
        rows_.push_back(r);
        if (wide) {
            const int sy = y + textH + S(10), w = (iw - (int)(segs.size() - 1) * segGap) / (int)segs.size();
            for (size_t i = 0; i < segs.size(); ++i) add(std::get<0>(segs[i]), {ix + (int)i * (w + segGap), sy, w, sh}, std::get<1>(segs[i]), spx, std::get<2>(segs[i]), std::get<3>(segs[i]), ICON_NONE, ST_SEGMENT);
        } else {
            int right = ix + iw; const int sy = y + (rh - sh) / 2;
            for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
                const int w = std::max(S(76), tmp.textWidth(std::get<1>(*it), spx) + S(30)); right -= w;
                add(std::get<0>(*it), {right, sy, w, sh}, std::get<1>(*it), spx, std::get<2>(*it), std::get<3>(*it), ICON_NONE, ST_SEGMENT);
                right -= segGap;
            }
        }
        y += rh;
    };
    if (settings) {
        const bool sl = p.start_last, ss = p.start_settings;
        auto on = [&](Mode md) { return !sl && !ss && p.start_mode == md; };
        row("Start on", "What the screen shows when the vehicle starts", {{ID_START_AA, "Android", on(Mode::FullAA), true}, {ID_START_CAM, "Camera", on(Mode::FullCam), true},
            {ID_START_PIP, "Window", on(Mode::Pip), true}, {ID_START_SPLIT, "Split", on(Mode::Split), true}, {ID_START_LAST, "Last", sl, true}});
        row("Split: phone share", m.split_drag_enabled ? "How much of the screen the phone gets in Split. Drag the divider for any other size." : "How much of the screen the phone gets in Split",
            {{ID_SPLIT_33, "1/3", std::fabs(p.split_ratio - 0.33) < 0.04, true}, {ID_SPLIT_50, "1/2", std::fabs(p.split_ratio - 0.5) < 0.04, true}, {ID_SPLIT_67, "2/3", std::fabs(p.split_ratio - 0.67) < 0.04, true}});
        row("Camera safe zone", "The part of the picture that always stays visible when the camera is cropped",
            {{ID_ZONE_EDIT, "Edit", false, m.camera}, {ID_ZONE_RESET, "Clear", false, !p.roi.full()}});
        row("Appearance", "Auto follows the vehicle's day and night mode", {{ID_THEME_AUTO, "Auto", p.theme == 0, true}, {ID_THEME_DARK, "Dark", p.theme == 1, true}, {ID_THEME_LIGHT, "Light", p.theme == 2, true}});
        foot_left_ = m.status; foot_right_ = m.build;
    } else {
        section("OVERRIDES (SENT TO THE PHONE)");
        row("Gear", "", {{ID_DBG_GEAR_T, "Vehicle", p.dbg_gear == 0, true}, {ID_DBG_GEAR_P, "Park", p.dbg_gear == 1, true}, {ID_DBG_GEAR_R, "Reverse", p.dbg_gear == 2, true}, {ID_DBG_GEAR_D, "Drive", p.dbg_gear == 3, true}});
#ifdef AA_BRIDGE_DEBUG_SENSORS
        row("Driving status", "", {{ID_DBG_DRV_T, "Vehicle", p.dbg_driving == 0, true}, {ID_DBG_DRV_PARKED, "Parked", p.dbg_driving == 1, true}, {ID_DBG_DRV_MOVING, "Moving", p.dbg_driving == 2, true}});
#endif
        row("Night mode", "", {{ID_DBG_NIGHT_T, "Vehicle", p.dbg_night == 0, true}, {ID_DBG_NIGHT_DAY, "Day", p.dbg_night == 1, true}, {ID_DBG_NIGHT_NIGHT, "Night", p.dbg_night == 2, true}});
        section("EXPERIMENTS");
        row("Split divider drag", "Drag the line between the panes", {{ID_DBG_SPLITDRAG_OFF, "Off", !p.split_drag, true}, {ID_DBG_SPLITDRAG_ON, "On", p.split_drag, true}});
        section("VEHICLE REPORTS");
        readout_ = {ix, y, iw, S(24) * 3 + S(20)}; y += readout_.h + S(8);
        section("RECOVERY");
        {   // Reboot Pi | Restart bridge, right-aligned; first tap arms ("Tap again" for 3 s), second fires
            int right = ix + iw;
            std::vector<std::pair<int, std::string>> bs = {{ID_DBG_RESTART, armed(ID_DBG_RESTART) ? "Tap again" : "Restart bridge"}, {ID_DBG_REBOOT, armed(ID_DBG_REBOOT) ? "Tap again" : "Reboot Pi"}};
            for (auto& b : bs) { const int w = std::max(S(120), tmp.textWidth(b.second, spx) + S(36)); right -= w; add(b.first, {right, y, w, sh}, b.second, spx, armed(b.first), true, ICON_NONE, ST_SEGMENT); right -= S(8); }
            y += sh + S(8);
        }
        foot_left_ = "Tap a recovery button twice within 3 s. Android Auto returns by itself."; foot_right_ = m.build;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------------------------------------------------------
// All icons are drawn from fractions of a square box, so one routine serves the 36 px window buttons and the 60 px pill buttons alike.
void Menu::drawIcon(Canvas& c, int icon, const Rect& area, Color fg, double alpha) {
    const int u = std::min(area.w, area.h);
    const double ox = area.x + (area.w - u) / 2.0, oy = area.y + (area.h - u) / 2.0;
    auto X = [&](double f) { return ox + f * u; };
    auto Y = [&](double f) { return oy + f * u; };
    auto R = [&](double fx, double fy, double fw, double fh) { return Rect{(int)std::lround(X(fx)), (int)std::lround(Y(fy)), std::max(1, (int)std::lround(fw * u)), std::max(1, (int)std::lround(fh * u))}; };
    const Color col = fg.withAlpha(alpha);
    const double t = std::max(2.0, u * 0.085);
    {   // the icon set (Material Design Icons, alpha masks): everything but the Android Auto mark
        const char* nm = icon == ICON_CAM ? "videocam" : icon == ICON_PIP ? "picture_in_picture_alt" : icon == ICON_SPLIT ? "splitscreen" : icon == ICON_GEAR ? "settings"
            : icon == ICON_MAX ? "fullscreen" : icon == ICON_CLOSE ? "close" : icon == ICON_FIT ? "fit_screen" : icon == ICON_FILL ? "zoom_out_map" : icon == ICON_MIRROR ? "flip"
            : icon == ICON_FLIP ? "flip_vertical" : icon == ICON_RESET ? "settings_backup_restore" : icon == ICON_ZONE ? "crop" : icon == ICON_PRECOND ? "battery_charging_full" : nullptr;
        if (const IconMask* mk = nm ? iconMask(nm) : nullptr) {
            const int side = (int)std::lround(u * (icon == ICON_CLOSE ? 0.78 : 0.92));   // the 24-px grid has its own padding; the X is drawn a little smaller
            c.drawMask(mk->a, mk->w, mk->h, {(int)std::lround(ox + (u - side) / 2.0), (int)std::lround(oy + (u - side) / 2.0), side, side}, col);
            return;
        }
    }
    switch (icon) {
    case ICON_AA:      // navigation arrow (the Android Auto mark)
        c.fillTriangle(X(0.5), Y(0.14), X(0.16), Y(0.84), X(0.5), Y(0.66), col);
        c.fillTriangle(X(0.5), Y(0.14), X(0.84), Y(0.84), X(0.5), Y(0.66), col.withAlpha(alpha * 0.72));
        break;
    case ICON_CAM:
        c.fillRoundRect(R(0.10, 0.30, 0.54, 0.40), u * 0.07, col);
        c.fillTriangle(X(0.68), Y(0.50), X(0.90), Y(0.32), X(0.90), Y(0.68), col);
        break;
    case ICON_PIP:
        c.strokeRoundRect(R(0.10, 0.22, 0.80, 0.56), u * 0.08, t, col);
        c.fillRoundRect(R(0.50, 0.46, 0.32, 0.24), u * 0.04, col);
        break;
    case ICON_SPLIT:   // stacked: phone on top, camera below
        c.strokeRoundRect(R(0.10, 0.18, 0.80, 0.64), u * 0.08, t, col);
        c.fillRoundRect(R(0.16, 0.485, 0.68, 0.035), 1, col);
        break;
    case ICON_GEAR: {  // cog: a ring (outer 0.32, hole 0.16) with eight SQUARE teeth (rotated rectangles) outside it
        const double cx = X(0.5), cy = Y(0.5), ro = 0.32 * u, ri = 0.16 * u;
        for (int k = 0; k < 8; ++k) {
            const double a = k * M_PI / 4 + M_PI / 8, dx = std::cos(a), dy = std::sin(a), px = -dy, py = dx, r0 = 0.24 * u, r1 = 0.45 * u, hw = 0.085 * u;
            const double ax = cx + dx * r0 + px * hw, ay = cy + dy * r0 + py * hw, bx = cx + dx * r0 - px * hw, by = cy + dy * r0 - py * hw;
            const double ex = cx + dx * r1 + px * hw, ey = cy + dy * r1 + py * hw, fx = cx + dx * r1 - px * hw, fy = cy + dy * r1 - py * hw;
            c.fillTriangle(ax, ay, bx, by, ex, ey, col); c.fillTriangle(bx, by, fx, fy, ex, ey, col);
        }
        c.strokeRoundRect({(int)std::lround(cx - ro), (int)std::lround(cy - ro), (int)std::lround(2 * ro), (int)std::lround(2 * ro)}, ro, ro - ri, col);   // stroke lies inside the rect
        break; }
    case ICON_MAX: {   // four corner brackets
        const double a = 0.20, b = 0.46, k = 0.08;
        for (int sx = 0; sx < 2; ++sx) for (int sy = 0; sy < 2; ++sy) {
            const double x0 = sx ? 1 - a : a, y0 = sy ? 1 - a : a, dx = sx ? -1 : 1, dy = sy ? -1 : 1;
            const double lx = (b - a) * dx, ly = (b - a) * dy;
            c.fillRoundRect(R(std::min(x0, x0 + lx), sy ? y0 - k : y0, std::abs(lx), k), 1, col);
            c.fillRoundRect(R(sx ? x0 - k : x0, std::min(y0, y0 + ly), k, std::abs(ly)), 1, col);
        }
        break; }
    case ICON_CLOSE:
        c.line(X(0.28), Y(0.28), X(0.72), Y(0.72), t * 1.25, col);
        c.line(X(0.72), Y(0.28), X(0.28), Y(0.72), t * 1.25, col);
        break;
    case ICON_FIT:     // whole picture inside the window
        c.strokeRoundRect(R(0.08, 0.20, 0.84, 0.60), u * 0.06, t, col);
        c.fillRoundRect(R(0.30, 0.34, 0.40, 0.32), u * 0.04, col);
        break;
    case ICON_FILL:    // picture covers the window: frame with arrows pushing out of all four corners
        c.strokeRoundRect(R(0.22, 0.26, 0.56, 0.48), u * 0.06, t, col);
        for (int sx = 0; sx < 2; ++sx) for (int sy = 0; sy < 2; ++sy) {
            const double x0 = sx ? 0.74 : 0.26, y0 = sy ? 0.70 : 0.30, x1 = sx ? 0.92 : 0.08, y1 = sy ? 0.92 : 0.08;
            c.line(X(x0), Y(y0), X(x1), Y(y1), t, col);
            c.line(X(x1), Y(y1), X(sx ? x1 - 0.12 : x1 + 0.12), Y(y1), t, col); c.line(X(x1), Y(y1), X(x1), Y(sy ? y1 - 0.12 : y1 + 0.12), t, col);
        }
        break;
    case ICON_RESET: { // circular arrow (back to the start)
        const double cx = X(0.5), cy = Y(0.52), r = 0.30 * u;
        for (int k = 0; k < 26; ++k) { const double a0 = -2.0 + k * 0.19, a1 = a0 + 0.2; c.line(cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r, cy + std::sin(a1) * r, t, col); }
        c.fillTriangle(cx + std::cos(-2.0) * r - 0.13 * u, cy + std::sin(-2.0) * r - 0.02 * u, cx + std::cos(-2.0) * r + 0.10 * u, cy + std::sin(-2.0) * r - 0.14 * u, cx + std::cos(-2.0) * r + 0.06 * u, cy + std::sin(-2.0) * r + 0.12 * u, col);
        break; }
    case ICON_MIRROR:  // left-right
        c.fillTriangle(X(0.10), Y(0.28), X(0.10), Y(0.72), X(0.42), Y(0.50), col);
        c.fillTriangle(X(0.90), Y(0.28), X(0.90), Y(0.72), X(0.58), Y(0.50), col.withAlpha(alpha * 0.5));
        for (int k = 0; k < 4; ++k) c.fillRoundRect(R(0.485, 0.12 + k * 0.24, 0.03, 0.14), 1, col);
        break;
    case ICON_FLIP:    // up-down
        c.fillTriangle(X(0.28), Y(0.10), X(0.72), Y(0.10), X(0.50), Y(0.42), col);
        c.fillTriangle(X(0.28), Y(0.90), X(0.72), Y(0.90), X(0.50), Y(0.58), col.withAlpha(alpha * 0.5));
        for (int k = 0; k < 4; ++k) c.fillRoundRect(R(0.12 + k * 0.24, 0.485, 0.14, 0.03), 1, col);
        break;
    case ICON_ZOOM: {  // magnifier
        const double cx = X(0.43), cy = Y(0.43), r = 0.26 * u;
        c.strokeRoundRect({(int)(cx - r), (int)(cy - r), (int)(2 * r), (int)(2 * r)}, r, t * 1.1, col);
        c.line(X(0.63), Y(0.63), X(0.88), Y(0.88), t * 1.3, col);
        break; }
    case ICON_ENERGY: {    // lightning bolt (two triangles), hand-drawn: no Material mask in the icon set
        c.fillTriangle(X(0.58), Y(0.10), X(0.26), Y(0.56), X(0.50), Y(0.56), col);
        c.fillTriangle(X(0.42), Y(0.90), X(0.74), Y(0.44), X(0.50), Y(0.44), col);
        c.fillTriangle(X(0.26), Y(0.56), X(0.50), Y(0.56), X(0.50), Y(0.44), col);
        c.fillTriangle(X(0.50), Y(0.44), X(0.74), Y(0.44), X(0.50), Y(0.56), col);
        break; }
    case ICON_PRECOND: {   // battery with a bolt
        c.strokeRoundRect(R(0.10, 0.28, 0.72, 0.44), u * 0.08, t, col);
        c.fillRoundRect(R(0.84, 0.42, 0.07, 0.16), u * 0.02, col);
        c.fillTriangle(X(0.56), Y(0.33), X(0.31), Y(0.55), X(0.50), Y(0.55), col);
        c.fillTriangle(X(0.44), Y(0.45), X(0.66), Y(0.45), X(0.40), Y(0.68), col);
        break; }
    case ICON_ZONE: {  // crop frame with corner handles
        c.strokeRoundRect(R(0.20, 0.24, 0.60, 0.52), u * 0.02, std::max(1.5, t * 0.6), col);
        for (int sx = 0; sx < 2; ++sx) for (int sy = 0; sy < 2; ++sy) c.fillRoundRect(R(sx ? 0.74 : 0.14, sy ? 0.70 : 0.18, 0.12, 0.12), u * 0.02, col);
        break; }
    default: break;
    }
}

void Menu::drawWidget(Canvas& c, const Widget& w) {
    if (w.id == ID_NONE || w.id == ID_ZONE_CANVAS) return;                    // invisible touch absorbers
    if (w.id == ID_SPLIT_HANDLE) {                                            // the divider grip: a short bar on the pane boundary (the hit band is much taller)
        const int gw = S(96), gh = S(8), cy = w.r.y + w.r.h / 2;
        c.fillRoundRect({(w.r.x + w.r.w - gw) / 2 - S(6), cy - gh / 2 - S(6), gw + S(12), gh + S(12)}, S(10), kPaneBg);
        c.fillRoundRect({(w.r.x + w.r.w - gw) / 2, cy - gh / 2, gw, gh}, gh / 2, w.selected ? kAccent : kText.withAlpha(0.85));
        return;
    }
    const bool pressed = pressed_ == w.id;
    switch (w.style) {
    case ST_PILL: {
        const bool warm = w.id == ID_PRECOND && w.selected;            // preconditioning ON: solid warm fill, impossible to miss; OFF: plain dim icon
        Color bg = warm ? kWarm : w.selected ? kAccent : pressed ? kPillSel : Color{0, 0, 0, 0};
        if (bg.a) c.fillRoundRect(w.r, S(16), bg);
        drawIcon(c, w.icon, {w.r.x + S(10), w.r.y + S(8), w.r.w - S(20), w.r.h - S(16)}, warm ? Color{20, 12, 0, 255} : w.selected ? Color{255, 255, 255, 255} : kIcon, w.enabled ? (w.id == ID_PRECOND && !w.selected ? 0.8 : 1.0) : 0.4);
        break; }
    case ST_PANE: {
        c.fillRoundRect(w.r, S(10), pressed ? Color{46, 52, 66, 225} : kPaneBg);
        drawIcon(c, w.icon, {w.r.x + w.r.w / 8, w.r.y + w.r.h / 8, w.r.w * 3 / 4, w.r.h * 3 / 4}, kText, 1.0);
        break; }
    case ST_CHIP: {
        Color bg = !w.enabled ? kBtnOff.withAlpha(0.85) : w.selected ? kAccent : pressed ? kBtnPress : kChip;
        const Color fg = w.selected ? Color{255, 255, 255, 255} : w.enabled ? kText : kOff;
        c.fillRoundRect(w.r, S(16), bg);
        c.strokeRoundRect(w.r, S(16), std::max(1, S(2)), w.selected ? kAccent : kPillBorder);
        const int ih = w.r.h * 54 / 100;
        drawIcon(c, w.icon, {w.r.x, w.r.y + S(6), w.r.w, ih}, fg, 1.0);
        c.textCentered({w.r.x, w.r.y + w.r.h - S(26), w.r.w, S(22)}, w.label, w.px, fg);
        if (w.id == ID_CAM_FIT || w.id == ID_CAM_MIRROR || w.id == ID_CAM_FLIP)        // toggles carry a state dot: lit when on
            c.fillRoundRect({w.r.x + w.r.w - S(16), w.r.y + S(8), S(8), S(8)}, S(4), w.selected ? Color{255, 255, 255, 255} : kDim.withAlpha(0.6));
        break; }
    default: {
        Color bg = !w.enabled ? kBtnOff : w.selected ? kAccent : (pressed ? kBtnPress : kBtn);
        Color fg = !w.enabled ? kOff : kText;
        c.fillRoundRect(w.r, w.style == ST_SEGMENT ? S(12) : S(18), bg);
        if (w.icon != ICON_NONE) drawIcon(c, w.icon, {w.r.x + w.r.w / 5, w.r.y + w.r.h / 5, w.r.w * 3 / 5, w.r.h * 3 / 5}, fg, 1.0);
        else c.textCentered(w.r, w.label, w.px, fg);
    } }
}

// Panes that are wanted by the layout but have no live source show their artwork; live panes above them punch a hole (the overlay sits on top of video).
// ---------------------------------------------------------------------------------------------------------------------------------------------
// Energy page (Mode::Energy, beta): opaque, device-owned. Two large readouts on top (live power, 30 s efficiency), then a fixed-type list of rows in
// priority order; the list simply shows as many rows as fit (a smaller window later = fewer rows, same type size). Stale values are dashes.
void Menu::drawEnergyPage(Canvas& c, const Model& m) {
    c.fillRoundRect({0, 0, W_, H_}, 0, Color{0, 0, 0, 255});
    const int stripw = handleRect(W_, H_).w + S(16);                    // keep clear of the right-edge pill
    const int mg = S(28), cw = W_ - stripw - 2 * mg;
    int y = S(24);
    // two big tiles
    const int th = S(150), tw = (cw - S(16)) / 2;
    auto tile = [&](int x, const std::string& big, const std::string& label, bool accent) {
        c.fillRoundRect({x, y, tw, th}, S(18), kPaneBg);
        c.text(x + S(20), y + S(26), label, S(22), kDim);
        int px = S(84); while (px > S(36) && c.textWidth(big, px) > tw - S(40)) px -= S(4);   // "+12.3 kW" during regen ran off the tile at one fixed fallback size (ui62a)
        c.text(x + S(20), y + th - S(58), big, px, accent ? kAccent : kText);
    };
    tile(mg, m.energy_big_a, m.energy_big_a_label, false);
    tile(mg + tw + S(16), m.energy_big_b, m.energy_big_b_label, m.energy_big_b == "regen");
    y += th + S(18);
    // rows: label left, value+unit right, in two columns when the width allows
    const int rh = S(50), cols = cw >= S(820) ? 2 : 1, colw = (cw - (cols - 1) * S(24)) / cols;
    int col = 0, rowy = y;
    for (const auto& r : m.energy_rows) {
        if (rowy + rh > H_ - S(56)) break;                               // what does not fit is simply not shown
        const int x = mg + col * (colw + S(24));
        c.fillRoundRect({x, rowy + rh - std::max(1, S(1)), colw, std::max(1, S(1))}, 0, kPillBorder);
        c.text(x, rowy + rh / 2, r.label, S(24), kDim);
        const Color vc = r.level < 0 ? kOff : r.level == 1 ? kAccent : r.level == 2 ? kWarm : kText;
        const std::string v = r.level < 0 ? std::string("\xe2\x80\x94") : r.value;        // em dash for stale
        const int uw = r.unit.empty() || r.level < 0 ? 0 : c.textWidth(r.unit, S(18)) + S(6), vw = c.textWidth(v, S(28));
        c.text(x + colw - uw - vw, rowy + rh / 2, v, S(28), vc);
        if (uw) c.text(x + colw - uw + S(6), rowy + rh / 2 + S(3), r.unit, S(18), kDim);
        if (++col >= cols) { col = 0; rowy += rh + S(6); }
    }
    c.text(mg, H_ - S(28), m.energy_link, S(18), kDim);
    if (!m.build.empty()) { const int bw = c.textWidth(m.build, S(18)); c.text(mg + cw - bw, H_ - S(28), m.build, S(18), kDim); }
}

void Menu::drawPlaceholders(Canvas& c, const Model& m) {
    const Layout& L = m.layout;
    auto paint = [&](const Pane& p, Art a, int radius) {
        if (!p.visible || p.live || p.rect.empty()) return;
        const std::vector<uint8_t>& px = placeholderCover(a, p.rect.w, p.rect.h, radius);
        c.drawImage(px.data(), p.rect.w, p.rect.h, p.rect.x, p.rect.y);
    };
    if (m.split_dragging) return;   // divider drag: nothing is painted for the panes (both video layers are hidden, the plain base shows through); the overlay carries only the
                                    // divider band, so the per-step layer convert and the per-frame blend stay tiny. Art/icons here cost a full-window layer per step (ui49: still laggy).
    // Z-order is phone (bottom) < camera (top). The overlay sits above BOTH video layers, so: phone art first, then a hole where a LIVE camera window
    // is (it must not be covered by the phone art), then the camera art. A live phone needs no hole: nothing is painted for it, and the camera art above stays.
    if (!m.base_art) {                                  // base_art: the splash is video underneath, so there is no art to paint and no hole to keep in step with the PiP
        paint(L.phone, Art::AndroidAuto, 0);
        if (L.camera.live && L.camera.visible) c.clearRect(L.camera.rect);
    }
    if (m.pip_corners && !m.pip_dragging && m.mode == Mode::Pip && L.camera.live && L.camera.visible && L.camera_radius > 1 && !L.camera.rect.empty()) {
        // Over the splash (base layer, phone not live) the corners show the splash itself; over a live phone picture we do not know the pixels underneath, so a dark fill is used.
        const bool splash = m.base_art && L.phone.visible && !L.phone.live && !L.phone.rect.empty();
        const std::vector<uint8_t>* art = splash ? &placeholderCover(Art::AndroidAuto, L.phone.rect.w, L.phone.rect.h, 0) : nullptr;
        if (splash || !m.corner_restore || !L.phone.live)     // over a live phone the video path puts the phone's own pixels back (corner_restore); otherwise paint the cover
            c.fillRoundCorners(L.camera.rect, L.camera_radius, art ? art->data() : nullptr, splash ? L.phone.rect : Rect{0, 0, 0, 0}, Color{12, 14, 18, 255});
    }
    if (!(L.camera.live && L.camera.visible)) paint(L.camera, Art::Camera, m.mode == Mode::Pip ? L.camera_radius : 0);   // same corner radius as the live picture
}

void Menu::render(Canvas& c, const Model& m, int64_t now) {
    now_ = now;
    P_ = m.light ? &kLight : &kDark;
    c.clear();
    build(m, now);

    if (screen_ == Screen::Settings || screen_ == Screen::Debug) c.fillRoundRect({0, 0, W_, H_}, 0, Color{0, 0, 0, 255});   // opaque device-owned page
    else if (m.mode == Mode::Energy && screen_ != Screen::ZoneEdit) drawEnergyPage(c, m);                                    // opaque data page under the pill
    else if (screen_ != Screen::ZoneEdit) drawPlaceholders(c, m);

    if (screen_ == Screen::Hidden || screen_ == Screen::Home) {
        const Layout& L = m.layout;
        for (const auto& w : widgets_) if (w.style == ST_PANE || w.style == ST_CHIP || w.id == ID_SPLIT_HANDLE) drawWidget(c, w);
        if (m.split_dragging && m.split_edge != 0 && !L.split_handle.empty()) {        // edge zone hint while dragging: inside the divider band, left of the grip
            const std::string hint = m.split_edge < 0 ? "Full camera" : "Full Android Auto";                    // short: it must fit left of the grip inside the band
            const Rect b = L.split_handle; const int hh = std::min(b.h, S(40)), hw = c.textWidth(hint, S(20)) + S(32), hx = std::max(b.x + S(16), (W_ - S(96)) / 2 - S(28) - hw), hy = b.y + (b.h - hh) / 2;
            c.fillRoundRect({hx, hy, hw, hh}, hh / 2, kAccent); c.textCentered({hx, hy, hw, hh}, hint, S(20), Color{255, 255, 255, 255});
        }
        if (now - m.mode_changed_ms < kAwakeMs && !m.split_dragging && (screen_ == Screen::Home || awakeAt(now))) {   // mode toast: what you just switched to
            const char* name = m.mode == Mode::FullAA ? "Android Auto" : m.mode == Mode::FullCam ? "Camera" : m.mode == Mode::Pip ? "Camera window" : m.mode == Mode::Energy ? "Energy" : "Split";
            const int ic = m.mode == Mode::FullAA ? ICON_AA : m.mode == Mode::FullCam ? ICON_CAM : m.mode == Mode::Pip ? ICON_PIP : m.mode == Mode::Energy ? ICON_ENERGY : ICON_SPLIT;
            const int th = S(44), tw = c.textWidth(name, S(22)) + S(70), tx = (W_ - handleRect(W_, H_).w - tw) / 2;
            Rect tr{tx, H_ - S(90) - th, tw, th};                                                  // bottom centre; over a camera window -> top centre; still covered -> no toast
            if (L.camera.visible && tr.intersects(L.camera.rect)) tr.y = S(40);
            if (!(L.camera.visible && tr.intersects(L.camera.rect))) {
                c.fillRoundRect(tr, th / 2, kPillBg); c.strokeRoundRect(tr, th / 2, std::max(1, S(2)), kPillBorder);
                drawIcon(c, ic, {tr.x + S(12), tr.y + S(8), th - S(16), th - S(16)}, kText, 1.0);
                c.text(tr.x + S(48), tr.y + th / 2, name, S(22), kText);
            }
        }
        if (pillOpen(m) && !m.split_dragging) {
            const Rect R = pill_;
            c.fillRoundRect({R.x, R.y, R.w + S(30), R.h}, S(28), kPillBg);
            c.strokeRoundRect({R.x, R.y, R.w + S(30), R.h}, S(28), std::max(1, S(2)), kPillBorder);
            for (const auto& d : pill_divs_) c.fillRoundRect(d, 1, kPillBorder);
            for (const auto& w : widgets_) if (w.style == ST_PILL) drawWidget(c, w);
        }
        last_awake_ = awakeAt(now) || screen_ == Screen::Home;
        return;
    }
    if (screen_ == Screen::Settings || screen_ == Screen::Debug) {
        c.fillRoundRect(card_, S(30), kPanel);
        c.text(card_.x + S(90), card_.y + S(14) + S(28), screen_ == Screen::Settings ? "Settings" : "Debug", S(36), kText);
        const int ix = card_.x + S(30), iw = card_.w - 2 * S(30);
        for (size_t i = 0; i < rows_.size(); ++i) {
            const RowSpec& r = rows_[i];
            if (i) c.fillRoundRect({ix, r.y, iw, std::max(1, S(1))}, 0, P_->line);                    // separator between rows
            const bool hint2 = !r.hint.empty() && c.textWidth(r.hint, S(15)) > (r.wide ? iw : S(330));
            const int textH = S(26) + (r.hint.empty() ? 0 : (hint2 ? 2 : 1) * S(20));
            const int ty = r.wide ? r.y + S(4) : r.y + (r.h - textH) / 2;
            c.text(ix, ty + S(13), r.label, S(22), kText);
            if (!r.hint.empty()) {
                if (!hint2) c.text(ix, ty + S(26) + S(10), r.hint, S(15), kDim);
                else {   // wrap once at the last space that fits
                    const int maxw = r.wide ? iw : S(330); size_t cut = r.hint.size();
                    while (cut > 0 && c.textWidth(r.hint.substr(0, cut), S(15)) > maxw) { cut = r.hint.rfind(' ', cut - 1); if (cut == std::string::npos) { cut = 0; break; } }
                    c.text(ix, ty + S(26) + S(10), r.hint.substr(0, cut), S(15), kDim);
                    c.text(ix, ty + S(26) + S(30), cut < r.hint.size() ? r.hint.substr(cut + 1) : "", S(15), kDim);
                }
            }
        }
        for (const auto& s : sections_) c.text(s.first.x, s.first.y + s.first.h / 2, s.second, S(16), kDim);
        if (!readout_.empty()) {
            c.fillRoundRect(readout_, S(14), P_->readout);
            c.text(readout_.x + S(18), readout_.y + S(12) + S(12), m.truck_line1.empty() ? "no sensor data yet" : m.truck_line1, S(19), kText);
            c.text(readout_.x + S(18), readout_.y + S(12) + S(36), m.truck_line2, S(19), kText);
            c.text(readout_.x + S(18), readout_.y + S(12) + S(60), m.info_line, S(19), kText);
        }
        {   // footer: status dots (Settings) or the recovery hint (Debug), build tag on the right
            const int fy = card_.y + card_.h - S(36);
            c.fillRoundRect({ix, fy - S(16), iw, std::max(1, S(1))}, 0, P_->line);
            if (screen_ == Screen::Settings) {
                int x = ix;
                struct D { bool ok; std::string t; } ds[3] = {{m.phone, m.phone ? "Phone connected" : "No phone"}, {m.camera, m.camera ? "Camera OK" : "No camera"}, {true, "Vehicle"}};
                for (const auto& d : ds) { c.fillRoundRect({x, fy - S(5), S(10), S(10)}, S(5), d.ok ? Color{56, 193, 114, 255} : Color{240, 173, 78, 255}); c.text(x + S(18), fy, d.t, S(16), kDim); x += S(18) + c.textWidth(d.t, S(16)) + S(26); }
            } else c.text(ix, fy, foot_left_, S(15), kDim);
            if (!foot_right_.empty()) c.text(ix + iw - c.textWidth(foot_right_, S(15)), fy, foot_right_, S(15), kDim);
        }
        for (const auto& w : widgets_) {
            if (w.id == ID_DBG_RESTART || w.id == ID_DBG_REBOOT) {                    // recovery buttons: "careful" tint unless armed (then the accent)
                Widget t = w; drawWidget(c, t);
                if (!w.selected) { c.fillRoundRect(w.r, S(12), P_->warnBg); c.textCentered(w.r, w.label, w.px, P_->warnFg); }
            } else drawWidget(c, w);
        }
        return;
    }
    // ZoneEdit
    const Rect v = m.cam_view;
    const Color dim{0, 0, 0, 150};
    int x0 = zx0_, y0 = zy0_, x1 = zx1_, y1 = zy1_;
    if (!zone_has_) { x0 = v.x; y0 = v.y; x1 = v.x + v.w; y1 = v.y + v.h; }
    c.fillRoundRect({v.x, v.y, v.w, std::max(0, y0 - v.y)}, 0, dim);
    c.fillRoundRect({v.x, y1, v.w, std::max(0, v.y + v.h - y1)}, 0, dim);
    c.fillRoundRect({v.x, y0, std::max(0, x0 - v.x), std::max(0, y1 - y0)}, 0, dim);
    c.fillRoundRect({x1, y0, std::max(0, v.x + v.w - x1), std::max(0, y1 - y0)}, 0, dim);
    c.strokeRoundRect({x0, y0, x1 - x0, y1 - y0}, 0, S(4), kAccent);
    for (int cx : {x0, x1}) for (int cy : {y0, y1}) c.fillRoundRect({cx - S(10), cy - S(10), S(20), S(20)}, S(10), kText);
    std::string hint = zone_has_ ? "Area to keep. Drag again to redraw." : "Drag on the picture to choose the area to keep";
    int hw = c.textWidth(hint, S(26)) + S(40);
    c.fillRoundRect({(W_ - hw) / 2, S(24), hw, S(52)}, S(14), kPanel);
    c.textCentered({(W_ - hw) / 2, S(24), hw, S(52)}, hint, S(26), kText);
    for (const auto& w : widgets_) drawWidget(c, w);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------------------------------------
Intent Menu::activate(int id, const Model& m) {
    Intent it;
    auto find = [&](int i) -> const Widget* { for (const auto& w : widgets_) if (w.id == i) return &w; return nullptr; };
    const Widget* w = find(id);
    if (w && !w->enabled) return it;
    pressed_ = ID_NONE;
    auto setMode = [&](Mode md) { it.kind = Intent::SetMode; it.i = (int)md; if (screen_ == Screen::Hidden || screen_ == Screen::Home) screen_ = Screen::Home; };   // the pill stays open a moment
    auto setStart = [&](Mode md) { it.kind = Intent::SetStart; it.i = (int)md; };
    switch (id) {
    case ID_HANDLE: screen_ = Screen::Home; break;
    case ID_SETTINGS: screen_ = Screen::Settings; break;
    case ID_PRECOND: it.kind = Intent::SetPrecond; it.i = m.precond ? 0 : 1; if (screen_ == Screen::Hidden) screen_ = Screen::Home; break;   // the pill stays open a moment so the new state is seen
    case ID_CLOSE: screen_ = Screen::Hidden; break;
    case ID_BACK: screen_ = (screen_ == Screen::Debug) ? Screen::Settings : Screen::Hidden; break;
    case ID_DEBUG: screen_ = Screen::Debug; it.kind = Intent::OpenDebug; break;
    case ID_MODE_AA: setMode(Mode::FullAA); break;
    case ID_MODE_CAM: setMode(Mode::FullCam); break;
    case ID_MODE_SPLIT: setMode(Mode::Split); break;
    case ID_MODE_PIP: setMode(Mode::Pip); break;
    case ID_MODE_ENERGY: setMode(Mode::Energy); break;
    case ID_PANE_MAX: it.kind = Intent::SetMode; it.i = (int)Mode::FullCam; break;
    case ID_PANE_CLOSE: it.kind = Intent::SetMode; it.i = (int)Mode::FullAA; break;       // close the camera window -> Android Auto
    case ID_PANE_PIP: it.kind = Intent::SetMode; it.i = (int)Mode::Pip; break;
    case ID_START_AA: setStart(Mode::FullAA); break;
    case ID_START_CAM: setStart(Mode::FullCam); break;
    case ID_START_SPLIT: setStart(Mode::Split); break;
    case ID_START_PIP: setStart(Mode::Pip); break;
    case ID_START_SETTINGS: it.kind = Intent::SetStartSettings; break;
    case ID_START_LAST: it.kind = Intent::SetStartLast; break;
    case ID_CORNER_TL: it.kind = Intent::SetCorner; it.i = (int)Corner::TL; break;
    case ID_CORNER_TR: it.kind = Intent::SetCorner; it.i = (int)Corner::TR; break;
    case ID_CORNER_BL: it.kind = Intent::SetCorner; it.i = (int)Corner::BL; break;
    case ID_CORNER_BR: it.kind = Intent::SetCorner; it.i = (int)Corner::BR; break;
    case ID_SIZE_S: it.kind = Intent::SetSize; it.i = (int)PipSize::S; break;
    case ID_SIZE_M: it.kind = Intent::SetSize; it.i = (int)PipSize::M; break;
    case ID_SIZE_L: it.kind = Intent::SetSize; it.i = (int)PipSize::L; break;
    case ID_SPLIT_169: it.kind = Intent::SetSplit; it.d = 0.0; break;
    case ID_SPLIT_33: it.kind = Intent::SetSplit; it.d = 0.33; break;
    case ID_SPLIT_50: it.kind = Intent::SetSplit; it.d = 0.50; break;
    case ID_SPLIT_67: it.kind = Intent::SetSplit; it.d = 0.67; break;
    case ID_REV_OFF: it.kind = Intent::SetAutoCam; it.i = 0; break;
    case ID_REV_ON: it.kind = Intent::SetAutoCam; it.i = 1; break;
    case ID_DBG_GEAR_T: case ID_DBG_GEAR_P: case ID_DBG_GEAR_R: case ID_DBG_GEAR_D: it.kind = Intent::SetDbgGear; it.i = id - ID_DBG_GEAR_T; break;
    case ID_DBG_DRV_T: case ID_DBG_DRV_PARKED: case ID_DBG_DRV_MOVING: it.kind = Intent::SetDbgDriving; it.i = id - ID_DBG_DRV_T; break;
    case ID_DBG_NIGHT_T: case ID_DBG_NIGHT_DAY: case ID_DBG_NIGHT_NIGHT: it.kind = Intent::SetDbgNight; it.i = id - ID_DBG_NIGHT_T; break;
    case ID_THEME_AUTO: case ID_THEME_DARK: case ID_THEME_LIGHT: it.kind = Intent::SetTheme; it.i = id - ID_THEME_AUTO; break;
    case ID_DBG_SPLITDRAG_OFF: case ID_DBG_SPLITDRAG_ON: it.kind = Intent::SetSplitDrag; it.i = id - ID_DBG_SPLITDRAG_OFF; break;
    case ID_DBG_RESTART: case ID_DBG_REBOOT:
        if (armed(id)) { it.kind = id == ID_DBG_RESTART ? Intent::SysRestart : Intent::SysReboot; arm_id_ = 0; }
        else { arm_id_ = id; arm_t_ = now_; }                       // first tap: arm (the label becomes "Tap again" for 3 s)
        break;
    case ID_CAM_FIT: it.kind = Intent::SetCamFill; it.i = m.prefs.cam_fill ? 0 : 1; break;
    case ID_CAM_MIRROR: it.kind = Intent::SetCamMirror; it.i = m.prefs.cam_mirror ? 0 : 1; break;
    case ID_CAM_FLIP: it.kind = Intent::SetCamFlip; it.i = m.prefs.cam_flip ? 0 : 1; break;
    case ID_CAM_RESET: it.kind = Intent::ResetCamView; break;
    case ID_ZONE_EDIT: case ID_CAM_ZONE: {
        zone_from_settings_ = (id == ID_ZONE_EDIT);
        screen_ = Screen::ZoneEdit; it.kind = Intent::BeginZoneEdit;
        const Rect& v = m.cam_view; const Roi& r = m.prefs.roi;
        zone_has_ = !r.full() && !v.empty();
        zx0_ = v.x + (int)(r.x * v.w); zy0_ = v.y + (int)(r.y * v.h);
        zx1_ = zx0_ + (int)(r.w * v.w); zy1_ = zy0_ + (int)(r.h * v.h);
        break; }
    case ID_ZONE_RESET:
        it.kind = Intent::ResetRoi; zone_has_ = false;
        if (screen_ == Screen::ZoneEdit) screen_ = zone_from_settings_ ? Screen::Settings : Screen::Hidden;
        break;
    case ID_ZONE_CANCEL: screen_ = zone_from_settings_ ? Screen::Settings : Screen::Hidden; it.kind = Intent::EndZoneEdit; zone_has_ = false; break;
    case ID_ZONE_SAVE: {
        const Rect& v = m.cam_view;
        if (!zone_has_ || v.empty()) break;
        it.kind = Intent::SaveRoi;
        it.roi = {double(zx0_ - v.x) / v.w, double(zy0_ - v.y) / v.h, double(zx1_ - zx0_) / v.w, double(zy1_ - zy0_) / v.h};
        it.roi.clamp();
        screen_ = zone_from_settings_ ? Screen::Settings : Screen::Hidden; zone_has_ = false;
        break; }
    default: break;
    }
    return it;
}

void Menu::zoneDrag(int x, int y, bool start, const Model& m) {
    const Rect& v = m.cam_view;
    if (v.empty()) return;
    x = std::clamp(x, v.x, v.x + v.w); y = std::clamp(y, v.y, v.y + v.h);
    if (start) { ax_ = x; ay_ = y; zx0_ = zx1_ = x; zy0_ = zy1_ = y; zone_has_ = false; return; }
    zx0_ = std::min(ax_, x); zx1_ = std::max(ax_, x); zy0_ = std::min(ay_, y); zy1_ = std::max(ay_, y);
    zone_has_ = (zx1_ - zx0_) >= S(24) && (zy1_ - zy0_) >= S(24);
}

bool Menu::tick(int64_t now) {
    bool changed = false;
    // The pill / window buttons disappear by themselves a few seconds after the last touch (the pill stays in the full-screen camera).
    // Settings / Debug / zone editor never time out.
    if (screen_ == Screen::Home && !fullPage(mode_) && now - last_activity_ > kAwakeMs) { screen_ = Screen::Hidden; changed = true; }
    if (screen_ == Screen::Hidden && !fullPage(mode_) && awakeAt(now) != last_awake_) changed = true;
    if (arm_id_ && now - arm_t_ >= 3000) { arm_id_ = 0; changed = true; }
    return changed;
}

}  // namespace ui
