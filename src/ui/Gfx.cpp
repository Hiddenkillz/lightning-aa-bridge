#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#include "Gfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "FontData.h"

namespace ui {

static stbtt_fontinfo& font() {
    static stbtt_fontinfo f;
    static bool ok = false;
    if (!ok) { stbtt_InitFont(&f, kFontBold, stbtt_GetFontOffsetForIndex(kFontBold, 0)); ok = true; }
    return f;
}

void Canvas::blendPixel(int x, int y, Color c, double cov) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || cov <= 0) return;
    double sa = (c.a / 255.0) * std::min(1.0, cov);
    if (sa <= 0) return;
    uint8_t* p = &px_[((size_t)y * w_ + x) * 4];
    double da = p[3] / 255.0;
    double oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    p[0] = (uint8_t)std::lround((c.r * sa + p[0] * da * (1 - sa)) / oa);
    p[1] = (uint8_t)std::lround((c.g * sa + p[1] * da * (1 - sa)) / oa);
    p[2] = (uint8_t)std::lround((c.b * sa + p[2] * da * (1 - sa)) / oa);
    p[3] = (uint8_t)std::lround(oa * 255);
}

// signed distance (px) from point to a rounded rect; negative inside
static double sdRoundRect(double px, double py, const Rect& r, double rad) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double qx = std::fabs(px - cx) - (r.w / 2.0 - rad), qy = std::fabs(py - cy) - (r.h / 2.0 - rad);
    double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - rad;
}

void Canvas::fillRoundRect(const Rect& r, double rad, Color c) {
    rad = std::min(rad, std::min(r.w, r.h) / 2.0);
    for (int y = std::max(0, r.y - 1); y < std::min(h_, r.y + r.h + 1); ++y)
        for (int x = std::max(0, r.x - 1); x < std::min(w_, r.x + r.w + 1); ++x) {
            double d = sdRoundRect(x + 0.5, y + 0.5, r, rad);
            blendPixel(x, y, c, 0.5 - d);
        }
}

void Canvas::fillRoundCorners(const Rect& r, double rad, const uint8_t* art, const Rect& ar, Color fb) {
    rad = std::min(rad, std::min(r.w, r.h) / 2.0); const int k = (int)std::ceil(rad) + 1;
    auto corner = [&](int x0, int y0) {
        for (int y = std::max(0, y0); y < std::min(h_, y0 + k); ++y)
            for (int x = std::max(0, x0); x < std::min(w_, x0 + k); ++x) {
                const double cov = std::min(1.0, std::max(0.0, 0.5 + sdRoundRect(x + 0.5, y + 0.5, r, rad)));    // 1 outside the rounded shape, 0 inside
                if (cov <= 0) continue;
                Color c = fb;
                if (art && x >= ar.x && y >= ar.y && x < ar.x + ar.w && y < ar.y + ar.h) { const uint8_t* s = art + ((size_t)(y - ar.y) * ar.w + (x - ar.x)) * 4; c = Color{s[0], s[1], s[2], 255}; }
                blendPixel(x, y, c, cov);
            }
    };
    corner(r.x, r.y); corner(r.x + r.w - k, r.y); corner(r.x, r.y + r.h - k); corner(r.x + r.w - k, r.y + r.h - k);
}

void Canvas::strokeRoundRect(const Rect& r, double rad, double t, Color c) {
    rad = std::min(rad, std::min(r.w, r.h) / 2.0);
    for (int y = std::max(0, r.y - 2); y < std::min(h_, r.y + r.h + 2); ++y)
        for (int x = std::max(0, r.x - 2); x < std::min(w_, r.x + r.w + 2); ++x) {
            double d = std::fabs(sdRoundRect(x + 0.5, y + 0.5, r, rad) + t / 2.0) - t / 2.0;   // ring centred inside the edge
            blendPixel(x, y, c, 0.5 - d);
        }
}

void Canvas::fillTriangle(double x0, double y0, double x1, double y1, double x2, double y2, Color c) {
    int minx = (int)std::floor(std::min({x0, x1, x2})), maxx = (int)std::ceil(std::max({x0, x1, x2}));
    int miny = (int)std::floor(std::min({y0, y1, y2})), maxy = (int)std::ceil(std::max({y0, y1, y2}));
    auto edge = [](double ax, double ay, double bx, double by, double px, double py) { return (px - ax) * (by - ay) - (py - ay) * (bx - ax); };
    double area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            int in = 0;
            for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {   // 4x4 supersample
                double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
                double w0 = edge(x1, y1, x2, y2, px, py) / area, w1 = edge(x2, y2, x0, y0, px, py) / area, w2 = edge(x0, y0, x1, y1, px, py) / area;
                if (w0 >= 0 && w1 >= 0 && w2 >= 0) ++in;
            }
            blendPixel(x, y, c, in / 16.0);
        }
}

void Canvas::line(double x0, double y0, double x1, double y1, double t, Color c) {
    const double r = t / 2.0, dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    int minx = (int)std::floor(std::min(x0, x1) - r - 1), maxx = (int)std::ceil(std::max(x0, x1) + r + 1);
    int miny = (int)std::floor(std::min(y0, y1) - r - 1), maxy = (int)std::ceil(std::max(y0, y1) + r + 1);
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5, u = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
            u = std::clamp(u, 0.0, 1.0);
            double d = std::hypot(px - (x0 + u * dx), py - (y0 + u * dy)) - r;
            blendPixel(x, y, c, 0.5 - d);
        }
}

void Canvas::clearRect(const Rect& r) {
    for (int y = std::max(0, r.y); y < std::min(h_, r.y + r.h); ++y) {
        int x0 = std::max(0, r.x), x1 = std::min(w_, r.x + r.w);
        if (x1 > x0) std::fill(&px_[((size_t)y * w_ + x0) * 4], &px_[((size_t)y * w_ + x1) * 4], (uint8_t)0);
    }
}

void Canvas::drawImage(const uint8_t* img, int iw, int ih, int x0, int y0) {
    for (int j = 0; j < ih; ++j) {
        int y = y0 + j; if (y < 0 || y >= h_) continue;
        const uint8_t* srow = img + (size_t)j * iw * 4;
        int i0 = std::max(0, -x0), i1 = std::min(iw, w_ - x0);
        for (int i = i0; i < i1;) {
            // opaque run -> memcpy; anything else -> blend one pixel at a time
            int k = i; while (k < i1 && srow[k * 4 + 3] == 255) ++k;
            if (k > i) { std::memcpy(&px_[((size_t)y * w_ + x0 + i) * 4], srow + (size_t)i * 4, (size_t)(k - i) * 4); i = k; continue; }
            const uint8_t* s = srow + (size_t)i * 4;
            if (s[3]) blendPixel(x0 + i, y, Color{s[0], s[1], s[2], s[3]}, 1.0);
            ++i;
        }
    }
}

// --- text ---
static std::vector<int> decodeUtf8(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int cp, n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3f);
        out.push_back(cp); i += n;
    }
    return out;
}

int Canvas::textWidth(const std::string& s, int px) const {
    stbtt_fontinfo& f = font();
    float scale = stbtt_ScaleForPixelHeight(&f, (float)px);
    double w = 0; int prev = 0;
    for (int cp : decodeUtf8(s)) {
        int adv, lsb; stbtt_GetCodepointHMetrics(&f, cp, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(&f, prev, cp) * scale;
        w += adv * scale; prev = cp;
    }
    return (int)std::ceil(w);
}

void Canvas::text(int x, int cy, const std::string& s, int px, Color c) {
    stbtt_fontinfo& f = font();
    float scale = stbtt_ScaleForPixelHeight(&f, (float)px);
    int baseline = cy + (int)std::lround(0.364 * px);   // DejaVu cap height ~0.729em: centres capitals/digits on cy
    double cx = x; int prev = 0;
    for (int cp : decodeUtf8(s)) {
        int adv, lsb; stbtt_GetCodepointHMetrics(&f, cp, &adv, &lsb);
        if (prev) cx += stbtt_GetCodepointKernAdvance(&f, prev, cp) * scale;
        int w, h, xo, yo;
        unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&f, scale, scale, (float)(cx - std::floor(cx)), 0, cp, &w, &h, &xo, &yo);
        if (bmp) {
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i)
                blendPixel((int)std::floor(cx) + xo + i, baseline + yo + j, c, bmp[j * w + i] / 255.0);
            stbtt_FreeBitmap(bmp, nullptr);
        }
        cx += adv * scale; prev = cp;
    }
}

void Canvas::textCentered(const Rect& r, const std::string& s, int px, Color c) {
    int w = textWidth(s, px);
    text(r.x + (r.w - w) / 2, r.y + r.h / 2, s, px, c);
}

Rect Canvas::contentBounds() const {
    int x0 = w_, y0 = h_, x1 = -1, y1 = -1;
    for (int y = 0; y < h_; ++y) for (int x = 0; x < w_; ++x)
        if (px_[((size_t)y * w_ + x) * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

bool Canvas::saveRaw(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(px_.data(), 1, px_.size(), f);
    std::fclose(f);
    return true;
}

void Canvas::drawMask(const uint8_t* mask, int mw, int mh, const Rect& dst, Color c) {
    if (!mask || dst.w <= 0 || dst.h <= 0 || mw <= 0 || mh <= 0) return;
    const double sx = (double)mw / dst.w, sy = (double)mh / dst.h;
    for (int y = 0; y < dst.h; ++y) {
        const int py = dst.y + y; if (py < 0 || py >= h_) continue;
        const double fy = (y + 0.5) * sy - 0.5; const int y0 = std::clamp((int)std::floor(fy), 0, mh - 1), y1 = std::min(y0 + 1, mh - 1); const double ty = std::clamp(fy - y0, 0.0, 1.0);
        for (int x = 0; x < dst.w; ++x) {
            const int px = dst.x + x; if (px < 0 || px >= w_) continue;
            const double fx = (x + 0.5) * sx - 0.5; const int x0 = std::clamp((int)std::floor(fx), 0, mw - 1), x1 = std::min(x0 + 1, mw - 1); const double tx = std::clamp(fx - x0, 0.0, 1.0);
            const double a = (mask[y0 * mw + x0] * (1 - tx) + mask[y0 * mw + x1] * tx) * (1 - ty) + (mask[y1 * mw + x0] * (1 - tx) + mask[y1 * mw + x1] * tx) * ty;
            if (a > 0.5) blendPixel(px, py, c, a / 255.0);
        }
    }
}

}  // namespace ui
