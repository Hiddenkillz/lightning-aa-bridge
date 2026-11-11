// Vehicle sensor relay through four real sessions:
//   fake vehicle (HuSession as Sync 4, offers speed/fuel/brake/gear) <-A-> Pi MdSession --callbacks--> Pi HuSession <-B-> fake phone (MdSession)
// The phone must be OFFERED the vehicle's sensors, subscribe, and then receive the vehicle's speed / gear / parking brake / fuel; driving status stays ours.
#include "../src/device/MdSession.h"
#include "../src/headunit/HuSession.h"
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
namespace sns = aap_protobuf::service::sensorsource::message;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } else printf("ok:   %s\n", m); } while (0)
template <class F> static bool waitFor(F f, int ms = 8000) { for (int i = 0; i < ms / 20 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return f(); }
int main(int argc, char** argv) {
    Config cfg; cfg.relay_sensors = true; cfg.relay_nav = false;
    cfg.md_cert = argc > 1 ? argv[1] : "/etc/aa-bridge/md_cert.pem"; cfg.md_key = argc > 2 ? argv[2] : "/etc/aa-bridge/md_key.pem";
    cfg.hu_cert = argc > 3 ? argv[3] : "/etc/aa-bridge/hu_cert.pem"; cfg.hu_key = argc > 4 ? argv[4] : "/etc/aa-bridge/hu_key.pem";
    cfg.md_ca.clear(); cfg.md_dhparam.clear(); cfg.truck_silence_s = 0; cfg.truck_silence_start_s = 0;
    int A[2], B[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, A) || socketpair(AF_UNIX, SOCK_STREAM, 0, B)) { perror("socketpair"); return 2; }
    HuSession truck(cfg, A[1]); MdSession piTruck(cfg, A[0], A[0]); HuSession piPhone(cfg, B[1]); MdSession phone(cfg, B[0], B[0]);
    truck.setVehicleSensorTypes({3, 6, 7, 8, 23, 24}); truck.setVehicleSensorCaps({10}, {4, 1});            // an electric truck: CCS1 + J1772, and two sensor types newer than our protobuf library                       // what 'Sync 4' offers on top of driving status / night / location
    std::mutex m; std::atomic<int> speed{-1}, gear{-99}, brake{-1}, advertised{0}; std::atomic<int> drv_msgs{0};
    phone.onVehicle = [&](int what, int v) { if (what == 5) speed = v; else if (what == 3) gear = v; else if (what == 4) brake = v; else if (what == 6) ++drv_msgs; };
    std::vector<int> offeredToPi; std::vector<int> fuelPi, evPi, fuelPhone, evPhone; std::mutex raw_m; std::string phoneRaw;
    piTruck.onSensorCaps = [&](const std::vector<int>& f, const std::vector<int>& e) { fuelPi = f; evPi = e; piPhone.setVehicleSensorCaps(f, e); };
    phone.onSensorCaps = [&](const std::vector<int>& f, const std::vector<int>& e) { fuelPhone = f; evPhone = e; };
    phone.onSensorRaw = [&](const std::string& r) { std::lock_guard<std::mutex> l(raw_m); phoneRaw += r; };
    piTruck.onSensorTypes = [&](const std::vector<int>& t) { offeredToPi = t; piPhone.setVehicleSensorTypes(t); };     // same wiring as main.cpp (Device -> HeadUnit)
    piTruck.onSensorRaw = [&](const std::string& raw) { piPhone.relayVehicleSensors(raw); };
    piPhone.setVehicleSensorTypes({3, 6, 7, 8, 23, 24}); piPhone.setVehicleSensorCaps({10}, {4, 1});          // in the truck the vehicle session is up (and known) before the phone connects: HeadUnit hands these to every new phone session
    std::thread t1([&] { truck.run(); }), t2([&] { piTruck.run(); }), t3([&] { piPhone.run(); }), t4([&] { phone.run(); });
    CHECK(waitFor([&] { return piTruck.videoReady() && phone.videoReady(); }), "both sessions are up");
    auto has = [&](int t) { for (int x : offeredToPi) if (x == t) return true; return false; };
    CHECK(has(3) && has(6) && has(7) && has(8), "the Pi saw the vehicle offer speed, fuel, parking brake and gear");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));       // sensor subscriptions settle on both hops
    auto send = [&](int spd_e3, int g, bool pb) { sns::SensorBatch b; b.add_speed_data()->set_speed_e3(spd_e3); b.add_gear_data()->set_gear((sns::Gear)g); b.add_parking_brake_data()->set_parking_brake(pb);
        truck.sendSensorBatchRaw(b.SerializeAsString()); };
    send(13410, 4 /*drive*/, false);
    CHECK(waitFor([&] { return speed == 13410; }), "vehicle SPEED reaches the phone (13.41 m/s)");
    CHECK(gear == 4, "vehicle GEAR reaches the phone");
    CHECK(brake == 0, "vehicle PARKING BRAKE reaches the phone");
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); send(0, 1 /*park*/, true);
    CHECK(waitFor([&] { return speed == 0 && brake == 1; }), "later readings follow (stopped, brake on)");
    const int before = speed; send(9999, 4, true); send(8888, 4, true);       // two readings inside the 200 ms speed limit: at most the first may pass
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(speed == 9999 || speed == before || speed == 8888, "rate limiting does not corrupt the value");
    CHECK(has(23) && has(24), "the Pi saw the vehicle offer the two newer sensor types (23, 24)");
    CHECK(fuelPi == std::vector<int>{10} && evPi == std::vector<int>({4, 1}), "the vehicle's electric / CCS1 / J1772 declaration reached the Pi");
    CHECK(waitFor([&] { return !fuelPhone.empty(); }) && fuelPhone == std::vector<int>{10} && evPhone == std::vector<int>({4, 1}), "the phone is told the vehicle is electric with the same connectors");
    { std::string raw23; raw23.push_back((char)0xba); raw23.push_back((char)0x01); raw23.push_back((char)0x03); raw23.push_back((char)0x08); raw23.push_back((char)0x2a); raw23.push_back((char)0x10);     // field 23, 3 bytes: {1:42, 2:...}
      truck.sendSensorBatchRaw(raw23);
      CHECK(waitFor([&] { std::lock_guard<std::mutex> l(raw_m); return phoneRaw.find(raw23) != std::string::npos; }), "sensor type 23 data reaches the phone byte-exact (raw relay of a type our library cannot parse)"); }
    truck.stop(); piTruck.stop(); piPhone.stop(); phone.stop(); t1.join(); t2.join(); t3.join(); t4.join();
    printf(fails ? "SENSOR E2E FAILED\n" : "SENSOR E2E ALL PASSED\n"); return fails ? 1 : 0;
}
