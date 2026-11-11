#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Config.h"
#include "../TouchFrame.h"
#include "../VideoParams.h"
#include "../compositor/Compositor.h"
#include "../NavRelay.h"
#include "../RawRelay.h"
#include "../AvRelay.h"

// Phone-facing side: listens for the phone's wireless-AA TCP connection and runs one HuSession per connection.
class HeadUnit {
public:
    using PresenceCallback = std::function<void(bool phone_connected)>;
    explicit HeadUnit(const Config& config, std::shared_ptr<Compositor> compositor);
    ~HeadUnit();
    void start();
    void stop();
    void setPresenceCallback(PresenceCallback cb);
    void setVisibleWindow(int x, int y, int w, int h);   // truck's visible region; advertised to the phone for sessions that start afterwards
    void injectTouch(int x, int y, int action);
    void setVideoParams(const VideoParams& v);
    void setPhoneSensors(int driving_status, bool night);   // what the phone is told (current session now, and every session that starts later)
    void setAvRelay(std::shared_ptr<AvRelay> r);     // audio / microphone / control hub (phone side)
    void setVehicleSensorCaps(const std::vector<int>& fuel, const std::vector<int>& ev);   // fuel types / EV connector types the vehicle declares (advertised to the phone)
    void setVehicleSensorTypes(const std::vector<int>& offered);   // sensors the vehicle offers (advertised to phones that connect afterwards)
    void relayVehicleSensors(const std::string& raw_batch);        // a raw SensorBatch from the vehicle -> the phone
    void sendUiConfig(int l, int r, int t, int b);                 // ask the connected phone for a new UI region (margins in the codec frame)
    void setUiConfigReplyCallback(std::function<void()> f);
    void setNavRelay(std::shared_ptr<NavRelay> r);   // navigation channel hub (phone side)
    void setMediaRelay(std::shared_ptr<RawRelay> r); // media playback status hub (phone side)
    void injectKey(int keycode, bool down, bool longpress);   // steering-wheel / media key from the vehicle -> the phone
    void injectTouchFrame(const TouchFrame& f);   // all fingers, phone coordinates   // from the router; coordinates in the phone video space
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
