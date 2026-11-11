#pragma once
// Bluetooth side of wireless Android Auto: the Pi (head unit / "dongle") makes the paired phone join our Wi-Fi AP
// and connect to our TCP HuSession port.
//
// Flow (verified against aa-proxy-rs bluetooth.rs `send_params`, dongle mode):
//   1. register BlueZ profiles: AA Wireless (UUID 4de17a00-52cb-11e6-bdf4-0800200c9a66, RFCOMM ch 8, server) and HSP HS
//      (car-kit identity), plus a NoInputNoOutput pairing agent; adapter pairable + discoverable.
//   2. nudge paired phones: Device1.ConnectProfile(HSP AG) every few seconds while no phone is connected.
//   3. phone opens RFCOMM to our AA Wireless profile -> we run runWifiBootstrap() on that fd:
//        HU -> WifiStartRequest(ip, port)      id 1
//        ph -> WifiInfoRequest                 id 2
//        HU -> WifiInfoResponse(ssid,key,bssid,WPA2=8,DYNAMIC=1)   id 3
//        ph -> WifiStartResponse               id 7
//        ph -> WifiConnectStatus               id 6
//      message = [len16 BE (payload only)][id16 BE][protobuf]
//   4. phone joins the AP, TCP-connects to ip:port (HuSession).
// NOTE: the sysroot's generated WifiSecurityMode enum numbers WPA2_PERSONAL as 5, but the wire value is 8 -> protobufs
// below are hand-encoded.
#include <functional>
#include <memory>
#include <string>
#include "../Config.h"
#include "../btcall/AaCall.h"

namespace btboot {
struct WifiParams { std::string ip, ssid, key, bssid; int port = 5277; };

std::string encodeWifiStartRequest(const WifiParams& p);
std::string encodeWifiInfoResponse(const WifiParams& p);

// Blocking exchange on a connected RFCOMM-like stream fd. Returns true when the phone completed all stages.
// `log` receives human-readable stage lines. timeout_ms applies per read.
bool runWifiBootstrap(int fd, const WifiParams& p, const std::function<void(const std::string&)>& log, int timeout_ms = 10000);
}

class BtBootstrap {
public:
    explicit BtBootstrap(const Config& cfg);
    ~BtBootstrap();
    void start();   // spawns the GLib main-loop thread; safe to call without a Bluetooth adapter (logs and idles)
    void stop();
    void pairWithCar(const std::string& car_address);   // bt_pair_initiate: pair/connect to the vehicle's Bluetooth address ourselves (background, retried)
    void setAaCall(std::shared_ptr<btcall::AaCall> a);   // bt_call_aa: where the phone's call audio goes (set before start())
    std::string pairAddress();      // the Bluetooth address to announce to the vehicle (dongle if present, else onboard); empty if unknown
    void setPhoneActive(bool on);   // AA session with the phone is up (suppresses the reconnect kick)
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
