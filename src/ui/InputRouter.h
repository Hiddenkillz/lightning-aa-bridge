#pragma once
// Touch routing: decides whether a truck touch belongs to our UI, the camera
// window, or the phone (remapped into the phone's video frame coordinates).
// A gesture sticks to the target chosen on press until release, so dragging out
// of a button never leaks half a gesture to the phone.
#include <vector>
#include "Layout.h"

namespace ui {

struct UiButton { int id; Rect rect; };            // topmost last

enum class Target { None, Ui, Camera, Phone };
enum class Action : int { Down = 0, Up = 1, Move = 2 };   // AA touch_action: 0 press, 1 release, 2 move

struct Routed {
    Target target = Target::None;
    int ui_id = -1;                                 // when target == Ui
    int phone_x = 0, phone_y = 0;                   // when target == Phone (phone frame pixels)
    int ox = 0, oy = 0;                             // touch position in output pixels (always set)
    Action action = Action::Down;
    bool gesture_start = false;
};

class InputRouter {
public:
    // touch_w/h: the truck touchscreen's coordinate space (from SDR input descriptor).
    // phone_w/h: the phone video frame size (what the phone believes its screen is).
    void configure(int touch_w, int touch_h, int out_w, int out_h, int phone_w, int phone_h) {
        tw_ = touch_w; th_ = touch_h; ow_ = out_w; oh_ = out_h; pw_ = phone_w; ph_ = phone_h;
    }
    void setLayout(const Layout& l) { layout_ = l; }
    void setPhoneSpace(int w, int h) { pw_ = w; ph_ = h; }      // the touch space the phone is given (Split: may be the pane's own size)
    // Truck touch-space point -> phone frame pixels (clamped into the phone pane), for forwarding every finger of a phone-owned gesture.
    void toPhone(int tx, int ty, int& px, int& py) const {
        int ox = tw_ > 0 ? (int)((long long)tx * ow_ / tw_) : tx, oy = th_ > 0 ? (int)((long long)ty * oh_ / th_) : ty;
        const Rect& pr = layout_.phone.rect;
        int lx = std::clamp(ox - pr.x, 0, std::max(0, pr.w - 1)), ly = std::clamp(oy - pr.y, 0, std::max(0, pr.h - 1));
        px = pr.w > 0 ? (int)((long long)lx * pw_ / pr.w) : 0; py = pr.h > 0 ? (int)((long long)ly * ph_ / pr.h) : 0;
    }
    Target currentTarget() const { return active_ ? cur_ : Target::None; }   // target of the gesture in progress
    void cancel() { active_ = false; cur_ = Target::None; cur_id_ = -1; }
    void setButtons(std::vector<UiButton> b) { buttons_ = std::move(b); }

    Routed route(int tx, int ty, Action a) {
        Routed r; r.action = a;
        int ox = tw_ > 0 ? (int)((long long)tx * ow_ / tw_) : tx;       // touch space -> output pixels
        int oy = th_ > 0 ? (int)((long long)ty * oh_ / th_) : ty;
        if (a == Action::Down || !active_) {
            active_ = true; r.gesture_start = true;
            cur_ = Target::Phone; cur_id_ = -1;
            for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it)
                if (it->rect.contains(ox, oy)) { cur_ = Target::Ui; cur_id_ = it->id; break; }
            if (cur_ == Target::Phone && layout_.camera_hit.contains(ox, oy)) cur_ = Target::Camera;
            if (cur_ == Target::Phone && !(layout_.phone.live && layout_.phone.rect.contains(ox, oy))) cur_ = Target::None;   // placeholder panes take no touches
        }
        r.target = cur_; r.ui_id = cur_id_; r.ox = ox; r.oy = oy;
        if (cur_ == Target::Phone) {
            const Rect& pr = layout_.phone.rect;     // remap into phone frame, clamped (gesture may drift out)
            int lx = std::clamp(ox - pr.x, 0, std::max(0, pr.w - 1)), ly = std::clamp(oy - pr.y, 0, std::max(0, pr.h - 1));
            r.phone_x = pr.w > 0 ? (int)((long long)lx * pw_ / pr.w) : 0;
            r.phone_y = pr.h > 0 ? (int)((long long)ly * ph_ / pr.h) : 0;
        }
        if (a == Action::Up) active_ = false;
        return r;
    }

private:
    int tw_ = 0, th_ = 0, ow_ = 1280, oh_ = 720, pw_ = 1280, ph_ = 720;
    Layout layout_;
    std::vector<UiButton> buttons_;
    bool active_ = false;
    Target cur_ = Target::None;
    int cur_id_ = -1;
};

}  // namespace ui
