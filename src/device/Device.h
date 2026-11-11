#pragma once
#include "../TouchFrame.h"
#include "../VideoParams.h"
#include <memory>
#include <functional>
#include "../Config.h"
#include "../compositor/Compositor.h"
#include "../NavRelay.h"
#include "../RawRelay.h"
#include "../AvRelay.h"

class HeadUnit;

class Device {
public:
    using TouchCallback = std::function<void(int x, int y, int action)>;

    Device(const Config& config,
           std::shared_ptr<Compositor> compositor,
           std::shared_ptr<HeadUnit> headunit);
    ~Device();

    void start();
    void stop();

    // Register callback for touch events received from Sync 4
    void setTouchCallback(TouchCallback cb);
    using TouchFrameCallback = std::function<void(const TouchFrame&)>;
    void setTouchFrameCallback(TouchFrameCallback cb);
    using KeyCallback = std::function<void(int keycode, bool down, bool longpress, int meta)>;
    void setKeyCallback(KeyCallback cb);
    using GeometryCallback = std::function<void(int w, int h)>;
    void setGeometryCallback(GeometryCallback cb);
    using VideoParamsCallback = std::function<void(const VideoParams&)>;
    void setVideoParamsCallback(VideoParamsCallback cb);
    using WindowCallback = std::function<void(int x, int y, int w, int h)>;
    void setVisibleWindowCallback(WindowCallback cb);
    using VehicleCallback = std::function<void(int what, int value)>;   // 1 = reverse, 2 = moving
    void setVehicleCallback(VehicleCallback cb);
    using SensorTypesCallback = std::function<void(const std::vector<int>&)>;
    using SensorRawCallback = std::function<void(const std::string&)>;
    using SensorCapsCallback = std::function<void(const std::vector<int>& fuel, const std::vector<int>& ev)>;
    void setSensorCaps(SensorCapsCallback cb);
    void setBtAddressProvider(std::function<std::string()> fn);
    void setCarAddressCallback(std::function<void(const std::string&)> fn);
    void setSensorRelay(SensorTypesCallback types, SensorRawCallback raw);   // vehicle sensors -> the phone side
    void setAvRelay(std::shared_ptr<AvRelay> r);     // audio / microphone / control hub (vehicle side)
    void setNavRelay(std::shared_ptr<NavRelay> r);   // navigation channel hub (vehicle side)
    void setMediaRelay(std::shared_ptr<RawRelay> r); // media playback status hub (vehicle side)

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};
