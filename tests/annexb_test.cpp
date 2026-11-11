// Regression test for truck test 6: access units that start with an AUD (as h264parse alignment=au emits) must still yield SPS/PPS.
#include "../src/device/AnnexB.h"
#include <cstdio>
#include <string>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static std::string S(std::vector<int> v) { std::string s; for (int x : v) s.push_back((char)x); return s; }
int main() {
    std::string cfg, pic;
    // The real first AU of the truck test: AUD, SPS, PPS, IDR slice
    std::string au = S({0,0,0,1,0x09,0x10, 0,0,0,1,0x67,0x42,0xc0,0x1f,0xda, 0,0,0,1,0x68,0xce,0x3c,0x80, 0,0,1,0x65,0xaa,0xbb,0xcc});
    splitAnnexB((const uint8_t*)au.data(), au.size(), cfg, pic);
    CHECK(cfg == S({0,0,0,1,0x67,0x42,0xc0,0x1f,0xda, 0,0,0,1,0x68,0xce,0x3c,0x80}));
    CHECK(pic == S({0,0,1,0x65,0xaa,0xbb,0xcc}));
    // P-frame with AUD only
    std::string p = S({0,0,0,1,0x09,0x30, 0,0,0,1,0x41,0x9a,0x01});
    splitAnnexB((const uint8_t*)p.data(), p.size(), cfg, pic);
    CHECK(cfg.empty()); CHECK(pic == S({0,0,0,1,0x41,0x9a,0x01}));
    // legacy shape: SPS PPS IDR with no AUD, and SEI before slice
    std::string q = S({0,0,0,1,0x67,0x11, 0,0,0,1,0x68,0x22, 0,0,0,1,0x06,0x05,0x01, 0,0,0,1,0x65,0x33});
    splitAnnexB((const uint8_t*)q.data(), q.size(), cfg, pic);
    CHECK(cfg == S({0,0,0,1,0x67,0x11, 0,0,0,1,0x68,0x22}));
    CHECK(pic == S({0,0,0,1,0x06,0x05,0x01, 0,0,0,1,0x65,0x33}));
    // only an AUD -> nothing
    std::string a = S({0,0,0,1,0x09,0x10});
    splitAnnexB((const uint8_t*)a.data(), a.size(), cfg, pic);
    CHECK(cfg.empty() && pic.empty());
    printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
