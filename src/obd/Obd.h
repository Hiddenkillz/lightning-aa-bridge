// OBD-II (UDS service 0x22) poller for the Ford F-150 Lightning over an ELM327-compatible USB adapter (vLinker FS).
// Own thread, own USB device; never touches the Android Auto path. Values are published as a Snapshot with a timestamp per field;
// a field older than ~3x its poll interval is "stale" and the UI shows a dash, never a frozen number.
// Request table and formulas: skill reference obd2-lightning-pids.md (pelican.clutch.engineering, confirmed on 2022-2025 trucks).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obd {

struct Field {
    double v = 0;          // decoded value in the unit named in Snapshot
    int64_t at_ms = 0;     // steady clock (ms) of the last good reply; 0 = never
    bool ok = false;       // true once a reply has decoded at least once
    bool fresh(int64_t now_ms, int max_age_ms) const { return ok && now_ms - at_ms <= max_age_ms; }
};

// Named-state decodes (raw byte kept in Field::v).
enum ChargingState { CHG_NOT_READY = 0, CHG_READY = 1, CHG_FAULT = 2, CHG_WAIT = 3, CHG_PRE_COND = 4, CHG_CHARGING = 5, CHG_DONE = 6, CHG_EXT = 7, CHG_INIT = 8 };
enum ThermalMode { THM_OFF = 0, THM_EQUALIZE = 1, THM_COOLANT_FLOW = 2, THM_COOLANT_FAN = 3, THM_COOLANT_MODERATE = 4, THM_COOLANT_MAX = 5, THM_HEAT = 15 };
const char* chargingStateName(int v);
const char* thermalModeName(int v);
const char* heaterModeName(int v);

struct Snapshot {
    int64_t now_ms = 0;
    // link
    bool port_present = false;        // the adapter's serial device exists and is open
    bool adapter_ok = false;          // ELM init answered
    bool vehicle_awake = false;       // at least one module answered within the last 10 s
    std::string port;                 // device path in use
    uint32_t requests = 0, replies = 0, errors = 0;
    // high-voltage battery (BECM 7E4)
    Field pack_v;        // V        480D
    Field pack_a;        // A        48F9 (fallback 480A if 48F9 never answers); positive = discharge (verify on first drive)
    Field pack_kw;       // kW       derived: pack_v * pack_a / 1000 (same sign as pack_a)
    Field soc;           // %        4801 (0.002 % resolution)
    Field soc_disp;      // %        4845
    Field energy_kwh;    // kWh      4848 energy remaining
    Field soh;           // %        490C
    Field cap_ah;        // Ah       485C
    Field throughput_ah; // Ah       485D lifetime charge throughput
    Field mod_v_min;     // V        4840
    Field mod_v_avg;     // V        4841
    Field soc_spread;    // %        483E
    Field t_pack;        // C        4800
    Field t_max, t_min, t_range, t_avg;   // C   4808 bytes 0..3
    Field t_coolant_in;  // C        4846
    Field chg_state;     // enum     484F
    Field chg_status;    // enum     484D
    Field chg_power_limit_kw; // kW  48FB what the pack allows in right now (why a charger tapers)
    Field soc_target;    // %        05A1 charge target the vehicle is set to
    Field chg_a_req, chg_v_req;   // A (4842 /100), V (4844 x2): what the pack asks of the charger
    Field chg_out_a;     // A        4850 charger output current measured
    // charger (SOBDM 7E2): polled every 5 s while plugged in, every 60 s otherwise
    Field evse_v, evse_a, evse_hz;      // 485E /100 V, 485F A, 4860 /2 Hz: AC input
    Field pilot_duty;    // %        4861 /2: what the EVSE offers (amps = duty x 0.6 for 10..85 %)
    Field chg_out_v;     // V        484A /100
    Field chg_out_kw_max;// kW       48C4 /20 charger maximum power
    Field chg_in_kw_avail; // kW     484E /200 input power available
    Field coupler_ac_c;  // C        4888 -40
    Field coupler_dc1_c, coupler_dc3_c;   // C  4897 / 48A4 -40
    Field dc_status;     // enum     489E
    Field prox_status;   // enum     4862 (cable proximity)
    Field charger_status;// enum     484D @7E2 (HV charger status)
    // thermal / drive (PCM 7E6)
    Field thermal_mode;  // enum     48E0
    Field heater_mode;   // enum     48DF
    Field heater_kw;     // kW       48DE
    Field motor_coil_c;  // C        481F
    Field inverter_c;    // C        4824
    // 12 V system (BCM 726, DCDC 746)
    Field lv_v;          // V        402A
    Field lv_a;          // A        402B (negative = discharging)
    Field lv_soc;        // %        4028
    Field lv_age_h;      // h        4027
    Field lv_quiescent_ma; // mA     4025
    Field lv_state;      // enum?    420A battery state detection status (raw)
    Field dcdc_out_a;    // A        4836
    // environment / distance
    Field t_ext;         // C        DD05 @7E2
    Field t_int;         // C        DD04 @7E2
    Field odo_km;        // km       DD01 @7E0
    Field speed_obd_mps; // m/s      1505 @7E0 (val/128 km/h): fallback speed when the vehicle's AA sensor channel is not up
    // derived efficiency (speed comes from the vehicle's AA sensor channel via setSpeedMps())
    Field mi_per_kwh_30s;  // mi/kWh over a 30 s sliding window; v < 0 means net regen in the window (UI shows "regen")
    Field speed_mps;       // speed used for the window: Sync's sensor channel when fresh (<3 s), else speed_obd_mps
    bool speed_from_obd = false;
    // helpers
    bool preconditioning() const;   // chg_state == PRE_COND, or thermal_mode in {HEAT, COOLANT_*} (pack being actively conditioned)
    bool plugged() const;           // a charging cable is connected (chg_state in READY/WAIT/PRE_COND/CHARGING/DONE/EXT, or prox_status != 0)
    bool charging() const;          // energy going into the pack (chg_state CHARGING/EXT, or pack_a < -2 A while plugged)
    double evse_offer_a() const;    // amps offered by the EVSE from the pilot duty cycle (0 if unknown / DC)
};

class Poller {
public:
    struct Options {
        std::string port;                 // empty = first /dev/serial/by-id/* (or /dev/ttyUSB0, /dev/ttyACM0)
        int baud = 115200;
        bool log_raw = false;             // log every request/reply line (first drive)
        std::string log_prefix = "[Obd]";
    };
    explicit Poller(Options o);
    ~Poller();
    void start();
    void stop();
    Snapshot snapshot() const;
    void setSpeedMps(double mps);      // from the AA sensor channel (type 5 speed, m/s)
    std::string debugLine() const;     // one line for the Debug page: port, link state, counters
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Pure decode helpers (unit-tested without hardware): payload = data bytes AFTER the 0x62 DID DID echo.
// Returns false when the payload is too short.
bool decode(uint16_t module, uint16_t did, const std::vector<uint8_t>& payload, Snapshot& s, int64_t now_ms);

}  // namespace obd
