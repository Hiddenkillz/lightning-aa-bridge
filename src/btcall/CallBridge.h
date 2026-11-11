#pragma once
// Bluetooth call bridge: the phone's hands-free call (control + audio) is relayed to the vehicle through a second Bluetooth adapter (a USB dongle).
//   phone (hci onboard) <-RFCOMM HFP-> HfClient ... CallCore ... AgServer <-RFCOMM HFP-> vehicle (hci dongle)
//   phone <-SCO-> [pump] <-SCO-> vehicle      (8 kHz CVSD, copied untouched)
// BtBootstrap owns BlueZ (profile registration, adapters); it hands the accepted RFCOMM fds here. Nothing in this class talks D-Bus.
#include <memory>
#include "AaCall.h"
#include <string>
#include <vector>
#include "../Config.h"

namespace btcall {

struct AdapterInfo { std::string name, address; bool usb = false; int dev_id = -1; };
std::string adapterAddressByIndex(int dev_id);          // bdaddr of hci<dev_id> via HCIGETDEVINFO (empty if unknown / not yet opened)
std::vector<AdapterInfo> listAdapters();                 // from /sys/class/bluetooth (usb = the adapter sits on a USB bus: the dongle)
bool routeScoToHci(int dev_id, std::string* why);        // Broadcom controllers send SCO to PCM pins by default; move it to the host (HCI) path. No-op for other vendors.

class CallBridge {
public:
    explicit CallBridge(const Config& cfg);
    ~CallBridge();
    void setAdapters(const std::string& phone_adapter_addr, const std::string& vehicle_adapter_addr);
    void onPhoneLink(int fd, const std::string& phone_addr);        // RFCOMM from the phone (HFP): takes ownership of fd
    void onVehicleLink(int fd, const std::string& vehicle_addr);    // RFCOMM from the vehicle (HFP): takes ownership of fd
    void setAaCall(std::shared_ptr<AaCall> aa);    // bt_call_aa: call audio goes to the vehicle over Android Auto instead of a vehicle Bluetooth link
    bool phoneLinked() const;                      // the phone has a hands-free link to us right now
    void stop();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace btcall
