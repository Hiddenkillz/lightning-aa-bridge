#pragma once
// On-screen controls for the Sync 4 display. Pure model + drawing: no GStreamer, no I/O. UiController feeds it touches/time and applies the
// Intents it returns.
//
// What is on screen (all inside the visible window):
//   * right edge:  a collapsed tab, or the mode pill (AA | Camera | PiP | Split | Settings). The pill is ALWAYS shown in the full-screen camera,
//                  elsewhere it opens from the tab and collapses by itself.
//   * full-screen camera: a bottom bar with the camera controls (Fit/Fill, Mirror, Flip, Reset zoom, Safe zone).
//   * camera window (PiP / split): small window buttons inside the pane (PiP: Max, Close. Split: PiP, Max, Close). Tapping the body does nothing.
//   * panes without a live source show their placeholder artwork (Android Auto / camera).
//   * Settings: an opaque full-window page (X top-left, Debug top-right). Debug: sensor page.   ZoneEdit: draw the camera safe zone.
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>
#include "Gfx.h"
#include "InputRouter.h"
#include "Layout.h"

namespace ui {

enum Id : int {
    ID_NONE = 0, ID_HANDLE, ID_PRECOND,
    ID_MODE_AA, ID_MODE_CAM, ID_MODE_SPLIT, ID_MODE_PIP, ID_MODE_ENERGY, ID_SETTINGS, ID_CLOSE, ID_BACK, ID_DEBUG,
    ID_START_AA, ID_START_CAM, ID_START_SPLIT, ID_START_PIP, ID_START_SETTINGS, ID_START_LAST,
    ID_CORNER_TL, ID_CORNER_TR, ID_CORNER_BL, ID_CORNER_BR,
    ID_SIZE_S, ID_SIZE_M, ID_SIZE_L,
    ID_SPLIT_169, ID_SPLIT_33, ID_SPLIT_50, ID_SPLIT_67,
    ID_REV_OFF, ID_REV_ON,
    ID_ZONE_EDIT, ID_ZONE_RESET, ID_ZONE_CANCEL, ID_ZONE_SAVE, ID_ZONE_CANVAS,
    ID_PANE_MAX, ID_PANE_CLOSE, ID_PANE_PIP,                     // window buttons inside the camera pane
    ID_CAM_FIT, ID_CAM_MIRROR, ID_CAM_FLIP, ID_CAM_RESET, ID_CAM_ZONE,   // full-screen camera bar
    ID_DBG_GEAR_T = 100, ID_DBG_GEAR_P, ID_DBG_GEAR_R, ID_DBG_GEAR_D,    // Debug page: gear override
    ID_DBG_DRV_T = 110, ID_DBG_DRV_PARKED, ID_DBG_DRV_MOVING,           // driving status sent to the phone
    ID_DBG_NIGHT_T = 120, ID_DBG_NIGHT_DAY, ID_DBG_NIGHT_NIGHT,         // night mode sent to the phone
    ID_DBG_RESTART = 130, ID_DBG_REBOOT,
    ID_DBG_SPLITDRAG_OFF = 135, ID_DBG_SPLITDRAG_ON,                   // Debug: experiment toggle for the draggable Split divider                                // restart the bridge service / reboot the Pi (two taps within 3 s)
    ID_THEME_AUTO = 140, ID_THEME_DARK, ID_THEME_LIGHT,                 // Settings: Appearance
    ID_SPLIT_HANDLE = 150,                                              // Split: the divider band (drag to resize)
};

enum class Screen { Hidden, Home, Settings, Debug, ZoneEdit };   // Home = the pill, opened (it also stays open on its own in the full-screen camera)

struct Model {
    Mode mode = Mode::FullCam;
    Prefs prefs;
    bool phone = false, camera = false;   // a LIVE video source feeds the phone / camera pane
    Layout layout;                        // pane rectangles in window coordinates (panes without a live source are placeholders)
    bool moving = false;                  // informational only (no lock): vehicle speed state
    Rect cam_view;                        // where the full (un-cropped) camera frame is shown during zone editing
    bool corner_restore = false;          // the video path copies the phone's pixels into the rounded corners over a live phone: no dark fill needed
    bool pip_corners = false;             // cover the square corners of the live camera PiP with the splash (or a dark fill) so it looks rounded (overlay only: no video-path cost)
    bool pip_dragging = false;            // PiP being dragged: no corner covers (they would trail behind the video; they are drawn again on drop)
    bool base_art = false;                // the Android Auto splash is the video base layer (not painted here, no hole for the camera)
    bool precond_shown = false;           // the pill carries the battery-preconditioning toggle (staged feature, config precondition_button)
    int precond = 0;                      // 0 off, 1 on (requested; never saved: always off after a restart)
    bool energy_shown = false;            // the strip carries the Energy (vehicle data) page button (config obd_enable)
    struct EnergyRow { std::string label, value, unit; int level = 0; };   // level: 0 normal, 1 accent (regen / active), 2 warning, -1 stale (dash)
    std::string energy_big_a, energy_big_b, energy_big_a_label, energy_big_b_label;   // the two large readouts (kW, mi/kWh)
    std::vector<EnergyRow> energy_rows;   // the rest, in priority order (full page shows all)
    std::string energy_link;              // adapter / link status line
    std::string status;                   // one-line status (debug page)
    std::string truck_line1, truck_line2; // what the truck reports right now (debug page readout)
    std::string info_line;                // Debug readout, third line (camera format, CPU temperature, phone link)
    std::string build;                    // build tag shown in the footers (from /etc/aa-bridge-build, written by the deploy script)
    bool light = false;                   // effective appearance (Prefs::theme resolved with the night state)
    bool split_drag_enabled = false;      // config split_drag: the divider can be grabbed (grip + band shown)
    bool split_dragging = false;          // the split divider is being dragged (phone pane shows its placeholder; the grip follows the finger)
    int split_edge = 0;                   // while dragging: -1 = in the top zone (release -> full camera), +1 = bottom zone (release -> full Android), 0 = normal
    int64_t mode_changed_ms = -1000000;   // when the mode last changed (the mode toast shows for a few seconds after)
};

struct Intent {
    enum Kind { None, SetMode, SetStart, SetStartSettings, SetStartLast, SetCorner, SetSize, SetSplit, SetAutoCam, ResetRoi, SaveRoi, BeginZoneEdit, EndZoneEdit,
                SetCamFill, SetCamMirror, SetCamFlip, ResetCamView, OpenDebug, SetDbgGear, SetDbgDriving, SetDbgNight, SetPrecond, SysRestart, SysReboot, SetTheme, SetSplitDrag } kind = None;
    int i = 0;                            // mode / corner / size enum value, or bool
    double d = 0;                         // split ratio
    Roi roi;                              // SaveRoi
};

class Menu {
public:
    struct Pal;                                                  // colour palette (dark / light), chosen per render
    Menu(int W, int H) : W_(W), H_(H), s_(H / 720.0) {}

    Screen screen() const { return screen_; }
    bool zoneEditing() const { return screen_ == Screen::ZoneEdit; }

    // Draw everything for the current state into `c` (cleared first) and rebuild the hit areas.
    void render(Canvas& c, const Model& m, int64_t now_ms);
    const std::vector<UiButton>& buttons() const { return buttons_; }
    Rect contentBounds(const Canvas& c) const { return c.contentBounds(); }

    // True when the big full-window overlay layer is needed. False = only the small right-edge strip (handleRect) has content, so the cheap
    // edge layer can carry it (live Android Auto with the pill collapsed).
    bool needsPanel(const Model& m) const;

    // Input
    // Any touch anywhere calls this. The pill and the camera-window buttons are only on screen for kAwakeMs after the last touch (the touch itself
    // still goes to the phone / camera as usual). Returns true when this touch just woke something that needs drawing.
    static constexpr int64_t kAwakeMs = 4000;
    bool awakeAt(int64_t t) const { return t - last_activity_ < kAwakeMs; }
    static bool fullPage(Mode m) { return m == Mode::FullCam || m == Mode::Energy; }   // device-owned full screens: the pill stays open on its own
    bool noteActivity(int64_t now_ms) { const bool woke = !awakeAt(now_ms) && !fullPage(mode_); last_activity_ = now_ms; return woke; }
    void setPressed(int id) { pressed_ = id; }
    Intent activate(int id, const Model& m);                    // a completed tap on widget `id`
    void zoneDrag(int x, int y, bool start, const Model& m);    // drag inside ZoneEdit (output px)
    bool tick(int64_t now_ms);                                  // returns true when the picture needs a redraw

    void open() { screen_ = Screen::Home; }
    void openSettings() { screen_ = Screen::Settings; }
    void hide() { screen_ = Screen::Hidden; pressed_ = ID_NONE; }
    void setScreen(Screen s) { screen_ = s; }
    bool pillOpen(const Model& m) const { return screen_ == Screen::Home || (screen_ == Screen::Hidden && (fullPage(m.mode) || awakeAt(now_))); }
    void drawEnergyPage(Canvas& c, const Model& m);

private:
    struct Widget { int id; Rect r; std::string label; int px; bool selected; bool enabled; int icon; int style; };   // style: 0 button, 1 segment, 2 pane (translucent), 3 pill, 4 bar chip

    void build(const Model& m, int64_t now);
    void drawPlaceholders(Canvas& c, const Model& m);
    void drawIcon(Canvas& c, int icon, const Rect& box, Color fg, double alpha = 1.0);
    void drawWidget(Canvas& c, const Widget& w);
    const Pal* P_ = nullptr;
    void buildRows(const Model& m, bool settings);              // Settings / Debug page rows
    struct RowSpec { std::string label, hint; std::vector<std::tuple<int, std::string, bool, bool>> segs; bool wide; int y, h; };   // seg: id, label, selected, enabled
    std::vector<RowSpec> rows_; std::vector<std::pair<Rect, std::string>> sections_; Rect readout_; std::string foot_left_, foot_right_;
    int S(int v) const { return (int)(v * s_ + 0.5); }

    int W_, H_; double s_;
    Screen screen_ = Screen::Hidden;
    int pressed_ = ID_NONE;
    int64_t last_activity_ = -1000000, now_ = 0;   // starts asleep
    int arm_id_ = 0; int64_t arm_t_ = 0;           // Debug page restart/reboot: first tap arms for 3 s, second tap fires
    bool armed(int id) const { return arm_id_ == id && now_ - arm_t_ < 3000; }
    bool last_awake_ = false;
    Mode mode_ = Mode::FullCam;                                 // from the last build (tick() needs to know if the pill is persistent)
    Rect card_, pill_; std::vector<Rect> pill_divs_;   // group dividers inside the strip
    std::vector<Widget> widgets_;
    std::vector<std::pair<Rect, std::string>> labels_;
    std::vector<UiButton> buttons_;
    // zone editor draft (output px)
    bool zone_from_settings_ = true;                            // where Cancel/Save return to (Settings page, or the camera bar)
    bool zone_has_ = false; int zx0_ = 0, zy0_ = 0, zx1_ = 0, zy1_ = 0, ax_ = 0, ay_ = 0;
};

}  // namespace ui
