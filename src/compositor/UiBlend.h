#pragma once
// UI overlay blending done by us, right before the encoder, instead of by the GStreamer compositor.
//
// Why: the compositor converts every pad to its output format on EVERY output frame. A full-frame RGBA UI layer (placeholder, Settings, ...) cost
// ~140% of a Pi core per second even when nothing changed, the mixer fell below 30 fps, its timeline fell behind real time and the whole UI lagged by
// minutes (truck test 27). The UI only changes on a touch, so: convert RGBA -> YUV+alpha ONCE per change (YuvaLayer::fromRgba) and per frame only copy
// the opaque spans / blend the translucent pixels into the I420 frame (blendInto). Opaque full-window pages cost a few memcpy()s per frame.
//
// Colour: BT.601 limited range, same as GStreamer's videoconvert default for RGB -> I420. Alpha is straight (non-premultiplied), like GStreamer RGBA.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ui {

struct YuvaLayer {
    int x = 0, y = 0, w = 0, h = 0;                      // placement in the output frame (x, y, w, h all even)
    std::vector<uint8_t> Y, A;                           // w*h
    std::vector<uint8_t> U, V, AC;                       // (w/2)*(h/2): chroma and the alpha that applies to it
    struct Span { int x0 = 0, x1 = 0; bool opaque = false; };   // per row: [x0,x1) is the non-transparent run; opaque = every pixel in it has alpha 255
    std::vector<Span> yrows, crows;
    bool empty() const { return w <= 0 || h <= 0; }

    // Bounding box (x0,y0,x1,y1 exclusive) of the pixels whose alpha is not 0 in a w*h RGBA canvas; false when everything is transparent.
    static bool opaqueBounds(const uint8_t* rgba, int w, int h, int& x0, int& y0, int& x1, int& y1) {
        x0 = w; x1 = 0; y0 = h; y1 = 0;
        for (int j = 0; j < h; ++j) {
            const uint8_t* row = rgba + (size_t)j * w * 4; int a = 0, b = w;
            while (a < w && row[a * 4 + 3] == 0) ++a;
            if (a == w) continue;
            while (b > a && row[(b - 1) * 4 + 3] == 0) --b;
            x0 = std::min(x0, a); x1 = std::max(x1, b); y0 = std::min(y0, j); y1 = j + 1;
        }
        return x1 > x0 && y1 > y0;
    }

    static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

    // rgba: w*h*4 straight-alpha pixels (row stride = w*4). x,y,w,h are rounded to even values (4:2:0).
    static YuvaLayer fromRgba(const uint8_t* rgba, int w, int h, int x, int y) { return fromRgba(rgba, w, h, x, y, w); }
    // stride_px: pixels per source row (lets the caller convert a sub-rectangle of a larger canvas: pass a pointer to its top-left pixel).
    static YuvaLayer fromRgba(const uint8_t* rgba, int w, int h, int x, int y, int stride_px) {
        YuvaLayer L;
        w &= ~1; h &= ~1; x &= ~1; y &= ~1;
        if (!rgba || w <= 0 || h <= 0) return L;
        L.x = x; L.y = y; L.w = w; L.h = h;
        const int stride = stride_px;
        L.Y.resize((size_t)w * h); L.A.resize((size_t)w * h);
        L.U.resize((size_t)(w / 2) * (h / 2)); L.V.resize(L.U.size()); L.AC.resize(L.U.size());
        for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) {
            const uint8_t* p = rgba + ((size_t)j * stride + i) * 4;
            const int r = p[0], g = p[1], b = p[2];
            L.Y[(size_t)j * w + i] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            L.A[(size_t)j * w + i] = p[3];
        }
        for (int j = 0; j < h / 2; ++j) for (int i = 0; i < w / 2; ++i) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                const uint8_t* p = rgba + ((size_t)(2 * j + dy) * stride + 2 * i + dx) * 4; r += p[0]; g += p[1]; b += p[2]; a += p[3];
            }
            r = (r + 2) >> 2; g = (g + 2) >> 2; b = (b + 2) >> 2; a = (a + 2) >> 2;
            const size_t k = (size_t)j * (w / 2) + i;
            L.U[k] = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            L.V[k] = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            L.AC[k] = (uint8_t)a;
        }
        auto summarize = [](const std::vector<uint8_t>& A, int pw, int ph, std::vector<Span>& out) {
            out.assign((size_t)ph, Span{});
            for (int j = 0; j < ph; ++j) {
                const uint8_t* row = &A[(size_t)j * pw]; int a0 = 0, a1 = pw;
                while (a0 < pw && row[a0] == 0) ++a0;
                while (a1 > a0 && row[a1 - 1] == 0) --a1;
                bool opaque = a1 > a0; for (int i = a0; i < a1 && opaque; ++i) if (row[i] != 255) opaque = false;
                out[(size_t)j] = {a0, a1, opaque};
            }
        };
        summarize(L.A, w, h, L.yrows); summarize(L.AC, w / 2, h / 2, L.crows);
        return L;
    }

    static inline uint8_t mix(int dst, int src, int a) { return (uint8_t)((dst * (255 - a) + src * a + 127) / 255); }

    // Blend into planar I420 (strides in bytes; frame is fw x fh luma pixels). Rows / columns outside the frame are clipped.
    void blendInto(uint8_t* Yp, int ys, uint8_t* Up, int us, uint8_t* Vp, int vs, int fw, int fh) const {
        if (empty()) return;
        for (int j = 0; j < h; ++j) {
            const int fy = y + j; if (fy < 0 || fy >= fh) continue;
            const Span& s = yrows[(size_t)j]; if (s.x1 <= s.x0) continue;
            const int a0 = std::max(s.x0, -x), a1 = std::min(s.x1, fw - x); if (a1 <= a0) continue;
            uint8_t* d = Yp + (size_t)fy * ys + x; const uint8_t* sY = &Y[(size_t)j * w]; const uint8_t* sA = &A[(size_t)j * w];
            if (s.opaque) std::memcpy(d + a0, sY + a0, (size_t)(a1 - a0));
            else for (int i = a0; i < a1; ++i) { const int a = sA[i]; if (a == 255) d[i] = sY[i]; else if (a) d[i] = mix(d[i], sY[i], a); }
        }
        const int cw = w / 2, ch = h / 2, cx = x / 2, cy = y / 2, cfw = fw / 2, cfh = fh / 2;
        for (int j = 0; j < ch; ++j) {
            const int fy = cy + j; if (fy < 0 || fy >= cfh) continue;
            const Span& s = crows[(size_t)j]; if (s.x1 <= s.x0) continue;
            const int a0 = std::max(s.x0, -cx), a1 = std::min(s.x1, cfw - cx); if (a1 <= a0) continue;
            uint8_t* du = Up + (size_t)fy * us + cx; uint8_t* dv = Vp + (size_t)fy * vs + cx;
            const size_t o = (size_t)j * cw;
            if (s.opaque) { std::memcpy(du + a0, &U[o + a0], (size_t)(a1 - a0)); std::memcpy(dv + a0, &V[o + a0], (size_t)(a1 - a0)); }
            else for (int i = a0; i < a1; ++i) { const int a = AC[o + i]; if (a == 255) { du[i] = U[o + i]; dv[i] = V[o + i]; } else if (a) { du[i] = mix(du[i], U[o + i], a); dv[i] = mix(dv[i], V[o + i], a); } }
        }
    }
};

}  // namespace ui
