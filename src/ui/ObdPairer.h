#pragma once
// What the on-screen "OBD adapter" page needs from Bluetooth: scan, list, pair. UiController only knows this interface (the BlueZ implementation,
// BtObdPairer, lives in the headunit side and is linked into aa-bridge only), so the page is testable with a fake.
#include <string>
#include <vector>

namespace ui {

struct ObdPairDevice {
    std::string name, addr;     // addr "AA:BB:CC:DD:EE:FF" (upper case)
    int rssi = 0;               // dBm, 0 = unknown
    bool obd_like = false;      // the name looks like an OBD-II adapter (OBDLink, vLinker, ELM327 ...): listed first
};

struct ObdPairState {
    enum Phase { Idle, Scanning, Pairing, Done, Failed } phase = Idle;
    std::string message;                 // one line for the page footer ("Press the button on the adapter..." / "Pairing OBDLink LX..." / why it failed)
    std::vector<ObdPairDevice> devices;  // seen during the current scan, best first
    std::string paired_addr;             // set when phase == Done: the adapter that was paired and trusted
    unsigned serial = 0;                 // bumps whenever anything above changes (the UI redraws only then)
};

class ObdPairer {
public:
    virtual ~ObdPairer() = default;
    virtual void startScan() = 0;                      // begin discovery (auto-stops after ~30 s); no-op while pairing
    virtual void stopScan() = 0;
    virtual void pair(const std::string& addr) = 0;   // pair + trust `addr` in the background; progress in state()
    virtual void forget(const std::string& addr) = 0; // remove the bonding
    virtual ObdPairState state() = 0;                  // thread-safe snapshot
};

}  // namespace ui
