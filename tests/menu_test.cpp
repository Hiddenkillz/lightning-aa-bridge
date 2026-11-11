// Host test for Menu: state machine + rendering.
// g++ -std=c++17 tests/menu_test.cpp src/ui/Menu.cpp src/ui/Gfx.cpp src/ui/FontData.cpp src/ui/Placeholder.cpp src/ui/PlaceholderData.cpp -o menu_test
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../src/ui/Menu.h"
using namespace ui;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static int W = 1280, H = 720;      // override: menu_test <w> <h>  (the Sync 4 window is 694x720 at 720p, 1042x1080 at 1080p)

static void backdrop(std::vector<uint8_t>& bg) {   // synthetic "Android Auto-ish" backdrop so translucency/legibility can be judged
    bg.assign((size_t)W * H * 3, 0);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        uint8_t* p = &bg[((size_t)y * W + x) * 3];
        p[0] = 30 + x * 40 / W; p[1] = 60 + y * 60 / H; p[2] = 110;
        if (((x / 160) + (y / 120)) % 2 == 0) { p[0] += 18; p[1] += 18; p[2] += 18; }
    }
}
static void save(const char* name, const Canvas& c, std::vector<uint8_t> bg) {
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        double a = c.data()[i * 4 + 3] / 255.0;
        for (int k = 0; k < 3; ++k) bg[i * 3 + k] = (uint8_t)(c.data()[i * 4 + k] * a + bg[i * 3 + k] * (1 - a));
    }
    std::string path = std::string("/tmp/menu-") + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", W, H); std::fwrite(bg.data(), 1, bg.size(), f); std::fclose(f);
}
static const UiButton* findBtn(const Menu& m, int id) { for (auto& b : m.buttons()) if (b.id == id) return &b; return nullptr; }
static uint8_t alphaAt(const Canvas& c, int x, int y) { return c.data()[((size_t)y * c.w() + x) * 4 + 3]; }
static Model mk(Mode mode, bool phone, bool cam, const Prefs& p = Prefs()) {
    Model m; m.mode = mode; m.prefs = p; m.phone = phone; m.camera = cam;
    m.layout = compute(mode, p, W, H, 640, 480, phone, cam);
    m.cam_view = compute(Mode::FullCam, Prefs(), W, H, 640, 480, false, true).camera.rect;
    return m;
}

int main(int argc, char** argv) {
    if (argc > 2) { W = atoi(argv[1]); H = atoi(argv[2]); }
    const double sc = H / 720.0;
    Canvas c(W, H);
    std::vector<uint8_t> bg; backdrop(bg);
    int64_t t = 1000;
    Menu m(W, H);

    // ---- live Android Auto, nobody touched the screen: NOTHING is drawn and nothing is hit-testable (no tab, no pill) ----
    Model aa = mk(Mode::FullAA, true, true);
    m.render(c, aa, t);
    CHECK(m.screen() == Screen::Hidden && !m.needsPanel(aa) && m.buttons().empty());
    CHECK(c.contentBounds().w == 0 || c.contentBounds().h == 0);                    // fully transparent
    save("aa-idle", c, bg);
    CHECK(!m.tick(t + 1000));                                                         // idle stays idle (no redraw churn)

    // ---- any touch anywhere wakes the pill (5 buttons, big targets, inside the right-edge strip); the touch itself still goes to the phone ----
    CHECK(m.noteActivity(t) == true);                                                 // it woke something that needs drawing
    m.render(c, aa, t);
    CHECK(m.pillOpen(aa) && findBtn(m, ID_MODE_AA) && findBtn(m, ID_MODE_CAM) && findBtn(m, ID_MODE_PIP) && findBtn(m, ID_MODE_SPLIT) && findBtn(m, ID_SETTINGS) && m.buttons().size() == 5);
    { Rect hr = handleRect(W, H); for (auto& b : m.buttons()) CHECK(b.rect.w >= 60 * sc - 1 && b.rect.h >= 56 * sc - 1 && b.rect.x >= hr.x && b.rect.y >= hr.y && b.rect.y + b.rect.h <= hr.y + hr.h); }
    CHECK(!m.needsPanel(aa));                                                         // still the cheap edge layer with live AA
    save("aa-pill", c, bg);
    CHECK(m.noteActivity(t + 500) == false);                                          // already awake: no redraw needed
    // touches outside the pill are NOT absorbed: the router sends them on to the phone
    { InputRouter r; r.configure(W, H, W, H, W, H); r.setButtons(m.buttons()); r.setLayout(aa.layout);
      auto hit = r.route(W / 2, H / 2, Action::Down); CHECK(hit.target == Target::Phone); r.route(W / 2, H / 2, Action::Up);
      Rect pb = findBtn(m, ID_MODE_CAM)->rect; hit = r.route(pb.x + 5, pb.y + 5, Action::Down); CHECK(hit.target == Target::Ui && hit.ui_id == ID_MODE_CAM); r.route(pb.x + 5, pb.y + 5, Action::Up); }
    // it disappears by itself a few seconds after the LAST touch (each touch restarts the timer)
    m.noteActivity(t + 2500);
    CHECK(!m.tick(t + 4500));                                                         // 2 s after the last touch: still there
    CHECK(m.tick(t + 7000));                                                          // 4.5 s after: time to redraw without it
    m.render(c, aa, t + 7000); CHECK(m.buttons().empty() && !m.pillOpen(aa));
    // choosing a mode keeps the pill for the usual few seconds, then it goes
    Intent it = m.activate(ID_MODE_SPLIT, aa); CHECK(it.kind == Intent::SetMode && it.i == (int)Mode::Split && m.screen() == Screen::Home);
    m.noteActivity(t + 8000); CHECK(!m.tick(t + 10000)); CHECK(m.tick(t + 13000) && m.screen() == Screen::Hidden);
    // the pill never closes by itself in the full-screen camera; there it is simply always there
    Model cam = mk(Mode::FullCam, true, true);
    m.hide(); m.render(c, cam, t + 100000);                    // nobody touched for ages
    CHECK(m.screen() == Screen::Hidden && m.pillOpen(cam) && findBtn(m, ID_MODE_AA) && !findBtn(m, ID_HANDLE));
    CHECK(!m.tick(t + 160000));
    CHECK(m.needsPanel(cam));
    // no mode is ever greyed out: all five buttons enabled with nothing connected
    { Model none = mk(Mode::FullAA, false, false); m.open(); m.render(c, none, t);
      for (int id : {ID_MODE_AA, ID_MODE_CAM, ID_MODE_PIP, ID_MODE_SPLIT, ID_SETTINGS}) CHECK(m.activate(id, none).kind != Intent::None || id == ID_SETTINGS);
      m.hide(); }

    // ---- placeholders: a wanted pane without a live source shows its art; live panes are left clear ----
    { Model na = mk(Mode::FullAA, false, true); Menu mm(W, H); mm.render(c, na, t);
      CHECK(mm.needsPanel(na) && alphaAt(c, W / 2, H / 2) == 255 && alphaAt(c, 3, 3) == 255);                                         // art covers the whole window
      save("aa-placeholder", c, bg);
      Model la = mk(Mode::FullAA, true, true); mm.render(c, la, t); CHECK(!mm.needsPanel(la) && alphaAt(c, W / 2, H / 2) == 0); }
    { Model nc = mk(Mode::FullCam, true, false); Menu mm(W, H); mm.render(c, nc, t);
      CHECK(alphaAt(c, W / 2, H / 2) == 255);                                                                                         // camera art in the (letterboxed) camera pane
      save("cam-placeholder", c, bg); }
    { Model pc = mk(Mode::Pip, false, true); Menu mm(W, H); mm.render(c, pc, t);                                                      // AA art behind a LIVE camera window
      Rect cr = pc.layout.camera.rect;
      CHECK(alphaAt(c, cr.x + cr.w / 2, cr.y + cr.h / 2) == 0 && alphaAt(c, 20, 20) == 255);                                           // hole where the live camera is
      save("pip-aa-placeholder", c, bg); }
    { Model pn = mk(Mode::Pip, true, false); Menu mm(W, H); mm.render(c, pn, t);                                                      // camera art in the PiP, live phone around it
      Rect cr = pn.layout.camera.rect;
      CHECK(alphaAt(c, cr.x + cr.w / 2, cr.y + cr.h / 2) == 255 && alphaAt(c, 20, 20) == 0 && alphaAt(c, cr.x, cr.y) < 255);        // rounded corner
      save("pip-cam-placeholder", c, bg); }
    { Model sp = mk(Mode::Split, false, false); Menu mm(W, H); mm.render(c, sp, t); save("split-both-placeholders", c, bg);
      Rect pr = sp.layout.phone.rect, cr = sp.layout.camera.rect;
      CHECK(alphaAt(c, pr.x + pr.w / 2, pr.y + pr.h / 2) == 255 && alphaAt(c, cr.x + cr.w / 2, cr.y + cr.h / 2) == 255); }

    // ---- window buttons inside the camera pane: PiP = Max, Close.  Split = PiP, Max, Close. The body takes no taps (router: Camera, ignored) ----
    { Model pip = mk(Mode::Pip, true, true); Menu mm(W, H); mm.render(c, pip, t + 50000); CHECK(!findBtn(mm, ID_PANE_MAX));   // asleep: no buttons
      mm.noteActivity(t); mm.render(c, pip, t); save("pip-live", c, bg);
      Rect cr = pip.layout.camera.rect;
      const UiButton* mx = findBtn(mm, ID_PANE_MAX); const UiButton* cl = findBtn(mm, ID_PANE_CLOSE);
      CHECK(mx && cl && !findBtn(mm, ID_PANE_PIP));
      CHECK(cl->rect.x + cl->rect.w <= cr.x + cr.w && cl->rect.y >= cr.y && mx->rect.x + mx->rect.w <= cl->rect.x && cl->rect.y + cl->rect.h <= cr.y + cr.h);
      CHECK(mm.activate(ID_PANE_MAX, pip).kind == Intent::SetMode && mm.activate(ID_PANE_MAX, pip).i == (int)Mode::FullCam);
      CHECK(mm.activate(ID_PANE_CLOSE, pip).i == (int)Mode::FullAA);                                                                // close -> full-screen Android Auto
      Model sp = mk(Mode::Split, true, true); mm.render(c, sp, t); save("split-live", c, bg);
      CHECK(!findBtn(mm, ID_PANE_PIP) && !findBtn(mm, ID_PANE_MAX) && !findBtn(mm, ID_PANE_CLOSE));                                // split has NO window buttons (user decision 2026-10-17)
      CHECK(!findBtn(mm, ID_SPLIT_HANDLE));                                                                                        // no grab band unless config split_drag
      { Model en = sp; en.split_drag_enabled = true; mm.render(c, en, t); CHECK(findBtn(mm, ID_SPLIT_HANDLE) && findBtn(mm, ID_SPLIT_HANDLE)->rect.w == W); }
      { Model dr = sp; dr.split_drag_enabled = true; dr.split_dragging = true; dr.split_edge = -1; mm.render(c, dr, t); save("split-dragging-top", c, bg);
        dr.split_edge = 0; dr.layout = compute(Mode::Split, dr.prefs, W, H, 1280, 720, true, true); mm.render(c, dr, t); save("split-dragging", c, bg); }
      { Model tm = sp; tm.mode_changed_ms = t; mm.noteActivity(t); mm.render(c, tm, t + 100); save("mode-toast", c, bg); }
      { // a big PiP in the bottom-right corner reaches up into the edge strip: its buttons must move to the left, clear of the tab
        Prefs bp; bp.pip_frac = 0.68; Model big = mk(Mode::Pip, true, true, bp); Menu mb(W, H); mb.noteActivity(t); mb.render(c, big, t); save("pip-big-buttons", c, bg);
        Rect strip = handleRect(W, H); Rect cr3 = big.layout.camera.rect;
        for (int id : {ID_PANE_MAX, ID_PANE_CLOSE}) { Rect b = findBtn(mb, id)->rect; CHECK(!strip.intersects(b) && b.x >= cr3.x && b.x + b.w <= cr3.x + cr3.w); }
        CHECK(findBtn(mb, ID_MODE_AA)); }                                           // the pill (awake) is there too, and the buttons dodge it
      Model pp = mk(Mode::Pip, false, false); mm.render(c, pp, t); CHECK(findBtn(mm, ID_PANE_MAX) && findBtn(mm, ID_PANE_CLOSE));   // also on a placeholder window
      Model ful = mk(Mode::FullAA, true, true); mm.render(c, ful, t); CHECK(!findBtn(mm, ID_PANE_MAX)); }

    // ---- full-screen camera bar: Fit/Fill, Mirror, Flip, Reset (zoomed only), Zone (live camera only) ----
    { Prefs p; Model fc = mk(Mode::FullCam, true, true, p); Menu mm(W, H); mm.render(c, fc, t); save("cam-bar", c, bg);
      for (int id : {ID_CAM_FIT, ID_CAM_MIRROR, ID_CAM_FLIP, ID_CAM_RESET, ID_CAM_ZONE}) CHECK(findBtn(mm, id));
      { Rect b = findBtn(mm, ID_CAM_FIT)->rect; CHECK(b.y + b.h <= H && b.h >= 60 * sc && b.x >= 0); }
      it = mm.activate(ID_CAM_FIT, fc); CHECK(it.kind == Intent::SetCamFill && it.i == 1);
      it = mm.activate(ID_CAM_MIRROR, fc); CHECK(it.kind == Intent::SetCamMirror && it.i == 1);
      it = mm.activate(ID_CAM_FLIP, fc); CHECK(it.kind == Intent::SetCamFlip && it.i == 1);
      CHECK(mm.activate(ID_CAM_RESET, fc).kind == Intent::None);                                                                   // not zoomed -> disabled
      p.cam_fill = p.cam_mirror = p.cam_flip = true; p.cam_zoom = 2.5; Model on = mk(Mode::FullCam, true, true, p); mm.render(c, on, t); save("cam-bar-on", c, bg);
      CHECK(mm.activate(ID_CAM_FIT, on).i == 0 && mm.activate(ID_CAM_MIRROR, on).i == 0 && mm.activate(ID_CAM_FLIP, on).i == 0);   // toggles go back
      CHECK(mm.activate(ID_CAM_RESET, on).kind == Intent::ResetCamView);
      Model off = mk(Mode::FullCam, true, false); mm.render(c, off, t); CHECK(mm.activate(ID_CAM_ZONE, off).kind == Intent::None);   // no live camera: no zone editing
      mm.render(c, fc, t); it = mm.activate(ID_CAM_ZONE, fc); CHECK(it.kind == Intent::BeginZoneEdit && mm.zoneEditing()); }

    // ---- staged preconditioning toggle: 6 pill buttons all inside the strip; OFF and ON must look clearly different; a tap asks for the opposite state ----
    {   Menu pm(W, H); Model pa = mk(Mode::FullAA, true, true); pa.precond_shown = true;
        pm.noteActivity(t); pm.render(c, pa, t);
        CHECK(pm.buttons().size() == 6 && findBtn(pm, ID_PRECOND));
        { Rect hr = handleRect(W, H); for (auto& b : pm.buttons()) CHECK(b.rect.w >= 60 * sc - 1 && b.rect.h >= 46 * sc - 1 && b.rect.x >= hr.x && b.rect.y >= hr.y && b.rect.y + b.rect.h <= hr.y + hr.h); }
        save("pill-precond-off", c, bg);
        Rect pr = findBtn(pm, ID_PRECOND)->rect; const int cx = pr.x + pr.w / 2, cy = pr.y + pr.h - 3;
        std::vector<uint8_t> offPx(c.data() + ((size_t)cy * c.w() + cx) * 4, c.data() + ((size_t)cy * c.w() + cx) * 4 + 4);
        Intent pi = pm.activate(ID_PRECOND, pa); CHECK(pi.kind == Intent::SetPrecond && pi.i == 1);
        pa.precond = 1; pm.render(c, pa, t); save("pill-precond-on", c, bg);
        const uint8_t* onPx = c.data() + ((size_t)cy * c.w() + cx) * 4;
        CHECK(onPx[3] == 255 && onPx[0] > 200 && onPx[2] < 60);                                    // solid orange fill when ON
        CHECK(!(offPx[3] == 255 && offPx[0] > 200 && offPx[2] < 60));                              // not orange when OFF
        pi = pm.activate(ID_PRECOND, pa); CHECK(pi.kind == Intent::SetPrecond && pi.i == 0);
        Model p5 = mk(Mode::FullAA, true, true); pm.render(c, p5, t); CHECK(pm.buttons().size() == 5 && !findBtn(pm, ID_PRECOND));   // feature off -> exactly the old pill
    }

    // ---- Settings: opaque page, X left, Debug right, all five start screens, no auto-hide, no driving lock ----
    Model md = mk(Mode::Pip, true, true); md.moving = true;
    m.hide(); m.open(); m.render(c, md, t);
    it = m.activate(ID_SETTINGS, md); CHECK(m.screen() == Screen::Settings);
    md.prefs.start_mode = Mode::Pip; md.prefs.pip_corner = Corner::BR; md.prefs.pip_size = PipSize::M; md.prefs.split_ratio = 0.5;
    m.render(c, md, t); save("settings", c, bg);
    { const UiButton* x = findBtn(m, ID_CLOSE); const UiButton* dbg = findBtn(m, ID_DEBUG);
      CHECK(x && dbg && x->rect.x < dbg->rect.x && x->rect.x < W / 3 && dbg->rect.x > W / 2);                                       // X left, Debug right
      CHECK(alphaAt(c, 2, 2) == 255 && c.data()[0] == 0 && c.data()[1] == 0 && c.data()[2] == 0); }                                   // black page (nothing shows through)
    for (int id : {ID_START_AA, ID_START_CAM, ID_START_SPLIT, ID_START_PIP, ID_START_LAST, ID_THEME_AUTO, ID_THEME_DARK, ID_THEME_LIGHT}) CHECK(findBtn(m, id));
    CHECK(!findBtn(m, ID_START_SETTINGS));                                                                                            // "start on Settings" is no longer offered
    { const UiButton* a = findBtn(m, ID_START_AA); const UiButton* l = findBtn(m, ID_START_LAST); const UiButton* t3 = findBtn(m, ID_SPLIT_33);
      CHECK(a && l && a->rect.y == l->rect.y && a->rect.w == l->rect.w && a->rect.x < W / 4 && l->rect.x + l->rect.w > W * 3 / 4);   // five start options: a full-width row of equal segments
      CHECK(t3 && t3->rect.x > W / 2);                                                                                               // three segments fit beside the label: right column
      it = m.activate(ID_THEME_LIGHT, md); CHECK(it.kind == Intent::SetTheme && it.i == 2);
      md.light = true; m.render(c, md, t); save("settings-light", c, bg); CHECK(c.data()[(((size_t)(H / 2)) * W + W / 2) * 4] > 200);   // light card: bright pixel mid-card
      md.light = false; }
    it = m.activate(ID_START_CAM, md); CHECK(it.kind == Intent::SetStart && it.i == (int)Mode::FullCam && m.screen() == Screen::Settings);
    it = m.activate(ID_START_AA, md); CHECK(it.kind == Intent::SetStart && it.i == (int)Mode::FullAA);
    it = m.activate(ID_CORNER_TL, md); CHECK(it.kind == Intent::SetCorner && it.i == (int)Corner::TL);
    it = m.activate(ID_SIZE_L, md); CHECK(it.kind == Intent::SetSize && it.i == (int)PipSize::L);
    it = m.activate(ID_SPLIT_67, md); CHECK(it.kind == Intent::SetSplit && it.d > 0.66 && it.d < 0.68);
    it = m.activate(ID_REV_ON, md); CHECK(it.kind == Intent::SetAutoCam && it.i == 1);
    CHECK(!m.tick(t + 10 * 60 * 1000) && m.screen() == Screen::Settings);                                                            // no auto-close, ever
    // a settings page whose start screen is Settings highlights it (and not the mode)
    { Model ss = md; ss.prefs.start_settings = true; m.render(c, ss, t); save("settings-start-settings", c, bg); }
    m.render(c, md, t);
    // Debug page: X left goes back to Settings
    it = m.activate(ID_DEBUG, md); CHECK(it.kind == Intent::OpenDebug && m.screen() == Screen::Debug);
    md.truck_line1 = "Gear P   Parking brake on   Speed 0 km/h"; md.truck_line2 = "Driving status 0   Night no   Sent to phone: 0 / day"; md.status = "Phone connected  |  Camera OK";
    m.render(c, md, t); save("debug", c, bg); CHECK(findBtn(m, ID_BACK) && findBtn(m, ID_BACK)->rect.x < W / 3);
    for (int id : {ID_DBG_GEAR_T, ID_DBG_GEAR_P, ID_DBG_GEAR_R, ID_DBG_GEAR_D, ID_DBG_DRV_T, ID_DBG_DRV_PARKED, ID_DBG_DRV_MOVING, ID_DBG_NIGHT_T, ID_DBG_NIGHT_DAY, ID_DBG_NIGHT_NIGHT}) CHECK(findBtn(m, id));
    it = m.activate(ID_DBG_GEAR_R, md); CHECK(it.kind == Intent::SetDbgGear && it.i == 2);
    it = m.activate(ID_DBG_GEAR_T, md); CHECK(it.kind == Intent::SetDbgGear && it.i == 0);
    it = m.activate(ID_DBG_DRV_MOVING, md); CHECK(it.kind == Intent::SetDbgDriving && it.i == 2);
    it = m.activate(ID_DBG_DRV_T, md); CHECK(it.kind == Intent::SetDbgDriving && it.i == 0);
    it = m.activate(ID_DBG_NIGHT_NIGHT, md); CHECK(it.kind == Intent::SetDbgNight && it.i == 2);
    { Rect card = {0, 0, W, H}; for (auto& b : m.buttons()) if (b.id >= ID_DBG_GEAR_T) CHECK(b.rect.x >= card.x && b.rect.x + b.rect.w <= W && b.rect.y + b.rect.h <= H && b.rect.h >= 40 * sc); }   // all on screen, usable size
    { Model ov = md; ov.prefs.dbg_gear = 2; ov.prefs.dbg_driving = 2; ov.prefs.dbg_night = 2; m.render(c, ov, t); save("debug-overrides", c, bg); }
    m.render(c, md, t);
    CHECK(!m.tick(t + 600000) && m.screen() == Screen::Debug);
    // restart / reboot buttons: present, bottom of the page, two taps within 3 s to fire, a late second tap only re-arms
    { m.render(c, md, t); auto* so = findBtn(m, ID_DBG_SPLITDRAG_OFF); auto* sn = findBtn(m, ID_DBG_SPLITDRAG_ON);
      CHECK(so && sn && so->rect.x < sn->rect.x);                                                                   // Debug: Split divider drag Off | On (Off by default)
      it = m.activate(ID_DBG_SPLITDRAG_ON, md); CHECK(it.kind == Intent::SetSplitDrag && it.i == 1);
      it = m.activate(ID_DBG_SPLITDRAG_OFF, md); CHECK(it.kind == Intent::SetSplitDrag && it.i == 0); }
    { m.render(c, md, t); auto* rb = findBtn(m, ID_DBG_RESTART); auto* bb = findBtn(m, ID_DBG_REBOOT);
      CHECK(rb && bb && rb->rect.y > H / 2 && bb->rect.x < rb->rect.x);
      it = m.activate(ID_DBG_RESTART, md); CHECK(it.kind == Intent::None);                                      // first tap arms only
      m.render(c, md, t + 500); save("debug-armed", c, bg);
      it = m.activate(ID_DBG_RESTART, md); CHECK(it.kind == Intent::SysRestart);                                // second tap within 3 s fires
      it = m.activate(ID_DBG_REBOOT, md); CHECK(it.kind == Intent::None); m.render(c, md, t + 5000);
      it = m.activate(ID_DBG_REBOOT, md); CHECK(it.kind == Intent::None);                                       // 4.5 s later: window expired -> re-armed, not fired
      it = m.activate(ID_DBG_REBOOT, md); CHECK(it.kind == Intent::SysReboot);
      it = m.activate(ID_DBG_RESTART, md); CHECK(it.kind == Intent::None); CHECK(m.tick(t + 9000));           // disarm after 3 s asks for a redraw
      m.render(c, md, t + 9000); }
    m.activate(ID_BACK, md); CHECK(m.screen() == Screen::Settings);
    // X closes Settings
    m.activate(ID_CLOSE, md); CHECK(m.screen() == Screen::Hidden);
    // start on Settings: the page can be opened with no state underneath
    { Menu ms(W, H); ms.openSettings(); Model nn = mk(Mode::FullAA, false, false); ms.render(c, nn, t); CHECK(ms.screen() == Screen::Settings && c.data()[3] == 255); }

    // ---- safe zone editor, from Settings (returns there) and from the camera bar (returns to the camera) ----
    m.hide(); m.open(); m.render(c, md, t); m.activate(ID_SETTINGS, md); m.render(c, md, t);
    CHECK(m.activate(ID_ZONE_RESET, md).kind == Intent::None);                     // disabled: no zone set (full frame)
    md.cam_view = {160, 0, 960, 720};                                              // 4:3 camera pillarboxed
    it = m.activate(ID_ZONE_EDIT, md); CHECK(it.kind == Intent::BeginZoneEdit && m.zoneEditing());
    m.render(c, md, t); save("zone-empty", c, bg);
    CHECK(m.activate(ID_ZONE_SAVE, md).kind == Intent::None);                      // nothing drawn yet -> cannot save
    m.zoneDrag(400, 100, true, md); m.zoneDrag(700, 400, false, md); m.zoneDrag(880, 560, false, md);
    m.render(c, md, t); save("zone-drawn", c, bg);
    it = m.activate(ID_ZONE_SAVE, md);
    CHECK(it.kind == Intent::SaveRoi);
    CHECK(std::abs(it.roi.x - 0.25) < 0.01 && std::abs(it.roi.y - 100.0 / 720) < 0.01 && std::abs(it.roi.w - 0.5) < 0.01 && std::abs(it.roi.h - 460.0 / 720) < 0.01);
    CHECK(m.screen() == Screen::Settings);
    m.open(); m.activate(ID_SETTINGS, md); m.render(c, md, t); m.activate(ID_ZONE_EDIT, md);          // clamps to the camera area; reverse drag works
    m.zoneDrag(1270, 710, true, md); m.zoneDrag(10, 10, false, md); m.render(c, md, t);
    it = m.activate(ID_ZONE_SAVE, md); CHECK(it.kind == Intent::SaveRoi && it.roi.x >= 0 && it.roi.x + it.roi.w <= 1.0001 && it.roi.w > 0.99);
    m.open(); m.activate(ID_SETTINGS, md); m.render(c, md, t); m.activate(ID_ZONE_EDIT, md); m.render(c, md, t);
    it = m.activate(ID_ZONE_CANCEL, md); CHECK(it.kind == Intent::EndZoneEdit && m.screen() == Screen::Settings);
    { Model fcm = mk(Mode::FullCam, true, true); fcm.cam_view = md.cam_view; Menu mm(W, H); mm.render(c, fcm, t);
      mm.activate(ID_CAM_ZONE, fcm); CHECK(mm.zoneEditing()); mm.render(c, fcm, t);
      CHECK(mm.activate(ID_ZONE_CANCEL, fcm).kind == Intent::EndZoneEdit && mm.screen() == Screen::Hidden); }                          // back to the camera
    m.open(); m.activate(ID_SETTINGS, md); m.render(c, md, t); m.activate(ID_ZONE_EDIT, md); m.noteActivity(t);
    CHECK(!m.tick(t + 60000) && m.zoneEditing());                                  // the zone editor never auto-hides

    // ---- router integration: while idle EVERY touch (right edge included) is the phone's; while awake only the pill buttons are ours ----
    { Menu mm(W, H); mm.render(c, aa, t);
      InputRouter r; r.configure(W, H, W, H, W, H); r.setButtons(mm.buttons()); r.setLayout(aa.layout);
      auto hit = r.route(W - 20, H / 2, Action::Down); CHECK(hit.target == Target::Phone); r.route(W - 20, H / 2, Action::Up);
      hit = r.route(300, 300, Action::Down); CHECK(hit.target == Target::Phone); r.route(300, 300, Action::Up); }

    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
