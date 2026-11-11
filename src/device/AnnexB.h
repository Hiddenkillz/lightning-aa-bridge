#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Split an Annex-B access unit into parameter sets (SPS/PPS, NAL 7/8) and picture data (everything else).
// Access-unit delimiters (NAL 9) are dropped: h264parse alignment=au emits one first in EVERY access unit, and the
// old "SPS/PPS must be leading" logic mistook that for "no config" and silently discarded every frame (truck test 6).
inline void splitAnnexB(const uint8_t* d, size_t n, std::string& cfg, std::string& pic) {
    cfg.clear(); pic.clear();
    std::vector<size_t> starts;                       // offset of each start code's first byte
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) { starts.push_back((i > 0 && d[i-1] == 0) ? i - 1 : i); i += 2; }
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t s = starts[k], e = (k + 1 < starts.size()) ? starts[k+1] : n;
        size_t h = s; while (h < e && d[h] == 0) ++h; ++h;           // skip zeros + the 0x01 -> first NAL header byte
        if (h >= e) continue;
        int t = d[h] & 0x1f;
        if (t == 9) continue;                                         // AUD
        std::string& dst = (t == 7 || t == 8) ? cfg : pic;
        dst.append((const char*)d + s, e - s);
    }
}

