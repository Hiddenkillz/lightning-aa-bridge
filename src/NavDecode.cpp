#include "NavDecode.h"
#include <aap_protobuf/service/navigationstatus/message/NavigationStatus.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnEvent.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnDistanceEvent.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationState.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationCurrentPosition.pb.h>
namespace nsm = aap_protobuf::service::navigationstatus::message;
template <class M> static std::string tryParse(const char* name, const std::string& b) {
    M m; if (!m.ParseFromString(b)) return std::string(name) + "{unparsable}";
    std::string t = m.ShortDebugString(); return std::string(name) + "{" + (t.size() > 500 ? t.substr(0, 500) + "..." : t) + "}";
}
std::string explainNavMessage(uint16_t id, const std::string& b) {
    switch (id) {
    case 0x8001: return "InstrumentClusterStart"; case 0x8002: return "InstrumentClusterStop";
    case 0x8003: return tryParse<nsm::NavigationStatus>("NavigationStatus", b);
    case 0x8004: return tryParse<nsm::NavigationNextTurnEvent>("NavigationNextTurnEvent", b);
    case 0x8005: return tryParse<nsm::NavigationNextTurnDistanceEvent>("NavigationNextTurnDistanceEvent", b);
    case 0x8006: return tryParse<nsm::NavigationState>("NavigationState", b);
    case 0x8007: return tryParse<nsm::NavigationCurrentPosition>("NavigationCurrentPosition", b);
    case 0x8008: return "VehicleEnergyForecast(?) [EV routing data: see raw]";
    default: return "";
    }
}
