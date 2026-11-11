#pragma once
// Tiny software 2D renderer for the on-screen UI: RGBA8 (straight alpha) canvas,
// anti-aliased rounded rectangles / triangles, TrueType text (stb_truetype, font
// embedded in the binary). No external dependencies, deterministic, host-testable.
#include <cstdint>
#include <string>
#include <vector>
#include "Layout.h"

namespace ui {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    Color withAlpha(double k) const { return {r, g, b, (uint8_t)(a * k)}; }
};

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_((size_t)w * h * 4, 0) {}
    int w() const { return w_; }
    int h() const { return h_; }
    const uint8_t* data() const { return px_.data(); }
    uint8_t* data() { return px_.data(); }
    void clear() { std::fill(px_.begin(), px_.end(), 0); }

    void fillRoundRect(const Rect& r, double radius, Color c);
    void strokeRoundRect(const Rect& r, double radius, double thickness, Color c);
    void fillTriangle(double x0, double y0, double x1, double y1, double x2, double y2, Color c);
    void line(double x0, double y0, double x1, double y1, double thickness, Color c);      // anti-aliased round-capped line
    // Fill what lies OUTSIDE a rounded rectangle but inside its bounding square corners (anti-aliased), i.e. make a square video window look rounded by covering its 4 corners.
    // Colour comes from an opaque RGBA image `art` (placed at artRect, in canvas coordinates) where it covers the pixel, else `fallback`.
    void fillRoundCorners(const Rect& r, double radius, const uint8_t* art, const Rect& artRect, Color fallback);
    void clearRect(const Rect& r);                                                         // make a region fully transparent (hole for a live video pane)
    // Copy a w*h RGBA image to dst.x/dst.y (no scaling). Fast path for opaque rows; pixels with alpha < 255 (rounded corners) are blended.
    void drawImage(const uint8_t* rgba, int w, int h, int x, int y);
    // Draw an alpha mask (mw x mh, 0..255) scaled into dst (bilinear), tinted with c. Used for the icon set.
    void drawMask(const uint8_t* mask, int mw, int mh, const Rect& dst, Color c);

    // Text: px = glyph height in pixels. y is the vertical centre of the line.
    int textWidth(const std::string& utf8, int px) const;
    void text(int x, int cy, const std::string& utf8, int px, Color c);
    void textCentered(const Rect& r, const std::string& utf8, int px, Color c);

    void blendPixel(int x, int y, Color c, double coverage);

    // Bounding box of everything with alpha > 0 (for cropping the overlay).
    Rect contentBounds() const;

    // Save as binary PPM-with-alpha is awkward; dump raw RGBA for ffmpeg: -f rawvideo -pix_fmt rgba
    bool saveRaw(const std::string& path) const;

private:
    int w_, h_;
    std::vector<uint8_t> px_;
};

}  // namespace ui
