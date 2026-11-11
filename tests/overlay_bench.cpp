// Micro-benchmark: cost of ONE overlay refresh (what a PiP drag step does): Menu::render of the window canvas + the blender's opaqueBounds + YUVA conversion.
// g++ -std=c++17 -O2 tests/overlay_bench.cpp src/ui/{Menu,Gfx,FontData,Placeholder,PlaceholderData}.cpp -o overlay_bench
#include <chrono>
#include <cstdio>
#include "../src/compositor/UiBlend.h"
#include "../src/ui/Menu.h"
using namespace ui;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); }
int main(int argc, char** argv) {
    const int W = argc > 1 ? atoi(argv[1]) : 1042, H = argc > 2 ? atoi(argv[2]) : 1080;
    Canvas c(W, H); Menu m(W, H);
    auto mk = [&](Mode mode, bool phone, bool cam) { Model md; md.mode = mode; md.phone = phone; md.camera = cam; md.layout = compute(mode, Prefs(), W, H, 1280, 720, phone, cam); md.cam_view = compute(Mode::FullCam, Prefs(), W, H, 1280, 720, false, true).camera.rect; return md; };
    struct Case { const char* name; Model m; } cases[] = { {"AA placeholder (no phone), pill idle", mk(Mode::FullAA, false, true)}, {"PiP over AA placeholder, live camera", mk(Mode::Pip, false, true)}, {"PiP, live phone + live camera (normal use)", mk(Mode::Pip, true, true)} };
    int64_t t = 1000;
    for (auto& cs : cases) {
        double tr = 0, tb = 0, tc = 0; const int N = 30;
        for (int i = 0; i < N; ++i) {
            auto a = clk::now(); m.noteActivity(t); m.render(c, cs.m, t); tr += ms(a);
            a = clk::now(); int x0, y0, x1, y1; bool any = YuvaLayer::opaqueBounds(c.data(), W, H, x0, y0, x1, y1); tb += ms(a);
            a = clk::now(); if (any) { auto L = YuvaLayer::fromRgba(c.data() + ((size_t)y0 * W + x0) * 4, (x1 - x0) & ~1, (y1 - y0) & ~1, x0, y0, W); (void)L; } tc += ms(a);
        }
        std::printf("%-46s render %6.2f ms   bounds %5.2f ms   convert %6.2f ms   (per refresh, host x86)\n", cs.name, tr / N, tb / N, tc / N);
    }
}
