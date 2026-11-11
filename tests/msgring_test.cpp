// g++ -std=c++17 -pthread -Isrc tests/msgring_test.cpp -o msgring_test && ./msgring_test
#include "MsgRing.h"
static int fails = 0;
#define CHK(c, m) do { if (!(c)) { ++fails; std::cout << "FAIL: " << m << std::endl; } else std::cout << "ok: " << m << std::endl; } while (0)
int main() {
    MsgRing r; CHK(r.dump("T").find("(none)") != std::string::npos, "empty ring says so");
    for (int i = 0; i < 40; ++i) r.push(i % 2, 3, 0x8000 + i, 10 + i);
    const std::string d = r.dump("T");
    CHK(r.total() == 40, "counts every message");
    CHK(d.find("last 16 messages") != std::string::npos, "keeps only the last 16");
    CHK(d.find("0x8027") != std::string::npos && d.find("0x8017") == std::string::npos && d.find("0x8018") != std::string::npos, "newest present, oldest dropped, boundary exact");
    CHK(d.find("0x8018") < d.find("0x8027"), "oldest first");
    std::cout << (fails ? "MSGRING TEST FAILED" : "MSGRING ALL PASSED") << std::endl; return fails ? 1 : 0;
}
