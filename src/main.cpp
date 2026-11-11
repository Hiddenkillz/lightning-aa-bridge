#include <iostream>
#include <memory>
#include <thread>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <signal.h>
#include <gst/gst.h>

#include "headunit/HeadUnit.h"
#include "headunit/BtBootstrap.h"
#include "btcall/AaCall.h"
#include "headunit/WifiWatch.h"
#include "Watchdog.h"
#include "NavRelay.h"
#include "RawRelay.h"
#include "AvRelay.h"
#include "NavDecode.h"
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "ThreadCpu.h"
#include "device/Device.h"
#include "compositor/Compositor.h"
#include "ui/UiController.h"
#include "obd/Obd.h"
#include "Config.h"
#include "LogRotate.h"
#include "Trace.h"

static volatile bool g_running = true;

void sighandler(int) {
    g_running = false;
}

#include <execinfo.h>
#include <unistd.h>
#include <thread>
static void crashhandler(int s) {
    void* bt[40]; int n = backtrace(bt, 40);
    dprintf(2, "[aa-bridge] FATAL signal %d\n", s);
    backtrace_symbols_fd(bt, n, 2);
    sync(); _exit(128 + s);
}
int main(int argc, char* argv[]) {
    signal(SIGSEGV, crashhandler); signal(SIGABRT, crashhandler); signal(SIGBUS, crashhandler);
    std::thread([]{ for (;;) { sleep(1); sync(); } }).detach();
    // Power timeline. The Pi's firmware reports "undervoltage now" through the rpi_volt hwmon (in0_lcrit_alarm, 1 while the rail is below 4.63 V). Polled once a second;
    // only transitions are logged, with a running count, so a brown-out that coincides with a phone drop or a quiet truck shows up next to it in the log.
    std::thread([] {
        std::string alarm;
        if (DIR* d = opendir("/sys/class/hwmon")) { while (dirent* e = readdir(d)) { if (e->d_name[0] == '.') continue; const std::string h = std::string("/sys/class/hwmon/") + e->d_name;
            std::ifstream n(h + "/name"); std::string nm; std::getline(n, nm); if (nm == "rpi_volt") alarm = h + "/in0_lcrit_alarm"; } closedir(d); }
        if (alarm.empty()) { std::cout << "[Power] no rpi_volt hwmon: undervoltage timeline unavailable" << std::endl; return; }
        std::cout << "[Power] undervoltage monitor on " << alarm << std::endl;
        int last = -1; long events = 0;
        for (;;) { std::ifstream f(alarm); int v = -1; f >> v; if (v >= 0 && v != last) { if (v == 1) { ++events; std::cout << "[Power] UNDERVOLTAGE detected (rail below 4.63 V), event #" << events << std::endl; } else if (last == 1) std::cout << "[Power] voltage back to normal" << std::endl; last = v; } sleep(1); }
    }).detach();
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    gst_init(&argc, &argv);

    std::cout << "[aa-bridge] Starting up" << std::endl;

    Config config = Config::load("/etc/aa-bridge/aa-bridge.conf");
    trace::enabled = config.log_trace != 0;
    if (config.log_max_mb > 0) std::thread([mb = config.log_max_mb] {
        for (;;) { sleep(30); if (logrotate::rotateIfLarge(1, (long long)mb * 1024 * 1024)) std::cout << "[aa-bridge] log rotated (previous part kept as aa-bridge.log.1)" << std::endl; }
    }).detach();

    // Compositor: owns the GStreamer pipeline
    // Receives phone H.264 frames, overlays camera, outputs composed H.264
    auto compositor = std::make_shared<Compositor>(config);

    // HeadUnit: AA session toward the phone (Pi acts as head unit)
    // Receives phone video -> feeds compositor
    // Receives touch from Device -> forwards to phone
    auto headunit = std::make_shared<HeadUnit>(config, compositor);

    // Device: AA session toward Sync 4 (Pi acts as Android phone)
    // Sources video from compositor -> sends to Sync 4
    // Receives touch from Sync 4 -> forwards to HeadUnit
    auto device = std::make_shared<Device>(config, compositor, headunit);

    // The vehicle's sensor list and fuel/EV declaration are remembered across restarts too (ui54b: the phone connected during Sync's start-up USB flap, was
    // offered only the default set without the EV types 23/24 and never saw the battery charge for the whole drive; a phone session cannot be re-advertised).
    auto sensorCacheSave = [path = config.sensor_cache_path](const std::vector<int>& t, const std::vector<int>& f, const std::vector<int>& e) {
        if (path.empty() || t.empty()) return;
        const std::string tmp = path + ".tmp";
        { std::ofstream o(tmp); o << "types"; for (int x : t) o << " " << x; o << "\nfuel"; for (int x : f) o << " " << x; o << "\nev"; for (int x : e) o << " " << x; o << "\n"; }
        std::rename(tmp.c_str(), path.c_str()); };
    auto veh_types = std::make_shared<std::vector<int>>(); auto veh_fuel = std::make_shared<std::vector<int>>(); auto veh_ev = std::make_shared<std::vector<int>>();
    if (config.relay_sensors && !config.sensor_cache_path.empty()) {
        std::ifstream f(config.sensor_cache_path); std::string line; std::vector<int> t, fu, ev;
        while (std::getline(f, line)) { std::istringstream is(line); std::string k; is >> k; int v; std::vector<int>& dst = k == "types" ? t : k == "fuel" ? fu : ev; while (is >> v) dst.push_back(v); }
        if (!t.empty()) { std::cout << "[aa-bridge] sensor cache: " << t.size() << " vehicle sensor types, " << fu.size() << " fuel types, " << ev.size() << " EV connectors (from " << config.sensor_cache_path << ")" << std::endl;
            headunit->setVehicleSensorTypes(t); headunit->setVehicleSensorCaps(fu, ev); *veh_types = t; *veh_fuel = fu; *veh_ev = ev; }
    }
    if (config.relay_sensors) device->setSensorCaps([&headunit, veh_types, veh_fuel, veh_ev, sensorCacheSave](const std::vector<int>& f, const std::vector<int>& e) { headunit->setVehicleSensorCaps(f, e); *veh_fuel = f; *veh_ev = e; sensorCacheSave(*veh_types, f, e); });
    if (config.relay_sensors) device->setSensorRelay([&headunit, veh_types, veh_fuel, veh_ev, sensorCacheSave](const std::vector<int>& t) { headunit->setVehicleSensorTypes(t); if (!t.empty()) { *veh_types = t; sensorCacheSave(t, *veh_fuel, *veh_ev); } }, [&headunit](const std::string& raw) { headunit->relayVehicleSensors(raw); });
    // Navigation channel hub: carries turn-by-turn / EV forecast / nav focus between the phone and the vehicle (config relay_nav)
    std::shared_ptr<AvRelay> av;
    if (config.relay_audio || config.relay_control) { av = std::make_shared<AvRelay>(); headunit->setAvRelay(av); device->setAvRelay(av); }   // audio, microphone and raw control messages
    if (config.relay_media_status) { auto mr = std::make_shared<RawRelay>("Media"); headunit->setMediaRelay(mr); device->setMediaRelay(mr); }
    std::shared_ptr<NavRelay> nav;
    if (config.relay_nav) { nav = std::make_shared<NavRelay>(config.nav_require_start); nav->setDecoder(explainNavMessage); headunit->setNavRelay(nav); device->setNavRelay(nav); }

    // UI: layouts (full AA / full camera / split / PIP), prefs, control FIFO
    auto uictl = std::make_shared<ui::UiController>(config, compositor);

    // Touch: Sync 4 -> UiController -> menu | camera window | phone (coordinates remapped)
    uictl->setPhoneTouchForwarder([&headunit](const TouchFrame& f) { headunit->injectTouchFrame(f); });
    BtBootstrap bt(config);
    if (config.bt_call_aa && av) {                                   // phone call audio over Android Auto: call bridge <-> the vehicle's GUIDANCE sink + microphone
        auto aa = std::make_shared<btcall::AaCall>();
        aa->gain_pct = config.bt_call_aa_gain; aa->mic_gain_pct = config.bt_call_aa_mic_gain; aa->duck = config.bt_call_aa_duck; aa->agc = config.bt_call_aa_agc;
        aa->hooks.log = [](const std::string& s) { std::cout << "[Call] " << s << std::endl; };
        aa->hooks.start = [av] { av->phoneAudioStart(AvRelay::GUIDANCE, 0x53434f01); };
        aa->hooks.down = [av](const uint8_t* p, size_t n) { av->phoneAudioData(AvRelay::GUIDANCE, (uint64_t)btcall::AaCall::nowMs() * 1000, p, n); };
        aa->hooks.stop = [av] { av->phoneAudioStop(AvRelay::GUIDANCE); };
        aa->hooks.mic = [av, aa](bool open) { av->setCallMic(open, open ? AvRelay::MicData([aa](uint64_t, const uint8_t* p, size_t n) { aa->pushMic16k(p, n, btcall::AaCall::nowMs()); }) : AvRelay::MicData()); };
        bt.setAaCall(aa);
        std::cout << "[aa-bridge] phone call audio over Android Auto enabled (bt_call_aa=1, gain " << config.bt_call_aa_gain << "%)" << std::endl;
    } else if (config.bt_call_aa) std::cout << "[aa-bridge] bt_call_aa needs relay_audio=1; call audio over Android Auto stays off" << std::endl;
    if (config.precondition_button && nav) {                         // the strip's battery toggle -> a synthetic charging stop on the vehicle's navigation channel (see NavRelay)
        NavRelay::PrecondCfg pc; pc.power_w = config.precond_power_w; pc.eta_min = config.precond_eta_min; pc.tz_min = config.precond_tz_min; pc.period_s = config.precond_period_s; pc.name = config.precond_name; pc.mode = config.precond_mode; pc.replay_pos = config.precond_replay_pos; pc.replay_steps = config.precond_replay_steps; pc.replay_name = config.precond_replay_name; pc.replay_road = config.precond_replay_road; pc.replay_field1 = config.precond_replay_field1; pc.replay_field3 = config.precond_replay_field3; pc.replay_plain = config.precond_replay_plain;
        uictl->setPreconditionSink([nav, pc](bool on) { nav->setPrecondition(on, pc); });
        nav->setPrecondStateCallback([&uictl](bool on) { uictl->setPreconditionState(on); });
        std::cout << "[aa-bridge] battery preconditioning button enabled (mode " << pc.mode << ", replay variant: pos=" << pc.replay_pos << " steps=" << pc.replay_steps << " name='" << pc.replay_name << "' field1=" << pc.replay_field1 << " field3=" << pc.replay_field3 << " plain=" << pc.replay_plain << "): charging stop '" << pc.name << "' " << pc.power_w / 1000 << " kW, ETA now+" << pc.eta_min << " min (tz " << pc.tz_min << " min)" << std::endl;
    }
    headunit->setPresenceCallback([&uictl, &bt](bool on) { uictl->setPhonePresent(on); bt.setPhoneActive(on); });
    uictl->setUiConfigSender([&headunit](int l, int r, int t, int b) { headunit->sendUiConfig(l, r, t, b); });
    headunit->setUiConfigReplyCallback([&uictl] { uictl->reshapeReply(); });
    device->setVideoParamsCallback([&headunit](const VideoParams& v) { headunit->setVideoParams(v); });
    device->setVisibleWindowCallback([&uictl, &headunit](int x, int y, int w, int h) { uictl->setVisibleWindow(x, y, w, h); headunit->setVisibleWindow(x, y, w, h); });
    // The vehicle's window is remembered across restarts: a phone that connects before the vehicle has announced it (Sync's USB link flapping at
    // start-up, ui51a) is then advertised and laid out for the real window instead of the whole frame.
    if (!config.window_cache_path.empty()) {
        { std::ifstream f(config.window_cache_path); int x = 0, y = 0, w = 0, h = 0; if (f >> x >> y >> w >> h && w > 0 && h > 0) {
            std::cout << "[aa-bridge] window cache: " << w << "x" << h << " at " << x << "," << y << " (from " << config.window_cache_path << ")" << std::endl;
            uictl->setVisibleWindow(x, y, w, h); headunit->setVisibleWindow(x, y, w, h); } }
        uictl->setWindowCallback([path = config.window_cache_path](int x, int y, int w, int h) {
            const std::string tmp = path + ".tmp"; { std::ofstream f(tmp); f << x << " " << y << " " << w << " " << h << "\n"; } std::rename(tmp.c_str(), path.c_str()); });
    }
    device->setGeometryCallback([&uictl](int w, int h) { uictl->setTouchGeometry(w, h); });
    std::shared_ptr<obd::Poller> obdp;
    if (config.obd_enable) {                                   // vehicle data over the OBD-II port (own thread, own USB device; Energy page)
        obd::Poller::Options oo; oo.port = config.obd_port; oo.baud = config.obd_baud; oo.log_raw = config.obd_log_raw;
        obdp = std::make_shared<obd::Poller>(oo); obdp->start();
        std::cout << "[aa-bridge] OBD-II poller started (port " << (config.obd_port.empty() ? "auto" : config.obd_port) << ", raw log " << (config.obd_log_raw ? "on" : "off") << ")" << std::endl;
    }
    device->setVehicleCallback([&uictl, obdp](int what, int v) { uictl->setTruckSensor(what, v); if (obdp && what == 5 && v >= 0) obdp->setSpeedMps(v / 1000.0); });
    uictl->setPhoneSensorSink([&headunit](int driving_status, bool night) { headunit->setPhoneSensors(driving_status, night); });
    if (config.relay_keys) device->setKeyCallback([&headunit](int kc, bool down, bool lp, int) { headunit->injectKey(kc, down, lp); });
    device->setTouchFrameCallback([&uictl](const TouchFrame& f) { uictl->onTouchFrame(f); });

    // Start in order: compositor first, then both AA sessions
    compositor->setCameraPresenceCallback([&uictl](bool on) { uictl->setCameraPresent(on); });
    compositor->start();
    uictl->start();
    headunit->start();
    { std::ifstream s("/var/lib/aa-bridge/cert-refresh.status"); std::string line; if (std::getline(s, line)) std::cout << "[Cert] last start-up refresh over home Wi-Fi: " << line << std::endl; }
    {   // Which phone-side certificate are we presenting to the vehicle, and when does it stop working? (Sync 4 rejects an expired one: AUTH_COMPLETE -24.)
        // The Pi has no RTC, so its own "days left" would be wrong: log the absolute date, the analyzer on a machine with a real clock does the warning.
        if (FILE* f = fopen(config.md_cert.c_str(), "r")) {
            if (X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr)) {
                BIO* b = BIO_new(BIO_s_mem()); ASN1_TIME_print(b, X509_get0_notAfter(x)); char buf[64] = {0}; int n = BIO_read(b, buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0;
                std::cout << "[Cert] phone-side certificate " << config.md_cert << " valid until: " << buf << " (the Pi clock is not trusted, no days-left here)" << std::endl; BIO_free(b); X509_free(x);
            } else std::cout << "[Cert] WARNING: cannot parse " << config.md_cert << std::endl;
            fclose(f);
        } else std::cout << "[Cert] WARNING: cannot open " << config.md_cert << std::endl;
    }
    HangWatchdog hang_wd(config.hang_watchdog_s > 0 ? config.hang_watchdog_s : 1000000000);
    hang_wd.start();   // pings systemd only while frames flow; harmless when the unit has no WatchdogSec
    WifiWatch wifi_watch;
    if (config.wifi_watch) wifi_watch.start();
    if (config.bt_pair_proxy && config.bt_pair_initiate) device->setCarAddressCallback([&bt](const std::string& a) { bt.pairWithCar(a); });
    if (config.bt_pair_proxy) device->setBtAddressProvider([&bt]() { return bt.pairAddress(); });
    if (config.bt_bootstrap) bt.start(); else std::cout << "[aa-bridge] Bluetooth bootstrap disabled by config (bt_bootstrap=0)" << std::endl;
    device->start();

    ThreadCpuReporter cpu_report; cpu_report.start(5);
    std::cout << "[aa-bridge] Running. Ctrl+C to stop." << std::endl;

    int tick = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (obdp && ++tick % 10 == 0) uictl->setEnergy(obdp->snapshot());        // 1 Hz: the Energy page redraws from this while shown
    }

    std::cout << "[aa-bridge] Shutting down..." << std::endl;
    if (obdp) obdp->stop();
    device->stop();
    headunit->stop();
    uictl->stop();
    compositor->stop();

    gst_deinit();
    return 0;
}
