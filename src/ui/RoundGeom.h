#pragma once
// Rounded-rectangle geometry shared by the overlay drawing (Gfx.cpp) and the video-path corner restore (Compositor.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Layout.h"

namespace ui {

// Signed distance from (px,py) to the rounded rectangle r with corner radius rad: negative inside, positive outside.
inline double sdRoundRectGeom(double px, double py, const Rect& r, double rad) {
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, hx = r.w / 2.0 - rad, hy = r.h / 2.0 - rad;
    const double qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - rad;
}

// The four square corners of a rounded rectangle that must show the layer UNDER the rectangle. Patches are k x k (k even), origins even.
struct CornerPatches {
    int k = 0; int ox[4] = {0, 0, 0, 0}, oy[4] = {0, 0, 0, 0};
    std::vector<uint8_t> cov;                       // 4 * k * k, 0..255: how much of the underlying layer shows through at that pixel
    bool valid() const { return k > 0; }
    static CornerPatches make(const Rect& r, double rad) {
        CornerPatches p; if (r.w < 8 || r.h < 8 || rad < 1) return p;
        rad = std::min(rad, std::min(r.w, r.h) / 2.0);
        int k = ((int)std::ceil(rad) + 3) & ~1; p.k = k;
        const int xs[2] = {r.x & ~1, (r.x + r.w - k) & ~1}, ys[2] = {r.y & ~1, (r.y + r.h - k) & ~1};
        p.cov.assign((size_t)4 * k * k, 0);
        for (int c = 0; c < 4; ++c) {
            p.ox[c] = xs[c & 1]; p.oy[c] = ys[c >> 1];
            for (int y = 0; y < k; ++y) for (int x = 0; x < k; ++x) {
                const double d = sdRoundRectGeom(p.ox[c] + x + 0.5, p.oy[c] + y + 0.5, r, rad);
                const double a = std::min(1.0, std::max(0.0, 0.5 + d));
                p.cov[(size_t)c * k * k + (size_t)y * k + x] = (uint8_t)std::lround(a * 255);
            }
        }
        return p;
    }
};

}  // namespace ui
