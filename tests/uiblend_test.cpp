// UiBlend: RGBA -> YUVA conversion and blending into I420, against a straightforward float reference.
// g++ -std=c++17 -O2 tests/uiblend_test.cpp -o uiblend_test
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "../src/compositor/UiBlend.h"
using ui::YuvaLayer;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

struct Frame { int w, h; std::vector<uint8_t> Y, U, V; Frame(int w_, int h_, uint8_t y, uint8_t u, uint8_t v) : w(w_), h(h_), Y((size_t)w_ * h_, y), U((size_t)w_ / 2 * (h_ / 2), u), V((size_t)w_ / 2 * (h_ / 2), v) {} };
static void blend(const YuvaLayer& L, Frame& f) { L.blendInto(f.Y.data(), f.w, f.U.data(), f.w / 2, f.V.data(), f.w / 2, f.w, f.h); }

int main() {
    // 1. fully transparent layer changes nothing; summaries say so
    { std::vector<uint8_t> z(16 * 16 * 4, 0); YuvaLayer L = YuvaLayer::fromRgba(z.data(), 16, 16, 4, 4); Frame f(64, 48, 100, 90, 160); Frame g = f; blend(L, f); CHECK(f.Y == g.Y && f.U == g.U && f.V == g.V); CHECK(L.yrows[3].x1 <= L.yrows[3].x0); }
    // 2. fully opaque white: Y=235 U=V=128 inside the rect (BT.601 limited), untouched outside; opaque rows take the memcpy path
    { std::vector<uint8_t> w(16 * 16 * 4, 255); YuvaLayer L = YuvaLayer::fromRgba(w.data(), 16, 16, 8, 6); Frame f(64, 48, 0, 0, 0); blend(L, f);
      CHECK(f.Y[(size_t)10 * 64 + 12] == 235 && f.U[(size_t)5 * 32 + 6] == 128 && f.V[(size_t)5 * 32 + 6] == 128); CHECK(f.Y[(size_t)5 * 64 + 12] == 0 && f.Y[(size_t)10 * 64 + 7] == 0 && f.Y[(size_t)10 * 64 + 24] == 0);
      CHECK(L.yrows[0].opaque && L.yrows[0].x0 == 0 && L.yrows[0].x1 == 16); }
    // 3. half alpha black over mid grey: about half way, in Y and chroma
    { std::vector<uint8_t> p(16 * 16 * 4); for (size_t i = 0; i < p.size(); i += 4) { p[i] = p[i + 1] = p[i + 2] = 0; p[i + 3] = 128; } YuvaLayer L = YuvaLayer::fromRgba(p.data(), 16, 16, 0, 0); Frame f(32, 32, 130, 200, 60); blend(L, f);
      CHECK(std::abs((int)f.Y[5 * 32 + 5] - (130 * 127 + 16 * 128) / 255) <= 1); CHECK(std::abs((int)f.U[3 * 16 + 3] - (200 * 127 + 128 * 128) / 255) <= 1); CHECK(!L.yrows[0].opaque); }
    // 4. random layers vs a float reference (straight alpha, BT.601): Y within 2, chroma within 3
    { std::mt19937 rng(7); int worstY = 0, worstC = 0;
      for (int t = 0; t < 40; ++t) { const int w = 2 * (4 + rng() % 40), h = 2 * (4 + rng() % 30); std::vector<uint8_t> p((size_t)w * h * 4);
          for (size_t i = 0; i < p.size(); i += 4) { p[i] = rng(); p[i + 1] = rng(); p[i + 2] = rng(); const int m = rng() % 4; p[i + 3] = m == 0 ? 0 : m == 1 ? 255 : (uint8_t)rng(); }
          const int fx = 2 * (rng() % 10), fy = 2 * (rng() % 10); YuvaLayer L = YuvaLayer::fromRgba(p.data(), w, h, fx, fy); Frame f(120, 100, 77, 111, 143), base = f; blend(L, f);
          for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) { const uint8_t* q = &p[((size_t)j * w + i) * 4]; const double a = q[3] / 255.0;
              const double Ys = 16 + 0.2578 * q[0] + 0.5039 * q[1] + 0.0977 * q[2]; const double exp = base.Y[(size_t)(fy + j) * 120 + fx + i] * (1 - a) + Ys * a; worstY = std::max(worstY, (int)std::abs(std::round(exp) - f.Y[(size_t)(fy + j) * 120 + fx + i])); }
          for (int j = 0; j < h / 2; ++j) for (int i = 0; i < w / 2; ++i) { double r = 0, g = 0, b = 0, a = 0; for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) { const uint8_t* q = &p[((size_t)(2 * j + dy) * w + 2 * i + dx) * 4]; r += q[0]; g += q[1]; b += q[2]; a += q[3]; }
              r /= 4; g /= 4; b /= 4; a /= 4 * 255.0; const double Us = 128 - 0.1484 * r - 0.2891 * g + 0.4375 * b; const size_t k = (size_t)(fy / 2 + j) * 60 + fx / 2 + i; const double exp = base.U[k] * (1 - a) + Us * a;
              worstC = std::max(worstC, (int)std::abs(std::round(exp) - f.U[k])); } }
      std::printf("random layers: worst error Y %d, U %d\n", worstY, worstC); CHECK(worstY <= 2 && worstC <= 3); }
    // 5. clipping: a layer hanging over every edge of the frame must not touch memory outside it (guard bands stay intact)
    { const int FW = 32, FH = 32; std::vector<uint8_t> w(24 * 24 * 4, 255);
      for (auto pos : std::vector<std::pair<int, int>>{{-10, -10}, {20, 20}, {-10, 20}, {20, -10}, {-30, 0}, {40, 0}, {0, 40}}) {
          YuvaLayer L = YuvaLayer::fromRgba(w.data(), 24, 24, pos.first, pos.second);
          std::vector<uint8_t> Yb(FW * FH + 64, 77), Ub(FW / 2 * FH / 2 + 64, 77), Vb(FW / 2 * FH / 2 + 64, 77); L.blendInto(Yb.data() + 32, FW, Ub.data() + 32, FW / 2, Vb.data() + 32, FW / 2, FW, FH);
          bool guards = true; for (int i = 0; i < 32; ++i) { guards &= Yb[i] == 77 && Ub[i] == 77 && Vb[i] == 77; } for (size_t i = FW * FH + 32; i < Yb.size(); ++i) guards &= Yb[i] == 77; for (size_t i = FW / 2 * FH / 2 + 32; i < Ub.size(); ++i) guards &= Ub[i] == 77 && Vb[i] == 77;
          CHECK(guards); } }
    // 6. odd sizes/positions are rounded to even, empty input gives an empty layer
    { std::vector<uint8_t> w(15 * 15 * 4, 255); YuvaLayer L = YuvaLayer::fromRgba(w.data(), 15, 15, 3, 5); CHECK(L.w == 14 && L.h == 14 && L.x == 2 && L.y == 4); CHECK(YuvaLayer::fromRgba(nullptr, 8, 8, 0, 0).empty()); CHECK(YuvaLayer::fromRgba(w.data(), 1, 1, 0, 0).empty()); }
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails); return fails != 0;
}
