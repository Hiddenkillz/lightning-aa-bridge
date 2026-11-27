// Settings > OBD adapter: the page, its buttons, saving the chosen adapter, handing it to the poller, and the BlueZ helpers (names, phone filter, ordering).
// Built by CMake as obd-pair-test. A fake ObdPairer stands in for BlueZ; UiController needs GStreamer only because it takes a Compositor (never started).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <gst/gst.h>
#include "../src/Config.h"
#include "../src/compositor/Compositor.h"
#include "../src/headunit/BtObdPairer.h"
#include "../src/ui/UiController.h"
using namespace ui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }
static std::string slurp(const std::string& p) { std::ifstream f(p); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

struct FakePairer : ObdPairer {
    std::mutex m; ObdPairState st; int scans = 0, stops = 0; std::vector<std::string> paired, forgot;
    void startScan() override { std::lock_guard<std::mutex> l(m); ++scans; st.phase = ObdPairState::Scanning; st.message = "Searching"; ++st.serial; }
    void stopScan() override { std::lock_guard<std::mutex> l(m); ++stops; if (st.phase == ObdPairState::Scanning) { st.phase = ObdPairState::Idle; ++st.serial; } }
    void pair(const std::string& a) override { std::lock_guard<std::mutex> l(m); paired.push_back(a); st.phase = ObdPairState::Pairing; st.message = "Pairing"; ++st.serial; }
    void forget(const std::string& a) override { std::lock_guard<std::mutex> l(m); forgot.push_back(a); st.phase = ObdPairState::Idle; st.paired_addr.clear(); ++st.serial; }
    ObdPairState state() override { std::lock_guard<std::mutex> l(m); return st; }
    void devices(std::vector<ObdPairDevice> d) { std::lock_guard<std::mutex> l(m); st.devices = std::move(d); ++st.serial; }
    void finish(const std::string& a) { std::lock_guard<std::mutex> l(m); st.phase = ObdPairState::Done; st.paired_addr = a; st.message = "Paired"; ++st.serial; }
};

static void tap(UiController& ui, int id) {
    const Rect r = ui.buttonRect(id);
    if (r.w <= 0) return;
    const int x = r.x + r.w / 2, y = r.y + r.h / 2;
    ui.onTouch(x, y, 0); wait(120); ui.onTouch(x, y, 1); wait(350);
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    // ---- BtObdPairer helpers
    CHECK(BtObdPairer::looksLikeObdAdapter("OBDLink LX") && BtObdPairer::looksLikeObdAdapter("V-LINK") && BtObdPairer::looksLikeObdAdapter("ELM327"));
    CHECK(!BtObdPairer::looksLikeObdAdapter("Pixel 9") && !BtObdPairer::looksLikeObdAdapter("JBL Flip") && !BtObdPairer::looksLikeObdAdapter(""));
    CHECK(BtObdPairer::looksLikePhone({"0000110a-0000-1000-8000-00805f9b34fb"}) && BtObdPairer::looksLikePhone({"00001101-0000-1000-8000-00805f9b34fb", "4de17a00-52cb-11e6-bdf4-0800200c9a66"}));
    CHECK(!BtObdPairer::looksLikePhone({"00001101-0000-1000-8000-00805f9b34fb"}) && !BtObdPairer::looksLikePhone({}));
    { std::vector<ObdPairDevice> v = {{"Speaker", "A", -40, false}, {"OBDLink LX", "B", -80, true}, {"Thing", "C", 0, false}, {"Watch", "D", -60, false}};
      BtObdPairer::sortDevices(v);
      CHECK(v[0].addr == "B" && v[1].addr == "A" && v[2].addr == "D" && v[3].addr == "C"); }   // OBD-like first, then strongest signal, unknown signal last

    const std::string cf = "/tmp/obd-pair-test.conf", pf = "/tmp/obd-pair-test-prefs.conf";
    const std::string A1 = "00:04:3E:AA:BB:01", A2 = "00:04:3E:AA:BB:02";
    put(cf, "camera_enable=0\nobd_enable=1\n");
    put(pf, "start_mode=aa\n");
    Config c = Config::load(cf); c.prefs_path = pf;
    CHECK(c.obd_enable && !c.camera_enable);

    {   // ---- the page, end to end
        auto ui = std::make_shared<UiController>(c, std::make_shared<Compositor>(c));
        auto fake = std::make_shared<FakePairer>();
        std::vector<std::string> ports;
        ui->setObdPairer(fake);
        ui->setObdAdapterHandler([&](const std::string& p) { ports.push_back(p); });
        ui->start();
        CHECK(ports.size() == 1 && ports[0].empty());                       // nothing saved yet: the poller keeps auto-discovery
        ui->command("menu settings"); wait(300);
        CHECK(ui->screen() == Screen::Settings);
        CHECK(ui->buttonRect(ID_OBD_OPEN).w > 0);                           // the row exists (obd_enable)
        tap(*ui, ID_OBD_OPEN);
        CHECK(ui->screen() == Screen::ObdPair && fake->scans == 1);
        CHECK(ui->buttonRect(ID_OBD_SCAN).w > 0 && ui->buttonRect(ID_OBD_FORGET).w > 0 && ui->buttonRect(ID_OBD_DEV0).w == 0);
        fake->devices({{"OBDLink LX", A1, -55, true}, {"Headset", A2, -70, false}});
        wait(300);
        CHECK(ui->buttonRect(ID_OBD_DEV0).w > 0 && ui->buttonRect(ID_OBD_DEV0 + 1).w > 0 && ui->buttonRect(ID_OBD_DEV0 + 2).w == 0);
        tap(*ui, ID_OBD_DEV0);
        CHECK(fake->paired.size() == 1 && fake->paired[0] == A1);           // the tapped row, by the address that was on screen
        CHECK(ui->prefs().obd_addr.empty());                                // not adopted until the pairing finishes
        fake->finish(A1); wait(300);
        CHECK(ui->prefs().obd_addr == A1);
        CHECK(ports.size() == 2 && ports[1] == "bt:" + A1);                 // the poller is told to use it
        ui->command("save"); wait(300);
        CHECK(slurp(pf).find("obd_addr=" + A1) != std::string::npos);
        tap(*ui, ID_BACK);
        CHECK(ui->screen() == Screen::Settings && fake->stops >= 1);
        tap(*ui, ID_OBD_OPEN); wait(100);
        tap(*ui, ID_OBD_FORGET);
        CHECK(fake->forgot.size() == 1 && fake->forgot[0] == A1 && ui->prefs().obd_addr.empty());
        CHECK(ports.size() == 3 && ports[2].empty());                       // back to auto-discovery
        ui->stop();
    }
    {   // ---- the saved choice is reloaded and handed to the poller at start-up; junk in the prefs file is ignored
        put(pf, "start_mode=aa\nobd_addr=" + A2 + "\n");
        auto ui = std::make_shared<UiController>(c, std::make_shared<Compositor>(c));
        std::vector<std::string> ports; ui->setObdAdapterHandler([&](const std::string& p) { ports.push_back(p); });
        CHECK(ports.size() == 1 && ports[0] == "bt:" + A2);
        ui->stop();
        put(pf, "start_mode=aa\nobd_addr=not-an-address\n");
        UiController ui2(c, std::make_shared<Compositor>(c)); CHECK(ui2.prefs().obd_addr.empty());
    }
    {   // ---- obd_port in the config file wins: the page cannot change it
        put(pf, "start_mode=aa\nobd_addr=" + A2 + "\n");
        Config f = c; f.obd_port = "/dev/ttyUSB0";
        auto ui = std::make_shared<UiController>(f, std::make_shared<Compositor>(f));
        auto fake = std::make_shared<FakePairer>(); std::vector<std::string> ports;
        ui->setObdPairer(fake); ui->setObdAdapterHandler([&](const std::string& p) { ports.push_back(p); });
        ui->start();
        CHECK(ports.size() == 1 && ports[0] == "/dev/ttyUSB0");
        ui->command("menu settings"); wait(300);
        tap(*ui, ID_OBD_OPEN);
        CHECK(ui->screen() == Screen::Settings && fake->scans == 0);       // the button is disabled
        ui->stop();
    }
    {   // ---- OBD off: no row at all
        Config n = c; n.obd_enable = false;
        UiController ui(n, std::make_shared<Compositor>(n));
        ui.start();
        ui.command("menu settings"); wait(300);
        CHECK(ui.buttonRect(ID_OBD_OPEN).w == 0);
        ui.stop();
    }
    std::printf(fails ? "OBD PAIR TEST FAILED (%d)\n" : "OBD PAIR TEST PASSED (ALL PASSED)\n", fails);
    return fails ? 1 : 0;
}
