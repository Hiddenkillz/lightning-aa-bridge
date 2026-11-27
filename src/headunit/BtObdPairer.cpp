#include "BtObdPairer.h"
#include <gio/gio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iostream>
#include <mutex>
#include <thread>

#define LOG(x) do { std::cout << "[ObdPair] " << x << std::endl; } while (0)

using ui::ObdPairDevice;
using ui::ObdPairState;

bool BtObdPairer::looksLikeObdAdapter(const std::string& name) {
    std::string n; for (char c : name) n += (char)std::tolower((unsigned char)c);
    static const char* kw[] = {"obd", "elm", "vlink", "v-link", "vgate", "viecar", "icar", "konnwei", "carista", "veepeak", "scantool"};
    for (const char* k : kw) if (n.find(k) != std::string::npos) return true;
    return false;
}

bool BtObdPairer::looksLikePhone(const std::vector<std::string>& uuids) {
    for (const auto& u : uuids)
        if (u.rfind("4de17a00", 0) == 0 || u.rfind("0000111f", 0) == 0 || u.rfind("00001112", 0) == 0 || u.rfind("0000110a", 0) == 0) return true;
    return false;
}

void BtObdPairer::sortDevices(std::vector<ObdPairDevice>& v) {
    std::stable_sort(v.begin(), v.end(), [](const ObdPairDevice& a, const ObdPairDevice& b) {
        if (a.obd_like != b.obd_like) return a.obd_like;
        if ((a.rssi != 0) != (b.rssi != 0)) return a.rssi != 0;
        if (a.rssi != b.rssi) return a.rssi > b.rssi;
        return a.name < b.name;
    });
}

class BtObdPairer::Impl {
public:
    explicit Impl(std::string ad) : ad_(std::move(ad)), cancel_(g_cancellable_new()) {}
    ~Impl() {
        stopThread(true);
        g_object_unref(cancel_);
        if (conn_) g_object_unref(conn_);
    }

    void startScan() {
        std::lock_guard<std::mutex> op(op_m_);
        { std::lock_guard<std::mutex> l(m_); if (st_.phase == ObdPairState::Pairing) return; }
        stopThreadLocked(false);
        { std::lock_guard<std::mutex> l(m_); st_.devices.clear(); st_.paired_addr.clear(); setLocked(ObdPairState::Scanning, "Searching... press the button on the adapter"); }
        stop_ = false; g_cancellable_reset(cancel_);
        th_ = std::thread([this] { scanLoop(); });
    }
    void stopScan() {
        std::lock_guard<std::mutex> op(op_m_);
        { std::lock_guard<std::mutex> l(m_); if (st_.phase != ObdPairState::Scanning) return; }
        stopThreadLocked(false);
        std::lock_guard<std::mutex> l(m_); setLocked(ObdPairState::Idle, "");
    }
    void pair(const std::string& addr) {
        std::lock_guard<std::mutex> op(op_m_);
        { std::lock_guard<std::mutex> l(m_); if (st_.phase == ObdPairState::Pairing) return; }
        stopThreadLocked(false);
        stop_ = false; g_cancellable_reset(cancel_);
        th_ = std::thread([this, addr] { pairProc(addr); });
    }
    void forget(const std::string& addr) {
        std::lock_guard<std::mutex> op(op_m_);
        { std::lock_guard<std::mutex> l(m_); if (st_.phase == ObdPairState::Pairing) return; }
        stopThreadLocked(false);
        if (GDBusConnection* c = conn()) {
            GError* e = nullptr;
            GVariant* r = g_dbus_connection_call_sync(c, "org.bluez", ad_.c_str(), "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", devPath(addr).c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &e);
            if (r) g_variant_unref(r);
            if (e) { LOG("forget " << addr << ": " << e->message); g_error_free(e); } else LOG("forgot " << addr);
        }
        std::lock_guard<std::mutex> l(m_); st_.paired_addr.clear(); setLocked(ObdPairState::Idle, "");
    }
    ObdPairState state() { std::lock_guard<std::mutex> l(m_); return st_; }

private:
    std::string ad_;
    GDBusConnection* conn_ = nullptr; std::mutex conn_m_;
    GCancellable* cancel_;
    std::mutex op_m_, m_;                 // op_m_ serialises start/stop/pair/forget; m_ guards st_
    ObdPairState st_;
    std::thread th_; std::atomic<bool> stop_{false};

    void setLocked(ObdPairState::Phase ph, const std::string& msg) { st_.phase = ph; st_.message = msg; ++st_.serial; }
    void setMsg(ObdPairState::Phase ph, const std::string& msg) { std::lock_guard<std::mutex> l(m_); setLocked(ph, msg); }
    void stopThread(bool cancel) { std::lock_guard<std::mutex> op(op_m_); stopThreadLocked(cancel); }
    void stopThreadLocked(bool) { stop_ = true; g_cancellable_cancel(cancel_); if (th_.joinable()) th_.join(); }

    GDBusConnection* conn() {
        std::lock_guard<std::mutex> l(conn_m_);
        if (!conn_) { GError* e = nullptr; conn_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e); if (!conn_) { LOG("no system bus: " << (e ? e->message : "?")); if (e) g_error_free(e); } }
        return conn_;
    }
    std::string devPath(const std::string& addr) const { std::string p = addr; std::replace(p.begin(), p.end(), ':', '_'); return ad_ + "/dev_" + p; }

    GVariant* callSync(const std::string& path, const char* iface, const char* method, GVariant* args, int timeout_ms, GError** err) {
        GDBusConnection* c = conn(); if (!c) { if (args && g_variant_is_floating(args)) g_variant_unref(args); return nullptr; }
        return g_dbus_connection_call_sync(c, "org.bluez", path.c_str(), iface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout_ms, cancel_, err);
    }

    // Devices BlueZ currently knows on our adapter, named, and not a phone.
    std::vector<ObdPairDevice> listDevices() {
        std::vector<ObdPairDevice> out;
        GError* e = nullptr;
        GVariant* r = callSync("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr, 5000, &e);
        if (e) g_error_free(e);
        if (!r) return out;
        GVariant* objs = g_variant_get_child_value(r, 0);
        GVariantIter it; g_variant_iter_init(&it, objs);
        const gchar* path; GVariant* ifaces;
        while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            if (std::string(path).rfind(ad_ + "/dev_", 0) == 0) {
                if (GVariant* dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"))) {
                    const gchar* addr = nullptr; const gchar* name = nullptr; gint16 rssi = 0;
                    g_variant_lookup(dev, "Address", "&s", &addr); g_variant_lookup(dev, "Name", "&s", &name); g_variant_lookup(dev, "RSSI", "n", &rssi);
                    std::vector<std::string> uuids;
                    if (GVariant* u = g_variant_lookup_value(dev, "UUIDs", G_VARIANT_TYPE("as"))) { GVariantIter ui; g_variant_iter_init(&ui, u); const gchar* s; while (g_variant_iter_next(&ui, "&s", &s)) uuids.push_back(s); g_variant_unref(u); }
                    if (addr && name && *name && !looksLikePhone(uuids)) {
                        ObdPairDevice d; d.addr = addr; for (auto& ch : d.addr) ch = (char)std::toupper((unsigned char)ch);
                        d.name = name; d.rssi = rssi; d.obd_like = looksLikeObdAdapter(d.name);
                        out.push_back(d);
                    }
                    g_variant_unref(dev);
                }
            }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objs); g_variant_unref(r);
        sortDevices(out);
        if (out.size() > 12) out.resize(12);
        return out;
    }

    static bool sameList(const std::vector<ObdPairDevice>& a, const std::vector<ObdPairDevice>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (a[i].addr != b[i].addr || a[i].name != b[i].name || a[i].rssi != b[i].rssi) return false;
        return true;
    }

    void scanLoop() {
        if (!conn()) { setMsg(ObdPairState::Failed, "Bluetooth is not available"); return; }
        { GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}")); g_variant_builder_add(&b, "{sv}", "Transport", g_variant_new_string("bredr"));
          GError* e = nullptr; GVariant* r = callSync(ad_, "org.bluez.Adapter1", "SetDiscoveryFilter", g_variant_new("(a{sv})", &b), 3000, &e); if (r) g_variant_unref(r); if (e) g_error_free(e); }
        { GError* e = nullptr; GVariant* r = callSync(ad_, "org.bluez.Adapter1", "StartDiscovery", nullptr, 5000, &e);
          if (r) g_variant_unref(r);
          if (e) { const std::string msg = e->message; const bool inprog = msg.find("InProgress") != std::string::npos; g_error_free(e);
                   if (!inprog) { LOG("StartDiscovery: " << msg); setMsg(ObdPairState::Failed, "Could not start the Bluetooth scan"); return; } } }
        LOG("scan started");
        const auto t0 = std::chrono::steady_clock::now();
        while (!stop_ && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
            std::vector<ObdPairDevice> v = listDevices();
            {
                std::lock_guard<std::mutex> l(m_);
                if (!stop_ && st_.phase == ObdPairState::Scanning && !sameList(v, st_.devices)) {
                    st_.devices = v; ++st_.serial;
                    st_.message = (!v.empty() && v.front().obd_like) ? "Tap Pair next to your adapter" : v.empty() ? "Searching... press the button on the adapter" : "Searching... press the button on the adapter";
                }
            }
            for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        { GError* e = nullptr; GVariant* r = g_dbus_connection_call_sync(conn(), "org.bluez", ad_.c_str(), "org.bluez.Adapter1", "StopDiscovery", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &e); if (r) g_variant_unref(r); if (e) g_error_free(e); }
        LOG("scan stopped");
        std::lock_guard<std::mutex> l(m_);
        if (!stop_ && st_.phase == ObdPairState::Scanning) setLocked(ObdPairState::Idle, st_.devices.empty() ? "No adapter found. Press its button and tap Scan again." : "Scan finished. Tap Scan to search again.");
    }

    void pairProc(const std::string& addr) {
        std::string name = addr;
        { std::lock_guard<std::mutex> l(m_); for (const auto& d : st_.devices) if (d.addr == addr && !d.name.empty()) name = d.name; setLocked(ObdPairState::Pairing, "Pairing " + name + "... keep the adapter in pairing mode"); }
        if (!conn()) { setMsg(ObdPairState::Failed, "Bluetooth is not available"); return; }
        { GError* e = nullptr; GVariant* r = g_dbus_connection_call_sync(conn(), "org.bluez", ad_.c_str(), "org.bluez.Adapter1", "StopDiscovery", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &e); if (r) g_variant_unref(r); if (e) g_error_free(e); }
        const std::string path = devPath(addr);
        bool paired = false;
        { GError* e = nullptr; GVariant* r = callSync(path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.bluez.Device1", "Paired"), 3000, &e);
          if (r) { GVariant* v = nullptr; g_variant_get(r, "(v)", &v); if (v) { paired = g_variant_get_boolean(v); g_variant_unref(v); } g_variant_unref(r); }
          if (e) g_error_free(e); }
        if (!paired) {
            LOG("pairing " << addr << " (" << name << ")");
            GError* e = nullptr; GVariant* r = callSync(path, "org.bluez.Device1", "Pair", nullptr, 45000, &e);
            if (r) g_variant_unref(r);
            if (e) {
                const std::string msg = e->message; const bool exists = msg.find("AlreadyExists") != std::string::npos; g_error_free(e);
                if (!exists) {
                    LOG("Pair failed: " << msg);
                    if (stop_) { setMsg(ObdPairState::Idle, ""); return; }
                    setMsg(ObdPairState::Failed, "Pairing failed. Press the adapter's button and try again.");
                    return;
                }
            }
        }
        { GVariant* args = g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE));
          GError* e = nullptr; GVariant* r = callSync(path, "org.freedesktop.DBus.Properties", "Set", args, 3000, &e); if (r) g_variant_unref(r); if (e) { LOG("trust: " << e->message); g_error_free(e); } }
        LOG("paired and trusted " << addr);
        std::lock_guard<std::mutex> l(m_); st_.paired_addr = addr; setLocked(ObdPairState::Done, "Paired " + name);
    }
};

BtObdPairer::BtObdPairer(std::string adapter_path) : impl_(std::make_unique<Impl>(std::move(adapter_path))) {}
BtObdPairer::~BtObdPairer() = default;
void BtObdPairer::startScan() { impl_->startScan(); }
void BtObdPairer::stopScan() { impl_->stopScan(); }
void BtObdPairer::pair(const std::string& addr) { impl_->pair(addr); }
void BtObdPairer::forget(const std::string& addr) { impl_->forget(addr); }
ui::ObdPairState BtObdPairer::state() { return impl_->state(); }
