#include "BtBootstrap.h"
#include "../btcall/CallBridge.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <ctime>
#include <sys/wait.h>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <functional>
#include <algorithm>
#include <sstream>
#include <map>
#include <set>
#include <vector>

#define LOG(x) do { std::cout << "[Bt] " << x << std::endl; } while (0)

namespace btboot {
static void putVarint(std::string& s, uint64_t v) { while (v >= 0x80) { s.push_back((char)(v | 0x80)); v >>= 7; } s.push_back((char)v); }
static void putStr(std::string& s, int field, const std::string& v) { s.push_back((char)((field << 3) | 2)); putVarint(s, v.size()); s += v; }
static void putInt(std::string& s, int field, uint64_t v) { s.push_back((char)((field << 3) | 0)); putVarint(s, v); }

std::string encodeWifiStartRequest(const WifiParams& p) { std::string s; putStr(s, 1, p.ip); putInt(s, 2, (uint64_t)p.port); return s; }
std::string encodeWifiInfoResponse(const WifiParams& p) {
    std::string s;
    putStr(s, 1, p.ssid); putStr(s, 2, p.key); putStr(s, 3, p.bssid);
    putInt(s, 4, 8);   // security_mode = WPA2_PERSONAL (wire value 8)
    putInt(s, 5, 1);   // access_point_type = DYNAMIC
    return s;
}

static bool readExact(int fd, uint8_t* b, size_t n, int timeout_ms) {
    size_t got = 0;
    while (got < n) {
        pollfd pf{fd, POLLIN, 0};
        int r = ::poll(&pf, 1, timeout_ms);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; return false; }
        ssize_t k = ::read(fd, b + got, n - got);
        if (k <= 0) return false;
        got += (size_t)k;
    }
    return true;
}
static bool sendMsg(int fd, uint16_t id, const std::string& body) {
    std::string pkt; pkt.push_back((char)(body.size() >> 8)); pkt.push_back((char)body.size()); pkt.push_back((char)(id >> 8)); pkt.push_back((char)id); pkt += body;
    size_t off = 0;
    while (off < pkt.size()) { ssize_t w = ::send(fd, pkt.data() + off, pkt.size() - off, MSG_NOSIGNAL); if (w <= 0) return false; off += (size_t)w; }
    return true;
}
static bool recvMsg(int fd, uint16_t want, std::string& body, int timeout_ms, std::string& why) {
    uint8_t h[4];
    if (!readExact(fd, h, 4, timeout_ms)) { why = "timeout/closed waiting for message"; return false; }
    uint16_t len = (uint16_t)((h[0] << 8) | h[1]), id = (uint16_t)((h[2] << 8) | h[3]);
    body.assign(len, 0);
    if (len && !readExact(fd, (uint8_t*)&body[0], len, timeout_ms)) { why = "short payload"; return false; }
    if (id != want) { why = "unexpected message id " + std::to_string(id) + " (wanted " + std::to_string(want) + ")"; return false; }
    return true;
}

// protobuf: first varint field with tag 0x08 (WifiStartResponse/WifiConnectStatus "status"); negative values are 10-byte varints. -999 = absent.
static long long statusField(const std::string& b) {
    if (b.empty() || (unsigned char)b[0] != 0x08) return -999;
    unsigned long long v = 0; int shift = 0;
    for (size_t i = 1; i < b.size() && shift < 70; ++i) { v |= (unsigned long long)(b[i] & 0x7f) << shift; shift += 7; if (!(b[i] & 0x80)) break; }
    return (long long)(int)(unsigned int)v;
}

bool runWifiBootstrap(int fd, const WifiParams& p, const std::function<void(const std::string&)>& log, int timeout_ms) {
    std::string body, why;
    log("stage 1: WifiStartRequest ip=" + p.ip + " port=" + std::to_string(p.port));
    if (!sendMsg(fd, 1, encodeWifiStartRequest(p))) { log("send failed"); return false; }
    if (!recvMsg(fd, 2, body, timeout_ms, why)) { log("stage 2 (WifiInfoRequest) failed: " + why); return false; }
    log("stage 2: WifiInfoRequest received -> sending ssid=" + p.ssid + " bssid=" + p.bssid + " (WPA2, key not logged)");
    if (!sendMsg(fd, 3, encodeWifiInfoResponse(p))) { log("send failed"); return false; }
    if (!recvMsg(fd, 7, body, timeout_ms, why)) { log("stage 4 (WifiStartResponse) failed: " + why); return false; }
    { const int st = statusField(body); log("stage 4: WifiStartResponse received (" + std::to_string(body.size()) + " bytes)" + (st == -999 ? " (no status field in it: normal for this phone, not an error)" : " status=" + std::to_string(st))); }
    auto t0 = std::chrono::steady_clock::now();
    if (!recvMsg(fd, 6, body, timeout_ms, why)) { log("stage 5 (WifiConnectStatus) failed: " + why); return false; }
    long long st = statusField(body);
    log("stage 5: WifiConnectStatus received after " + std::to_string((int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count())
        + " ms, status=" + std::to_string(st) + (st == 0 ? " (success: phone joined Wi-Fi)" : " (NOT success: 1=unsolicited, -1=no compatible version, -2=inaccessible channel, -3=incorrect credentials, others=see AAW Status)"));
    return st == 0;
}
}  // namespace btboot

// ---------------------------------------------------------------------------------------------------------------
static const char* kAawUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";
static const char* kHspHs   = "00001108-0000-1000-8000-00805f9b34fb";
static const char* kHfpHf   = "0000111e-0000-1000-8000-00805f9b34fb";
static const char* kHfpAg   = "0000111f-0000-1000-8000-00805f9b34fb";
static const char* kHspAg   = "00001112-0000-1000-8000-00805f9b34fb";

static const char* kProfileXml =
    "<node><interface name='org.bluez.Profile1'>"
    "<method name='Release'/>"
    "<method name='NewConnection'><arg type='o' direction='in'/><arg type='h' direction='in'/><arg type='a{sv}' direction='in'/></method>"
    "<method name='RequestDisconnection'><arg type='o' direction='in'/></method>"
    "</interface></node>";
static const char* kAgentXml =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/>"
    "</interface></node>";

class BtBootstrap::Impl {
public:
    explicit Impl(const Config& c) : cfg_(c) {}

    void start() { thread_ = std::thread([this] { run(); }); }
    void stop() {
        stopping_ = true;
        if (loop_) g_main_loop_quit(loop_);
        if (thread_.joinable()) thread_.join();
    }

private:
    Config cfg_;
    std::thread thread_;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* conn_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<int> sessions_{0};
    std::atomic<bool> phone_active_{false};
    int stale_ = 0, kicks_ = 0;                // GLib thread only
    int release_wait_ = 0, releases_ = 0; bool was_active_ = false, nudge_paused_ = false;     // bt_release_phone bookkeeping (GLib thread only)
    std::vector<std::string> kicked_;
    std::set<std::string> inflight_;
    std::set<std::string> trust_set_;
    std::string ad_ = "/org/bluez/hci0", dongle_;           // onboard adapter path (the phone) / dongle adapter path (the vehicle; empty without call bridge)
    std::unique_ptr<btcall::CallBridge> call_; std::thread dongle_thread_, watch_thread_; std::mutex dongle_m_;
    std::string dongle() { std::lock_guard<std::mutex> g(dongle_m_); return dongle_; }                                              // devices with a ConnectProfile nudge still pending (GLib thread only)
    std::map<std::string, std::chrono::steady_clock::time_point> backoff_until_;  // after a nudge TIMED OUT (phone unreachable): leave it alone for a while
    std::map<std::string, std::string> last_state_;                               // last logged description per device: log only changes
    std::chrono::steady_clock::time_point t_ready_ = std::chrono::steady_clock::now();
    int nudge_s_ = 8;
public:
    bool wantHfp() const { return cfg_.bt_phone_profile == "hfp" || (cfg_.bt_phone_profile.empty() && (cfg_.bt_call_bridge || cfg_.bt_call_aa)); }
    std::set<std::string> hsp_fallback_;          // phones that refused a hands-free connect: woken over the headset profile instead (GLib thread only)
    int hf_keep_wait_ = 0;
    std::string pairAddress() {
        std::string a;
        for (int i = 0; i < (cfg_.bt_call_bridge ? 80 : 1); ++i) {              // bridge on: the dongle is the "phone" the vehicle must pair with; wait for it (<= 8 s)
            const std::string dg = dongle(); if (!dg.empty()) a = adapterAddress(dg);
            if (!a.empty() || stopping_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (a.empty()) { a = adapterAddress(ad_); if (cfg_.bt_call_bridge) LOG("pair address: dongle not ready, falling back to the onboard radio " << a << " (calls will NOT bridge)"); }
        return a;
    }
    std::atomic<bool> bridge_ready_{false};
    void setPhoneActive(bool on) { phone_active_ = on; if (on) { stale_ = 0; kicks_ = 0; } }
private:

    static std::string readFile(const std::string& p) { std::ifstream f(p); std::string s; std::getline(f, s); return s; }

    // The phone's Wi-Fi join fails if hostapd is not yet beaconing, and on a truck start the phone arrives within seconds of the Pi booting.
    // Hold the handoff until hostapd reports ENABLED (checked once a second, up to 45 s), then go ahead regardless.
    void waitForAp() {
        if (getenv("AA_BT_NO_AP_WAIT")) return;                      // test hook (no hostapd on the test host)
        const std::string cmd = "hostapd_cli -i " + cfg_.ap_iface + " status 2>/dev/null | grep -c '^state=ENABLED'";
        for (int i = 0; i < 45 && !stopping_; ++i) {
            bool up = false;
            if (FILE* f = popen(cmd.c_str(), "r")) { char b[16] = {0}; if (fgets(b, sizeof b, f)) up = atoi(b) > 0; pclose(f); }
            if (up) { LOG("access point ready" << (i ? " after " + std::to_string(i) + " s wait" : std::string(" (no wait)"))); return; }
            if (i == 0) LOG("access point not up yet; holding the Wi-Fi handoff until hostapd is ENABLED");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LOG("access point still not ENABLED after 45 s; sending Wi-Fi details anyway");
    }

    btboot::WifiParams params() const {
        btboot::WifiParams w;
        w.ip = "192.168.50.1"; w.port = 5277; w.ssid = cfg_.ap_ssid; w.key = cfg_.ap_password;
        w.bssid = readFile("/sys/class/net/" + cfg_.ap_iface + "/address");
        return w;
    }

    // ---- D-Bus helpers -----------------------------------------------------------------------------------
    GVariant* call(const char* path, const char* iface, const char* method, GVariant* args, const char* dest = "org.bluez", int timeout = 5000) {
        GError* e = nullptr;
        GVariant* r = g_dbus_connection_call_sync(conn_, dest, path, iface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout, nullptr, &e);
        if (!r) { LOG(std::string(method) + " failed: " + (e ? e->message : "?")); if (e) g_error_free(e); }
        return r;
    }
    // Non-blocking BlueZ call. EVERYTHING that talks to a phone (ConnectProfile, Disconnect, Connect) must be asynchronous: this GLib loop also has to answer
    // BlueZ's Profile1.NewConnection when the phone opens the AA Wireless profile. With synchronous calls a ConnectProfile to an unreachable phone blocked the loop
    // for its whole 15 s timeout, over and over (truck 28: the phone's connection was answered up to 28 s late, reproduced by tests/bt_dbus_test.py).
    struct Async { Impl* self; std::string what, dev; std::chrono::steady_clock::time_point t0; std::function<void(bool, bool)> done; };
    std::string last_err_;                       // message of the D-Bus error that just completed (GLib thread only)
    void callAsync(const std::string& path, const char* iface, const char* method, GVariant* args, int timeout_ms, const std::string& what, const std::string& dev,
                   std::function<void(bool ok, bool timed_out)> done = {}) {
        auto* a = new Async{this, what, dev, std::chrono::steady_clock::now(), std::move(done)};
        g_dbus_connection_call(conn_, "org.bluez", path.c_str(), iface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr,
            [](GObject* src, GAsyncResult* res, gpointer ud) {
                auto* a = static_cast<Async*>(ud); GError* e = nullptr;
                GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
                const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a->t0).count();
                const bool to = e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
                if (r) { LOG(a->what << " ok (" << ms << " ms)"); g_variant_unref(r); } else LOG(a->what << " failed after " << ms << " ms: " << (e ? e->message : "?"));
                a->self->last_err_ = (!r && e) ? std::string(e->message) : std::string();
                if (a->done) a->done(r != nullptr, to);
                if (e) g_error_free(e);
                delete a; }, a);
    }
    void setProp(const char* path, const char* iface, const char* prop, GVariant* val) {
        GVariant* r = call(path, "org.freedesktop.DBus.Properties", "Set", g_variant_new("(ssv)", iface, prop, val));
        if (r) g_variant_unref(r);
    }

    // ---- profile objects ---------------------------------------------------------------------------------
    static void onProfileCall(GDBusConnection*, const gchar*, const gchar* obj_path, const gchar*, const gchar* method,
                              GVariant* params, GDBusMethodInvocation* inv, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        std::string m = method, path = obj_path;
        if (m == "NewConnection") {
            const gchar* dev = nullptr; gint32 idx = -1; GVariant* props = nullptr;
            g_variant_get(params, "(&oh@a{sv})", &dev, &idx, &props);
            int fd = -1;
            GDBusMessage* msg = g_dbus_method_invocation_get_message(inv);
            GUnixFDList* fl = g_dbus_message_get_unix_fd_list(msg);
            if (fl) { GError* e = nullptr; fd = g_unix_fd_list_get(fl, idx, &e); if (e) g_error_free(e); }
            std::string devs = dev ? dev : "";
            LOG("NewConnection on " << path << " from " << devs << " fd=" << fd);
            if (props) g_variant_unref(props);
            const std::string dg = self->dongle(); const bool onDongle = !dg.empty() && devs.rfind(dg + "/", 0) == 0;
            auto addrOf = [](const std::string& d) { std::string a = d.substr(d.rfind("dev_") == std::string::npos ? 0 : d.rfind("dev_") + 4); for (auto& ch : a) if (ch == '_') ch = ':'; return a; };
            if (fd >= 0 && self->call_ && path == "/aabridge/hfphf" && devs.rfind(self->ad_ + "/", 0) == 0) { self->call_->onPhoneLink(fd, addrOf(devs)); g_dbus_method_invocation_return_value(inv, nullptr); return; }
            if (fd >= 0 && self->call_ && path == "/aabridge/hfpag" && onDongle) { self->call_->onVehicleLink(fd, addrOf(devs)); g_dbus_method_invocation_return_value(inv, nullptr); return; }
            if (fd >= 0 && onDongle) { LOG("closing " << path << " from the vehicle-side adapter (only hands-free belongs there)"); ::close(fd); g_dbus_method_invocation_return_value(inv, nullptr); return; }
            if (path == "/aabridge/aaw" && fd >= 0) {
                self->sessions_++;
                std::thread([self, fd, devs] {
                    self->waitForAp();
                    bool ok = btboot::runWifiBootstrap(fd, self->params(), [](const std::string& s) { LOG(s); });
                    LOG(std::string("wireless bootstrap ") + (ok ? "COMPLETE" : "INCOMPLETE (the phone did not send its final Wi-Fi status over Bluetooth; it may still join and connect, watch the [Wifi]/[HeadUnit] lines)") + " for " << devs);
                    // give the phone a moment before dropping the RFCOMM link
                    std::this_thread::sleep_for(std::chrono::seconds(ok ? 5 : 0));
                    ::close(fd);
                    self->sessions_--;
                }).detach();
            } else if (fd >= 0) {
                ::close(fd);   // HSP trigger profile: accept and drop (no AT state machine; matches aa-proxy-rs)
            }
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else {
            g_dbus_method_invocation_return_value(inv, nullptr);
        }
    }

    static void onAgentCall(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                            GVariant* params, GDBusMethodInvocation* inv, gpointer) {
        std::string m = method;
        LOG("pairing agent: " << m);
        if (m == "RequestPinCode") g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
        else if (m == "RequestPasskey") g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0u));
        else { (void)params; g_dbus_method_invocation_return_value(inv, nullptr); }   // confirmations/authorizations: accept
    }

    bool exportObject(const char* xml, const char* path, GDBusInterfaceVTable* vt) {
        GError* e = nullptr;
        GDBusNodeInfo* ni = g_dbus_node_info_new_for_xml(xml, &e);
        if (!ni) { LOG("introspection xml error: " << (e ? e->message : "?")); return false; }
        guint id = g_dbus_connection_register_object(conn_, path, ni->interfaces[0], vt, this, nullptr, &e);
        g_dbus_node_info_unref(ni);
        if (!id) { LOG("register_object " << path << " failed: " << (e ? e->message : "?")); return false; }
        return true;
    }

    void registerProfile(const char* path, const char* uuid, const char* name, bool withChannel) {
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Name", g_variant_new_string(name));
        if (withChannel) g_variant_builder_add(&b, "{sv}", "Role", g_variant_new_string("server"));      // the HSP profile keeps the default role (client+server): with "server" BlueZ refuses to ConnectProfile out to the phone (55 of 55 nudges failed "profile-unavailable" in truck 36)
        g_variant_builder_add(&b, "{sv}", "RequireAuthentication", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&b, "{sv}", "RequireAuthorization", g_variant_new_boolean(FALSE));
        if (withChannel) g_variant_builder_add(&b, "{sv}", "Channel", g_variant_new_uint16(8));
        GVariant* r = call("/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", g_variant_new("(osa{sv})", path, uuid, &b));
        if (r) { LOG("registered profile " << name << " (" << uuid << ")"); g_variant_unref(r); }
    }

    // ---- nudge paired phones ------------------------------------------------------------------------------
    // One-line description of a paired phone's Bluetooth state (what a failed connection needs to be diagnosed from): logged when it CHANGES.
    static std::string describeDevice(GVariant* dev) {
        gboolean conn = FALSE, res = FALSE, trusted = FALSE; gint16 rssi = 0; const gchar* name = nullptr;
        g_variant_lookup(dev, "Connected", "b", &conn); g_variant_lookup(dev, "ServicesResolved", "b", &res); g_variant_lookup(dev, "Trusted", "b", &trusted);
        const bool hasRssi = g_variant_lookup(dev, "RSSI", "n", &rssi); g_variant_lookup(dev, "Alias", "&s", &name);
        bool aaw = false, hfpag = false, hspag = false, a2dp = false; int n = 0;
        if (GVariant* u = g_variant_lookup_value(dev, "UUIDs", G_VARIANT_TYPE("as"))) {
            GVariantIter it; g_variant_iter_init(&it, u); const gchar* s;
            while (g_variant_iter_next(&it, "&s", &s)) { ++n; std::string v = s; for (auto& ch : v) ch = (char)tolower((unsigned char)ch);
                if (v.rfind("4de17a00", 0) == 0) aaw = true; else if (v.rfind("0000111f", 0) == 0) hfpag = true; else if (v.rfind("00001112", 0) == 0) hspag = true; else if (v.rfind("0000110a", 0) == 0) a2dp = true; }
            g_variant_unref(u);
        }
        std::ostringstream o; o << (name ? name : "?") << ": Connected=" << (conn ? 1 : 0) << " ServicesResolved=" << (res ? 1 : 0) << " Trusted=" << (trusted ? 1 : 0)
            << " RSSI=" << (hasRssi ? std::to_string(rssi) : std::string("n/a")) << " UUIDs=" << n << " [AAwireless=" << aaw << " HFP-AG=" << hfpag << " HSP-AG=" << hspag << " A2DP-src=" << a2dp << "]";
        return o.str();
    }

    // Self-heal for the phone adapter's visibility: BlueZ (main.conf DiscoverableTimeout, an adapter power cycle, a USB re-enumeration of the dongle) can drop
    // Discoverable/Pairable behind our back; the user then sees only the vehicle dongle ("Pixel") when trying to pair a phone (ui61 first pairing). Checked each nudge.
    int disc_heals_ = 0;
    void healDiscoverable() {
        GVariant* r = call(ad_.c_str(), "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.bluez.Adapter1", "Discoverable")); if (!r) return;
        GVariant* v = nullptr; g_variant_get(r, "(v)", &v); const bool disc = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v); if (v) g_variant_unref(v); g_variant_unref(r);
        if (disc) return;
        const char* ad = ad_.c_str();
        setProp(ad, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE));
        setProp(ad, "org.bluez.Adapter1", "Pairable", g_variant_new_boolean(TRUE));
        setProp(ad, "org.bluez.Adapter1", "PairableTimeout", g_variant_new_uint32(0));
        setProp(ad, "org.bluez.Adapter1", "DiscoverableTimeout", g_variant_new_uint32(0));
        setProp(ad, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean(TRUE));
        if (++disc_heals_ <= 20 || disc_heals_ % 100 == 0) LOG("phone adapter " << ad_ << " was NOT discoverable: made discoverable+pairable again (" << disc_heals_ << "x)");
    }
    void nudgePairedPhones() {
        healDiscoverable();
        if (sessions_ > 0) return;
        GVariant* r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr);
        if (!r) return;
        GVariant* objs = g_variant_get_child_value(r, 0);
        GVariantIter it; g_variant_iter_init(&it, objs);
        const gchar* path; GVariant* ifaces;
        std::vector<std::string> targets, connTargets;
        while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            if (GVariant* dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"))) {
                gboolean paired = FALSE, connected = FALSE; g_variant_lookup(dev, "Paired", "b", &paired); g_variant_lookup(dev, "Connected", "b", &connected);
                if (paired && std::string(path).rfind(ad_ + "/", 0) != 0) paired = FALSE;       // devices of the dongle (the vehicle) are not nudged
                if (paired) {
                    gboolean trusted = FALSE; g_variant_lookup(dev, "Trusted", "b", &trusted);
                    if (!trusted && !trust_set_.count(path)) {      // an untrusted phone needs an authorization for each incoming link and BlueZ may refuse it: trust it so it reconnects by itself
                        trust_set_.insert(path); LOG("marking " << path << " Trusted (so the phone can reconnect on its own)");
                        callAsync(path, "org.freedesktop.DBus.Properties", "Set", g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE)), 5000, "Trust", path);
                    }
                    targets.push_back(path); if (connected) connTargets.push_back(path);
                    const std::string d = describeDevice(dev);
                    if (last_state_[path] != d) { last_state_[path] = d;
                        LOG("phone state (t+" << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_ready_).count() << " s) " << path << " " << d); }
                }
                g_variant_unref(dev);
            }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objs); g_variant_unref(r);
        // Phone on Bluetooth but no Android Auto session for ~50 s (it does not always restart the Wi-Fi handoff by itself after a drop):
        // bounce the Bluetooth link so the phone runs its "connected to car" logic again. Capped so it cannot loop.
        if (!connTargets.empty() && !phone_active_) ++stale_; else stale_ = 0;
        if (stale_ >= 6 && kicks_ < 4) {
            ++kicks_; stale_ = -4;
            for (auto& t : connTargets) {
                LOG("phone on Bluetooth with no Android Auto session for ~" << 6 * nudge_s_ << " s; bouncing the link (" << kicks_ << "/4) " << t);
                callAsync(t, "org.bluez.Device1", "Disconnect", nullptr, 10000, "Disconnect", t);
                kicked_.push_back(t);
            }
            g_timeout_add_seconds(3, onReconnect, this);
        }
        // bt_release_phone: the Android Auto session is up (Wi-Fi), so the phone's Bluetooth link to us is only in the way: while it is up the phone may route its calls
        // to us (a fake headset) instead of to the vehicle. Disconnect it, a few times at most per session (the phone may reconnect on its own).
        if (cfg_.bt_release_phone) {
            const bool act = phone_active_;
            if (act && !was_active_) { release_wait_ = 0; releases_ = 0; }
            was_active_ = act;
            if (act && !connTargets.empty() && releases_ < 8) {
                if (++release_wait_ * nudge_s_ >= cfg_.bt_release_delay_s) {
                    ++releases_; release_wait_ = -3;
                    for (auto& t : connTargets) {
                        LOG("release: Android Auto runs over Wi-Fi; disconnecting the phone's Bluetooth link to us so its hands-free can use the vehicle (" << releases_ << "/8) " << t);
                        callAsync(t, "org.bluez.Device1", "Disconnect", nullptr, 10000, "Release", t);
                    }
                }
            } else if (!act || connTargets.empty()) release_wait_ = 0;
        }
        const auto now = std::chrono::steady_clock::now();
        // ui34a: a nudge during the Android Auto session re-opened the headset profile on the phone and that CLOSED the phone's hands-free link to us
        // (Android treats headset and hands-free as one service). A phone with a session up, or a hands-free link to us, is left alone.
        const bool phone_busy = phone_active_ || (call_ && call_->phoneLinked());
        if (phone_busy != nudge_paused_) { nudge_paused_ = phone_busy; LOG((phone_busy ? std::string("nudges paused (phone session / hands-free link up)") : std::string("nudges resume (no phone session)"))); }
        // Hands-free keeper (ui35a): the session runs but the phone has no hands-free link to us (it came in as a headset, or the link dropped):
        // connect the hands-free profile, every bt_hf_keep_s. Never the headset profile here (that is what put the phone in headset mode).
        const bool hf_missing = call_ ? !call_->phoneLinked() : (getenv("AA_TEST_HF_MISSING") != nullptr);
        if (wantHfp() && phone_active_ && hf_missing && !connTargets.empty()) {
            if (++hf_keep_wait_ * nudge_s_ >= std::max(1, cfg_.bt_hf_keep_s)) {
                hf_keep_wait_ = 0;
                for (auto& t : connTargets) {
                    if (inflight_.count(t)) continue;
                    LOG("hands-free link missing while the session runs: connecting it " << t);
                    inflight_.insert(t);
                    callAsync(t, "org.bluez.Device1", "ConnectProfile", g_variant_new("(s)", kHfpAg), 15000, "ConnectProfile(HFP)", t, [this, t](bool, bool) { inflight_.erase(t); });
                }
            }
        } else hf_keep_wait_ = 0;
        for (auto& t : targets) {
            if (phone_busy) break;
            if (inflight_.count(t)) continue;                                        // the previous nudge has not finished: never stack calls
            auto b = backoff_until_.find(t); if (b != backoff_until_.end() && now < b->second) continue;
            const bool hfp = wantHfp() && !hsp_fallback_.count(t);
            LOG("nudging " << t << " via " << (hfp ? "HFP AG (hands-free)" : "HSP AG") << " ConnectProfile");
            inflight_.insert(t);
            callAsync(t, "org.bluez.Device1", "ConnectProfile", g_variant_new("(s)", hfp ? kHfpAg : kHspAg), 15000, "ConnectProfile", t, [this, t, hfp](bool ok, bool timed_out) {
                inflight_.erase(t);
                if (timed_out) { backoff_until_[t] = std::chrono::steady_clock::now() + std::chrono::seconds(30); LOG("phone unreachable (ConnectProfile timed out); next nudge in 30 s"); }
                else if (!ok && hfp && !hsp_fallback_.count(t) && last_err_.find("already") == std::string::npos && last_err_.find("busy") == std::string::npos) { hsp_fallback_.insert(t); LOG("hands-free connect refused by " << t << ": waking it over the headset profile from now on (the hands-free keeper adds hands-free once the session runs)"); }
            });
        }
    }

    static gboolean onReconnect(gpointer ud) {
        auto* s = static_cast<Impl*>(ud);
        for (auto& t : s->kicked_) { LOG("reconnecting " << t); s->callAsync(t, "org.bluez.Device1", "Connect", nullptr, 20000, "Connect", t); }
        s->kicked_.clear(); return G_SOURCE_REMOVE;
    }
    std::string adapterAddress(const std::string& path) {
        GVariant* r = call(path.c_str(), "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.bluez.Adapter1", "Address")); std::string out;
        if (r) { GVariant* v = nullptr; g_variant_get(r, "(v)", &v); if (v) { const char* s = g_variant_get_string(v, nullptr); if (s) out = s; g_variant_unref(v); } g_variant_unref(r); }
        return out;
    }
    // Pair + connect to the vehicle ourselves. Sync 4 answered our address announcement but never opened a Bluetooth link, so the Pi starts it (as Android does with
    // BluetoothDevice.createBond on the car address from the service channel). Order: D-Bus Adapter1.ConnectDevice (bluetoothd --experimental), else btmgmt pair
    // (kernel management interface, no discovery needed), then trust + Device1.Connect. Everything is logged; retried a few times.
    std::atomic<bool> car_busy_{false}; std::thread car_thread_;
public:
    // Adapter roles. By address when configured; otherwise onboard (non-USB) = phone, first USB = vehicle; with NO onboard adapter and two USB
    // dongles (onboard radio disabled), the lower address becomes the phone side and the higher the vehicle side, logged once so the user can pin them.
    std::string phoneAdapterName() {
        auto v = btcall::listAdapters();
        if (!cfg_.bt_phone_adapter.empty()) { for (auto& a : v) if (a.address == cfg_.bt_phone_adapter) return a.name; return ""; }
        for (auto& a : v) if (!a.usb) return a.name;
        std::vector<btcall::AdapterInfo> usb; for (auto& a : v) if (a.usb && !a.address.empty() && a.address != cfg_.bt_vehicle_adapter) usb.push_back(a);
        if (usb.size() >= 2 || (usb.size() == 1 && !cfg_.bt_vehicle_adapter.empty())) {
            std::sort(usb.begin(), usb.end(), [](const btcall::AdapterInfo& x, const btcall::AdapterInfo& y) { return x.address < y.address; });
            if (!roles_warned_.exchange(true)) LOG("no onboard adapter and bt_phone_adapter not set: using " << usb[0].name << " (" << usb[0].address << ") as the PHONE adapter; set bt_phone_adapter/bt_vehicle_adapter to pin the roles");
            return usb[0].name; }
        return "";
    }
    std::string vehicleAdapterName() {
        auto v = btcall::listAdapters();
        if (!cfg_.bt_vehicle_adapter.empty()) { for (auto& a : v) if (a.address == cfg_.bt_vehicle_adapter) return a.name; return ""; }
        const std::string ph = ad_.size() > 11 ? ad_.substr(11) : "";       // "/org/bluez/hciN" -> "hciN": never the phone adapter
        std::vector<btcall::AdapterInfo> usb; for (auto& a : v) if (a.usb && a.name != ph) usb.push_back(a);
        if (usb.empty()) return "";
        std::sort(usb.begin(), usb.end(), [](const btcall::AdapterInfo& x, const btcall::AdapterInfo& y) { return x.address < y.address; });
        return usb.back().name;
    }
    std::atomic<bool> roles_warned_{false};
    void pairWithCar(const std::string& car) {
        if (!cfg_.bt_pair_initiate || car.empty() || stopping_) return;
        if (car_busy_.exchange(true)) return;
        if (car_thread_.joinable()) car_thread_.join();
        car_thread_ = std::thread([this, car] { carPairLoop(car); car_busy_ = false; });
    }
private:
    bool devFlag(const std::string& path, const char* prop) {
        GError* e = nullptr; GVariant* r = g_dbus_connection_call_sync(conn_, "org.bluez", path.c_str(), "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.bluez.Device1", prop), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &e);
        if (e) g_error_free(e); if (!r) return false;
        GVariant* v = nullptr; g_variant_get(r, "(v)", &v); bool b = v && g_variant_get_boolean(v); if (v) g_variant_unref(v); g_variant_unref(r); return b;
    }
    static int ioCapCode(const std::string& s) { return s == "DisplayOnly" ? 0 : s == "DisplayYesNo" ? 1 : s == "KeyboardOnly" ? 2 : s == "KeyboardDisplay" ? 4 : 3; }
    void carPairLoop(const std::string& car) {
        if (cfg_.bt_call_bridge) { for (int i = 0; i < 300 && !bridge_ready_ && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!bridge_ready_) LOG("pair-initiate: call bridge not ready after 30 s (no dongle?); pairing anyway"); }
        const std::string dg = dongle(); const std::string adapter = (!dg.empty() && !adapterAddress(dg).empty()) ? dg : ad_;
        std::string under = car; for (char& ch : under) if (ch == ':') ch = '_';
        const std::string dev = adapter + "/dev_" + under; const int idx = atoi(adapter.substr(adapter.rfind("hci") + 3).c_str());
        LOG("pair-initiate: vehicle " << car << " via " << adapter << " (device path " << dev << ")");
        for (int attempt = 1; attempt <= 6 && !stopping_; ++attempt) {
            if (devFlag(dev, "Paired")) {
                LOG("pair-initiate: already paired with the vehicle (attempt " << attempt << ")");
            } else {
                GError* e = nullptr; GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}")); g_variant_builder_add(&b, "{sv}", "Address", g_variant_new_string(car.c_str()));
                GVariant* r = g_dbus_connection_call_sync(conn_, "org.bluez", adapter.c_str(), "org.bluez.Adapter1", "ConnectDevice", g_variant_new("(a{sv})", &b), nullptr, G_DBUS_CALL_FLAGS_NONE, 40000, nullptr, &e);
                if (r) { LOG("pair-initiate: ConnectDevice ok (attempt " << attempt << ")"); g_variant_unref(r); }
                else { LOG("pair-initiate: ConnectDevice failed: " << (e ? e->message : "?")); if (e) g_error_free(e); }
                if (!devFlag(dev, "Paired")) {
                    const std::string cmd = "timeout 45 btmgmt -i " + std::to_string(idx) + " pair -c " + std::to_string(ioCapCode(cfg_.bt_io_cap)) + " -t 0 " + car + " 2>&1";
                    LOG("pair-initiate: trying '" << cmd << "'");
                    const auto t0 = std::chrono::steady_clock::now(); int lines = 0, rc = -1;
                    if (FILE* f = popen(cmd.c_str(), "r")) { char line[300]; while (fgets(line, sizeof line, f)) { ++lines; std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) LOG("pair-initiate: btmgmt: " << s); } rc = pclose(f); }
                    LOG("pair-initiate: btmgmt finished: exit " << (rc >= 0 ? WEXITSTATUS(rc) : rc) << " after " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms, " << lines << " output line(s)" << (rc >= 0 && WEXITSTATUS(rc) == 124 ? " (TIMED OUT at 45 s)" : ""));
                }
            }
            if (devFlag(dev, "Paired")) {
                setProp(dev.c_str(), "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE));
                GError* e = nullptr; GVariant* r = g_dbus_connection_call_sync(conn_, "org.bluez", dev.c_str(), "org.bluez.Device1", "Connect", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &e);
                if (r) { LOG("pair-initiate: Device1.Connect ok"); g_variant_unref(r); } else { LOG("pair-initiate: Device1.Connect failed: " << (e ? e->message : "?")); if (e) g_error_free(e); }
                LOG("pair-initiate: paired with the vehicle; connected=" << devFlag(dev, "Connected"));
                return;
            }
            LOG("pair-initiate: attempt " << attempt << " did not pair; retrying in 12 s");
            for (int i = 0; i < 12 && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LOG("pair-initiate: gave up (the vehicle never paired)");
    }
    // log every Bluetooth device the stack knows about whenever its state changes (what did the vehicle do after we announced our address?)
    void watchDevices() {
        std::map<std::string, std::string> last;
        while (!stopping_) {
            GVariant* r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr);
            if (r) {
                GVariant* objs = g_variant_get_child_value(r, 0); GVariantIter it; g_variant_iter_init(&it, objs); const gchar* path; GVariant* ifaces;
                while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
                    if (GVariant* dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"))) {
                        auto str = [&](const char* k) { std::string s; if (GVariant* v = g_variant_lookup_value(dev, k, nullptr)) { if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) s = g_variant_get_string(v, nullptr); g_variant_unref(v); } return s; };
                        auto flag = [&](const char* k) { bool b = false; if (GVariant* v = g_variant_lookup_value(dev, k, G_VARIANT_TYPE_BOOLEAN)) { b = g_variant_get_boolean(v); g_variant_unref(v); } return b; };
                        std::string uu; if (GVariant* v = g_variant_lookup_value(dev, "UUIDs", nullptr)) { GVariantIter ui; g_variant_iter_init(&ui, v); const gchar* u; while (g_variant_iter_next(&ui, "&s", &u)) { std::string s = u; if (s.rfind("0000111", 0) == 0 || s.rfind("0000110", 0) == 0) uu += s.substr(4, 4) + " "; } g_variant_unref(v); }
                        char buf[400]; snprintf(buf, sizeof buf, "%s '%s' paired=%d connected=%d trusted=%d svc=[%s]", path, str("Name").c_str(), flag("Paired"), flag("Connected"), flag("Trusted"), uu.c_str());
                        auto f = last.find(path); if (f == last.end() || f->second != buf) { last[path] = buf; LOG("device " << buf); }
                        g_variant_unref(dev);
                    }
                    g_variant_unref(ifaces);
                }
                g_variant_unref(objs); g_variant_unref(r);
            }
            for (int i = 0; i < 4 && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    int pairedOnDongle() {
        int n = 0; GVariant* r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr); if (!r) return 0;
        GVariant* objs = g_variant_get_child_value(r, 0); GVariantIter it; g_variant_iter_init(&it, objs); const gchar* path; GVariant* ifaces;
        while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            if (std::string(path).rfind(dongle_ + "/", 0) == 0) if (GVariant* dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"))) {
                gboolean paired = FALSE; g_variant_lookup(dev, "Paired", "b", &paired); if (paired) ++n; g_variant_unref(dev); }
            g_variant_unref(ifaces);
        }
        g_variant_unref(objs); g_variant_unref(r); return n;
    }
    void registerHfp(const char* path, const char* uuid, const char* name) {
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Name", g_variant_new_string(name));
        g_variant_builder_add(&b, "{sv}", "RequireAuthentication", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&b, "{sv}", "RequireAuthorization", g_variant_new_boolean(FALSE));
        GVariant* r = call("/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", g_variant_new("(osa{sv})", path, uuid, &b));
        if (r) { LOG("registered profile " << name << " (" << uuid << ")"); g_variant_unref(r); } else LOG("RegisterProfile " << name << " failed");
    }
    // Call audio over Android Auto (bt_call_aa): the onboard adapter is the phone's hands-free unit; its call audio goes to the vehicle through the AA sound channel.
public:
    std::shared_ptr<btcall::AaCall> aa_;
private:
    void setupCallAa() {
        static GDBusInterfaceVTable pvt2 = {onProfileCall, nullptr, nullptr, {}};
        exportObject(kProfileXml, "/aabridge/hfphf", &pvt2);
        const std::string pa = adapterAddress(ad_);
        if (pa.empty()) { LOG("call audio over Android Auto: adapter address unavailable; disabled"); return; }
        if (cfg_.bt_call_sco_route) { std::string why; const int id = atoi(ad_.substr(ad_.rfind("hci") + 3).c_str()); const bool ok = btcall::routeScoToHci(id, &why); LOG("call audio over Android Auto: SCO routing " << ad_ << ": " << why << (ok ? "" : " (FAILED)")); }
        registerHfp("/aabridge/hfphf", kHfpHf, "Hands-Free unit");
        call_ = std::make_unique<btcall::CallBridge>(cfg_);
        call_->setAaCall(aa_);
        call_->setAdapters(pa, "");
        LOG("call audio over Android Auto: ready (phone adapter " << ad_ << " " << pa << ")");
    }
    // Call bridge: the dongle is the "phone" the vehicle pairs with (hands-free audio gateway); the onboard adapter keeps talking to the real phone (hands-free unit).
    void setupCallBridge() {
        static GDBusInterfaceVTable pvt2 = {onProfileCall, nullptr, nullptr, {}};
        exportObject(kProfileXml, "/aabridge/hfphf", &pvt2);
        exportObject(kProfileXml, "/aabridge/hfpag", &pvt2);
        for (int i = 0; i < 30; ++i) { if (!adapterAddress(dongle_).empty()) break; std::this_thread::sleep_for(std::chrono::seconds(1)); }
        const std::string pa = adapterAddress(ad_), va = adapterAddress(dongle_);
        if (pa.empty() || va.empty()) { LOG("call bridge: adapter address unavailable (phone '" << pa << "', vehicle '" << va << "'); disabled"); return; }
        const char* d = dongle_.c_str();
        setProp(d, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE));
        setProp(d, "org.bluez.Adapter1", "Alias", g_variant_new_string(cfg_.bt_call_name.c_str()));
        setProp(d, "org.bluez.Adapter1", "Pairable", g_variant_new_boolean(TRUE));
        setProp(d, "org.bluez.Adapter1", "PairableTimeout", g_variant_new_uint32(0));
        const int paired = pairedOnDongle();
        const bool disc = cfg_.bt_call_discoverable == 1 || (cfg_.bt_call_discoverable < 0 && paired == 0);
        setProp(d, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean(disc ? TRUE : FALSE));
        setProp(d, "org.bluez.Adapter1", "DiscoverableTimeout", g_variant_new_uint32(0));
        const std::string idx = dongle_.substr(dongle_.rfind("hci") + 3);
        std::string cmd = "btmgmt --index " + idx + " class 2 3 >/dev/null 2>&1"; int rc = ::system(cmd.c_str());        // look like a smartphone to the vehicle
        LOG("call bridge: dongle " << dongle_ << " (" << va << ") named '" << cfg_.bt_call_name << "', " << paired << " paired vehicle(s), " << (disc ? "VISIBLE for pairing" : "hidden") << ", class cmd rc=" << rc << "; phone adapter " << ad_ << " (" << pa << ")");
        if (cfg_.bt_call_sco_route) for (const std::string& p : {ad_, dongle_}) { std::string why; int id = atoi(p.substr(p.rfind("hci") + 3).c_str()); bool ok = btcall::routeScoToHci(id, &why); LOG("call bridge: SCO routing " << p << ": " << why << (ok ? "" : " (call audio may be silent)")); }
        registerHfp("/aabridge/hfphf", kHfpHf, "Hands-Free unit");
        registerHfp("/aabridge/hfpag", kHfpAg, "Hands-Free Audio Gateway");
        call_ = std::make_unique<btcall::CallBridge>(cfg_);
        call_->setAdapters(pa, va);
        bridge_ready_ = true;
        if (cfg_.bt_call_sim) LOG("call bridge: TEST CALL mode on (bt_call_sim): a test call rings on the vehicle " << cfg_.bt_call_sim_delay_s << " s after it links");
    }
    static gboolean onNudge(gpointer ud) { auto* s = static_cast<Impl*>(ud); if (!s->stopping_) s->nudgePairedPhones(); return G_SOURCE_CONTINUE; }

    void run() {
        GError* e = nullptr;
        conn_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
        if (!conn_) { LOG("no system D-Bus (" << (e ? e->message : "?") << "); Bluetooth bootstrap disabled"); return; }
        if (cfg_.bt_call_bridge) {                                  // two adapters: which is which is decided by address (config) or by the bus, never by the hciN number
            bool found = false;
            for (int i = 0; i < 60 && !stopping_; ++i) {             // the phone adapter is needed now (the vehicle adapter is looked for later, in the background); addresses are known once bluetoothd opened the adapters (~12 s)
                const std::string on = phoneAdapterName();
                if (!on.empty()) { ad_ = "/org/bluez/" + on; found = true; break; }
                if (i == 15) { std::string seen; for (auto& a : btcall::listAdapters()) seen += a.name + "=" + (a.address.empty() ? "?" : a.address) + (a.usb ? "(usb) " : "(onboard) "); LOG("phone adapter not found yet; adapters: " << (seen.empty() ? "none" : seen)); }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!found) { std::string seen; for (auto& a : btcall::listAdapters()) seen += a.name + "=" + (a.address.empty() ? "?" : a.address) + (a.usb ? "(usb) " : "(onboard) ");
                LOG("PHONE ADAPTER NOT FOUND (bt_phone_adapter=" << cfg_.bt_phone_adapter << "); adapters: " << (seen.empty() ? "none" : seen) << " -> using " << ad_ << " as a last resort"); }
            else LOG("call bridge enabled: phone adapter " << ad_ << (cfg_.bt_phone_adapter.empty() ? " (first non-USB adapter)" : " (bt_phone_adapter=" + cfg_.bt_phone_adapter + ")"));
        }
        // wait for bluetoothd + adapter
        for (int i = 0; i < 60 && !stopping_; ++i) {
            GVariant* r = g_dbus_connection_call_sync(conn_, "org.bluez", ad_.c_str(), "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", "org.bluez.Adapter1", "Address"), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
            if (r) { g_variant_unref(r); break; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (i == 59) { LOG("no Bluetooth adapter " << ad_ << " after 60 s; bootstrap disabled"); return; }
        }
        if (stopping_) return;                                       // stop() during the start-up waits: do not enter a loop nobody will quit
        loop_ = g_main_loop_new(nullptr, FALSE);

        static GDBusInterfaceVTable pvt = {onProfileCall, nullptr, nullptr, {}};
        static GDBusInterfaceVTable avt = {onAgentCall, nullptr, nullptr, {}};
        exportObject(kProfileXml, "/aabridge/aaw", &pvt);
        exportObject(kProfileXml, "/aabridge/hsp", &pvt);
        exportObject(kAgentXml, "/aabridge/agent", &avt);

        const char* ad = ad_.c_str();
        setProp(ad, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE));
        setProp(ad, "org.bluez.Adapter1", "Alias", g_variant_new_string(cfg_.bt_name.c_str()));
        setProp(ad, "org.bluez.Adapter1", "Pairable", g_variant_new_boolean(TRUE));
        setProp(ad, "org.bluez.Adapter1", "PairableTimeout", g_variant_new_uint32(0));
        setProp(ad, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean(TRUE));
        setProp(ad, "org.bluez.Adapter1", "DiscoverableTimeout", g_variant_new_uint32(0));

        GVariant* r = call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", g_variant_new("(os)", "/aabridge/agent", cfg_.bt_io_cap.c_str()));
        if (r) g_variant_unref(r);
        r = call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", g_variant_new("(o)", "/aabridge/agent"));
        if (r) { LOG("pairing agent registered (" << cfg_.bt_io_cap << ", auto-accept)"); g_variant_unref(r); }
        registerProfile("/aabridge/aaw", kAawUuid, "AA Wireless", true);
        registerProfile("/aabridge/hsp", kHspHs, "HSP HS", false);
        if (cfg_.bt_call_aa && !cfg_.bt_call_bridge && aa_) setupCallAa();
        if (cfg_.bt_call_bridge) dongle_thread_ = std::thread([this] {      // the USB dongle may enumerate a few seconds after boot (or be plugged in later): look for up to 3 minutes
            for (int i = 0; i < 180 && !stopping_; ++i) {
                { const std::string dn = vehicleAdapterName(); if (!dn.empty()) { { std::lock_guard<std::mutex> g(dongle_m_); dongle_ = "/org/bluez/" + dn; } LOG("call bridge: found the vehicle adapter " << dn << (cfg_.bt_vehicle_adapter.empty() ? " (first USB adapter)" : " (bt_vehicle_adapter=" + cfg_.bt_vehicle_adapter + ")")); setupCallBridge(); return; } }
                if (i == 20) LOG("call bridge: no USB Bluetooth dongle found yet (still looking)");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            LOG("call bridge: no USB Bluetooth dongle appeared; calls will not be bridged");
        });

        if (cfg_.bt_monitor) { const int rc = ::system("systemctl restart aa-bridge-btmon.service > /dev/null 2>&1"); LOG("HCI monitor: aa-bridge-btmon.service (btmon -w /var/log/aa-bridge-btmon-up<uptime>-<epoch>.log, supervised by systemd, Restart=always) rc " << rc); }
        LOG("ready: discoverable as '" << cfg_.bt_name << "'; wifi ssid=" << cfg_.ap_ssid << " bssid=" << params().bssid << " ip=192.168.50.1:5277");
        if (const char* e = getenv("AA_BT_NUDGE_S")) nudge_s_ = std::max(1, atoi(e));        // test hook
        t_ready_ = std::chrono::steady_clock::now();
        g_timeout_add_seconds(nudge_s_, onNudge, this);
        if (cfg_.bt_pair_proxy) watch_thread_ = std::thread([this] { watchDevices(); });
        if (stopping_) return;
        g_main_loop_run(loop_);
        stopping_ = true; if (watch_thread_.joinable()) watch_thread_.join(); if (car_thread_.joinable()) car_thread_.join(); if (dongle_thread_.joinable()) dongle_thread_.join(); if (call_) { call_->stop(); call_.reset(); }
        g_main_loop_unref(loop_); loop_ = nullptr;
        g_object_unref(conn_); conn_ = nullptr;
    }
};

BtBootstrap::BtBootstrap(const Config& c) : impl_(std::make_unique<Impl>(c)) {}
BtBootstrap::~BtBootstrap() { stop(); }
void BtBootstrap::start() { impl_->start(); }
void BtBootstrap::stop() { if (impl_) impl_->stop(); }
void BtBootstrap::setAaCall(std::shared_ptr<btcall::AaCall> a) { if (impl_) impl_->aa_ = std::move(a); }
void BtBootstrap::pairWithCar(const std::string& a) { if (impl_) impl_->pairWithCar(a); }
std::string BtBootstrap::pairAddress() { return impl_ ? impl_->pairAddress() : std::string(); }
void BtBootstrap::setPhoneActive(bool on) { if (impl_) impl_->setPhoneActive(on); }
