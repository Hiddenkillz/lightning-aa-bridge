#pragma once
#include <memory>
#include <functional>
#include <cstdint>
#include <cstddef>
#include "../Config.h"
#include "../ui/Layout.h"

// Compositor owns the GStreamer pipeline:
//
//   appsrc (phone H.264) --> h264parse --> v4l2h264dec
//         \
//          --> compositor --> v4l2h264enc --> appsink (to Device/Sync4)
//         /
//   rtspsrc (ESP32 cam) --> rtph264depay --> h264parse --> v4l2h264dec
//
// Uses Pi 4 hardware V4L2 codecs (bcm2835-codec) for decode and encode.
// Falls back to software (avdec_h264 / x264enc) if V4L2 not available.

class Compositor {
public:
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, uint64_t pts_us)>;
    long long mixerLagMs() const;                       // how far the output timeline is behind real time at the last logged sample (-1 = unknown)

    explicit Compositor(const Config& config);
    ~Compositor();

    void start();
    void stop();

    // Called by HeadUnit when phone sends a H.264 NAL unit
    void pushPhoneFrame(const uint8_t* data, size_t size, uint64_t pts_us);
    size_t phoneQueueBytes() const;     // phone H.264 waiting in front of the decoder (flow control)

    // Called by Device to receive composed frames to send to Sync 4
    void setOutputCallback(FrameCallback cb);

    // Apply a layout (pad geometry, visibility, camera safe-zone crop) at runtime; no pipeline restart
    void applyLayout(const ui::Layout& layout);

    // On-screen UI layers (straight-alpha RGBA). Both have FIXED caps for the life of the pipeline; only
    // visibility changes at runtime (no renegotiation). Handle = small edge tab (size ui::handleRect), panel = full frame.
    void setHandle(const uint8_t* rgba, bool visible);   // rgba: handleRect(W,H).w * .h * 4 bytes (ignored when !visible)
    // One-shot: call `cb` (from a short-lived thread) when the next decoded phone frame leaves the phone branch. Lets the UI keep the placeholder up
    // until real picture exists. Re-arm on every new phone session.
    void armPhoneVideoNotify(std::function<void()> cb);
    // Called (from the watchdog thread) when camera frames start / stop arriving. Drives the camera placeholder.
    void setCameraPresenceCallback(std::function<void(bool)> cb);
    void setCameraFlip(bool mirror, bool flip);                    // horizontal / vertical flip of the camera picture (how it is mounted)
    void setPhoneCrop(int left, int right, int top, int bottom);   // crop the phone frame to the head unit's visible UI area
    void setHandleOrigin(int x, int y);                   // top-left of the (fixed-size) handle layer in output pixels
    // Rounded PiP corners over a live phone picture: copy the phone's pixels into the four corner patches of the camera window right before the encoder.
    // cam = the camera window in OUTPUT-frame pixels; (phone_ox,phone_oy) = where the phone picture's top-left sits in the output frame; phone_w/h = its size.
    void setCornerRestore(bool on, int x, int y, int w, int h, double radius, int phone_ox, int phone_oy, int phone_w, int phone_h);
    bool baseArt() const;                                  // the base layer is an appsrc that carries the Android Auto splash (see setBaseI420)
    // Base layer picture, a full output frame in I420 (video_width * video_height * 3 / 2 bytes); null = black. Shown under both video panes.
    void setBaseI420(const uint8_t* i420);
    bool uiBlender() const;                                // true: the UI is blended before the encoder (no keepalive needed, setPanelRegion available)
    // Blender only: the panel is the w*h RGBA canvas whose top-left lands at (x,y) of the output frame. Only its non-transparent bounding box is converted.
    void setPanelRegion(const uint8_t* rgba, int x, int y, int w, int h, bool visible);
    void setPanel(const uint8_t* rgba, bool visible);    // rgba: video_width * video_height * 4 bytes (ignored when !visible)

    // Show/hide camera overlay at runtime
    void setCameraVisible(bool visible);

    // Resize/reposition overlay (pixels)
    void setOverlayGeometry(int x, int y, int w, int h);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
