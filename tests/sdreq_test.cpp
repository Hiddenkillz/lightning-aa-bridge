// truck_mirror_phone: the Pi repeats the real phone's recorded introduction byte for byte to the vehicle. A fake vehicle (HuSession) records what it receives.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>
#include <sys/socket.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f.write(s.data(), (std::streamsize)s.size()); }
static bool run(bool mirror, const char* const* a, const std::string& in, const std::string& out) {
    ::remove(out.c_str());
    Config cfg; cfg.relay_nav = false; cfg.truck_mirror_phone = mirror; cfg.sdreq_file = in;
    cfg.md_cert = a[1]; cfg.md_key = a[2]; cfg.hu_cert = a[3]; cfg.hu_key = a[4]; cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    setenv("AA_TEST_SDREQ_OUT", out.c_str(), 1);
    int S[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, S)) return false;
    HuSession veh(cfg, S[1]); MdSession pi(cfg, S[0], S[0]);
    std::thread t1([&] { veh.run(); }), t2([&] { pi.run(); });
    bool up = waitFor([&] { return pi.videoReady(); });
    veh.stop(); pi.stop(); t1.join(); t2.join();
    return up;
}
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    namespace ctl = aap_protobuf::service::control::message;
    ctl::ServiceDiscoveryRequest real; real.set_device_name("Android Phone"); real.set_label_text("Android"); real.mutable_phone_info()->set_instance_id("00000000-0000-4000-8000-000000000001");
    real.set_small_icon(std::string("\x89PNG\r\n\x1a\n-small-icon-bytes", 24)); real.set_medium_icon(std::string(300, 'm')); real.set_large_icon(std::string(500, 'L'));
    const std::string rec = real.SerializeAsString(), in = "/tmp/sdreq_in.bin", out = "/tmp/sdreq_out.bin";
    put(in, rec);
    CHECK(run(true, argv, in, out), "session up (mirror on)");
    CHECK(slurp(out) == rec, "the vehicle receives the real phone's introduction byte for byte (names, phone info, icons)");
    CHECK(run(false, argv, in, out), "session up (mirror off)");
    ctl::ServiceDiscoveryRequest got; got.ParsePartialFromString(slurp(out));
    CHECK(got.device_name() == "Pixel" && got.label_text() == "AA Bridge", "mirror off: the built-in introduction is used");
    put(in, "");
    CHECK(run(true, argv, in, out), "session up (mirror on, nothing recorded yet)");
    got.Clear(); got.ParsePartialFromString(slurp(out)); CHECK(got.device_name() == "Pixel", "nothing recorded yet: falls back to the built-in introduction");
    printf(fails ? "SDREQ FAILED\n" : "SDREQ ALL PASSED\n"); return fails ? 1 : 0;
}
