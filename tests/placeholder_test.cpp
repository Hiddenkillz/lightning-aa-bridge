// g++ -std=c++17 -O1 tests/placeholder_test.cpp src/ui/Placeholder.cpp src/ui/PlaceholderData.cpp -o placeholder_test
#include <cstdio>
#include <string>
#include "../src/ui/Placeholder.h"
using namespace ui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void ppm(const char* name, const std::vector<uint8_t>& rgba, int w, int h) {
    std::string path = std::string("/tmp/ph-") + name + ".ppm"; FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i) { double a = rgba[i*4+3] / 255.0; for (int k = 0; k < 3; ++k) { uint8_t v = (uint8_t)(rgba[i*4+k] * a + 90 * (1 - a)); std::fputc(v, f); } }
    std::fclose(f);
}
int main() {
    for (Art a : {Art::AndroidAuto, Art::Camera}) {
        const Bitmap& b = placeholderSource(a);
        CHECK(b.w == 1280 && b.h == 720 && b.rgba.size() == (size_t)1280 * 720 * 4);
        const char* n = a == Art::AndroidAuto ? "aa" : "cam";
        auto big = placeholderCover(a, 1042, 1080, 0);                  // the Sync 4 window at 1080p: enlarge + crop sides
        CHECK(big.size() == (size_t)1042 * 1080 * 4 && big[3] == 255);
        ppm((std::string(n) + "-window").c_str(), big, 1042, 1080);
        auto small = placeholderCover(a, 312, 234, 20);                 // a PiP: shrink + rounded corners
        CHECK(small[3] == 0 && small[((size_t)117 * 312 + 156) * 4 + 3] == 255);   // corner pixel transparent, centre opaque
        ppm((std::string(n) + "-pip").c_str(), small, 312, 234);
        auto wide = placeholderCover(a, 640, 360, 0);                   // same aspect: straight downscale
        ppm((std::string(n) + "-wide").c_str(), wide, 640, 360);
    }
    // the centre of the AA art is the logo (blue-ish), the corner is near black
    auto aa = placeholderCover(Art::AndroidAuto, 640, 360, 0);
    const uint8_t* ctr = &aa[((size_t)200 * 640 + 320) * 4]; const uint8_t* cor = &aa[0];
    CHECK(ctr[2] > 80 && cor[0] < 25 && cor[1] < 25 && cor[2] < 30);
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
