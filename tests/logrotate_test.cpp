// g++ -std=c++17 tests/logrotate_test.cpp -o logrotate_test
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "../src/LogRotate.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static long long sz(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0 ? st.st_size : -1; }
int main() {
    const std::string p = "/tmp/logrotate-test.log"; std::remove(p.c_str()); std::remove((p + ".1").c_str());
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);                       // like systemd's StandardOutput=append:
    std::string line(99, 'x'); line += '\n';
    for (int i = 0; i < 1000; ++i) CHECK(write(fd, line.data(), line.size()) == (ssize_t)line.size());   // 100 000 bytes
    CHECK(!logrotate::rotateIfLarge(fd, 200000, p) && sz(p) == 100000 && sz(p + ".1") < 0);               // under the limit: untouched
    CHECK(!logrotate::rotateIfLarge(fd, 0, p));                                                          // 0 = never
    CHECK(logrotate::rotateIfLarge(fd, 50000, p));                                                       // over: rotated
    CHECK(sz(p + ".1") == 100000 && sz(p) == 0);
    CHECK(write(fd, "after\n", 6) == 6); CHECK(sz(p) == 6);                                                // writer continues at the start, no sparse hole
    { std::ifstream f(p); std::string s; std::getline(f, s); CHECK(s == "after"); }
    for (int i = 0; i < 600; ++i) CHECK(write(fd, line.data(), line.size()) == (ssize_t)line.size());   // second rotation replaces the old .1
    CHECK(logrotate::rotateIfLarge(fd, 50000, p)); CHECK(sz(p + ".1") == 60006 && sz(p) == 0);
    CHECK(sz(p + ".1.tmp") < 0);                                                                          // no temp file left behind
    for (int i = 0; i < 700; ++i) (void)!write(fd, line.data(), line.size());                            // path taken from /proc/self/fd (production)
    CHECK(logrotate::rotateIfLarge(fd, 50000)); CHECK(sz(p + ".1") == 70000);
    int pf[2]; CHECK(pipe(pf) == 0); CHECK(!logrotate::rotateIfLarge(pf[1], 1)); close(pf[0]); close(pf[1]);   // not a regular file: ignored
    close(fd); std::remove(p.c_str()); std::remove((p + ".1").c_str());
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails); return fails != 0;
}
