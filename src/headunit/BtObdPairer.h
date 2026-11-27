#pragma once
// BlueZ implementation of ui::ObdPairer: scan for Bluetooth Classic devices, pair + trust the one the user taps (an OBDLink LX is put in pairing mode with
// the button on it). Own system-bus connection and worker threads; never touches the GLib loop of BtBootstrap. Pairing relies on the default pairing agent
// that BtBootstrap registers (Just Works / auto-accept), so bt_bootstrap must be on.
#include <memory>
#include <string>
#include <vector>
#include "../ui/ObdPairer.h"

class BtObdPairer : public ui::ObdPairer {
public:
    explicit BtObdPairer(std::string adapter_path = "/org/bluez/hci0");
    ~BtObdPairer() override;
    void startScan() override;
    void stopScan() override;
    void pair(const std::string& addr) override;
    void forget(const std::string& addr) override;
    ui::ObdPairState state() override;

    // Pure helpers (unit-tested without BlueZ)
    static bool looksLikeObdAdapter(const std::string& name);                // OBDLink, vLinker, ELM327, ...
    static bool looksLikePhone(const std::vector<std::string>& uuids);        // exposes AA Wireless / hands-free AG / headset AG / A2DP source
    static void sortDevices(std::vector<ui::ObdPairDevice>& v);               // OBD-like first, then strongest signal, then name
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
