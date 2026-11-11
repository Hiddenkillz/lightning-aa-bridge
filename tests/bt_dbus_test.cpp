// Runs the real BtBootstrap against a fake BlueZ (tests/fake_bluez.py) on a private D-Bus for N seconds, so the shell test can look at its log.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "../src/headunit/BtBootstrap.h"
int main(int argc, char** argv) {
    Config c; c.bt_bootstrap = true; c.ap_password = "testpassword"; c.ap_iface = "lo";
    if (std::getenv("AA_TEST_RELEASE")) c.bt_release_phone = true;
    if (const char* p = std::getenv("AA_TEST_PHONE_PROFILE")) c.bt_phone_profile = p;
    if (const char* k = std::getenv("AA_TEST_HF_KEEP_S")) c.bt_hf_keep_s = atoi(k);
    BtBootstrap bt(c); bt.start();
    if (std::getenv("AA_TEST_PHONE_ACTIVE")) bt.setPhoneActive(true);
    std::this_thread::sleep_for(std::chrono::seconds(argc > 1 ? atoi(argv[1]) : 30));
    bt.stop();
    std::printf("[test] stopped cleanly\n");
    return 0;
}
