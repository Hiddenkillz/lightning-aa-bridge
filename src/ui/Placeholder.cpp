#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"
#include "Placeholder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <tuple>

namespace ui {

const Bitmap& placeholderSource(Art a) {
    static Bitmap bm[2]; static bool done[2] = {false, false};
    const int i = a == Art::AndroidAuto ? 0 : 1;
    if (!done[i]) {
        done[i] = true;
        const unsigned char* data = a == Art::AndroidAuto ? kArtAndroidAuto : kArtCamera;
        const unsigned long n = a == Art::AndroidAuto ? kArtAndroidAutoSize : kArtCameraSize;
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load_from_memory(data, (int)n, &w, &h, &ch, 4);
        if (px) { bm[i].w = w; bm[i].h = h; bm[i].rgba.assign(px, px + (size_t)w * h * 4); stbi_image_free(px); }
    }
    return bm[i];
}

std::vector<uint8_t> scaleCover(const Bitmap& src, int dw, int dh) {
    std::vector<uint8_t> out((size_t)std::max(0, dw) * std::max(0, dh) * 4, 0);
    if (src.w <= 0 || src.h <= 0 || dw <= 0 || dh <= 0) return out;
    const double s = std::max((double)dw / src.w, (double)dh / src.h);          // source -> destination scale (cover)
    const double bw = dw / s, bh = dh / s;                                      // source box that maps onto the destination
    const double sx0 = (src.w - bw) / 2.0, sy0 = (src.h - bh) / 2.0;
    auto at = [&](int x, int y, int k) -> double {
        x = std::clamp(x, 0, src.w - 1); y = std::clamp(y, 0, src.h - 1); return src.rgba[((size_t)y * src.w + x) * 4 + k];
    };
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint8_t* o = &out[((size_t)y * dw + x) * 4];
            if (s >= 1.0) {                                                     // enlarging: bilinear at the pixel centre
                double fx = sx0 + (x + 0.5) / s - 0.5, fy = sy0 + (y + 0.5) / s - 0.5;
                int ix = (int)std::floor(fx), iy = (int)std::floor(fy); double tx = fx - ix, ty = fy - iy;
                for (int k = 0; k < 3; ++k)
                    o[k] = (uint8_t)std::lround((at(ix, iy, k) * (1 - tx) + at(ix + 1, iy, k) * tx) * (1 - ty) + (at(ix, iy + 1, k) * (1 - tx) + at(ix + 1, iy + 1, k) * tx) * ty);
            } else {                                                            // shrinking: average the source box (no aliasing)
                int x0 = (int)std::floor(sx0 + x / s), x1 = std::max(x0 + 1, (int)std::ceil(sx0 + (x + 1) / s));
                int y0 = (int)std::floor(sy0 + y / s), y1 = std::max(y0 + 1, (int)std::ceil(sy0 + (y + 1) / s));
                double acc[3] = {0, 0, 0}; int n = 0;
                for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { for (int k = 0; k < 3; ++k) acc[k] += at(xx, yy, k); ++n; }
                for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::lround(acc[k] / n);
            }
            o[3] = 255;
        }
    }
    return out;
}

static void roundCorners(std::vector<uint8_t>& px, int w, int h, int radius) {
    radius = std::min(radius, std::min(w, h) / 2);
    if (radius <= 0) return;
    auto cover = [&](double cx, double cy, double x, double y) { return std::clamp(0.5 - (std::hypot(x - cx, y - cy) - radius), 0.0, 1.0); };
    for (int j = 0; j < radius; ++j)
        for (int i = 0; i < radius; ++i) {
            const double cxs[4] = {(double)radius, (double)(w - radius), (double)radius, (double)(w - radius)};
            const double cys[4] = {(double)radius, (double)radius, (double)(h - radius), (double)(h - radius)};
            const int xs[4] = {i, w - 1 - i, i, w - 1 - i}, ys[4] = {j, j, h - 1 - j, h - 1 - j};
            for (int q = 0; q < 4; ++q) {
                double a = cover(cxs[q], cys[q], xs[q] + 0.5, ys[q] + 0.5);
                px[((size_t)ys[q] * w + xs[q]) * 4 + 3] = (uint8_t)std::lround(255 * a);
            }
        }
}

const std::vector<uint8_t>& placeholderCover(Art a, int w, int h, int radius) {
    using Key = std::tuple<int, int, int, int>;
    struct Entry { Key key; std::vector<uint8_t> px; };
    static std::deque<Entry> cache;
    const Key key{(int)a, w, h, radius};
    for (auto& e : cache) if (e.key == key) return e.px;
    Entry e{key, scaleCover(placeholderSource(a), w, h)};
    roundCorners(e.px, w, h, radius);
    cache.push_back(std::move(e));
    while (cache.size() > 8) cache.pop_front();
    return cache.back().px;
}

}  // namespace ui
