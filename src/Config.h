#pragma once
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <map>
#include <iostream>

struct Config {
    // WiFi
    std::string ap_ssid        = "AABridge";
    std::string ap_password    = "aabridgepass";
    std::string ap_iface       = "wlan0";

    // Bluetooth
    std::string bt_name        = "AA-Bridge";
    bool bt_bootstrap          = true;   // run the BlueZ wireless-AA bootstrap (BtBootstrap); off for truck tests that need no phone

    // USB gadget
    std::string usb_gadget_dir = "/sys/kernel/config/usb_gadget/aa_bridge";
    std::string udc_name       = "fe980000.usb";  // Pi 4 UDC
    std::string usb_ffs_mount  = "/dev/usb-ffs/accessory"; // must match UsbGadget.h
    // 0 = present AOA accessory PID (18d1:2d00) immediately from boot.
    // 1 = present a default phone identity (18d1:4ee1) first, wait for the
    //     head unit's AOA START (or 5s timeout), then re-enumerate as 2d00.
    bool usb_two_stage         = false;
    // Phone-role TLS identity presented to the head unit (we are the TLS server).
    std::string md_cert        = "/etc/aa-bridge/md_cert.pem";
    std::string md_key         = "/etc/aa-bridge/md_key.pem";
    std::string md_dhparam     = "/etc/aa-bridge/dhparam.pem";   // optional
    std::string md_ca          = "/etc/aa-bridge/galroot_cert.pem"; // optional

    // Head-unit-role TLS identity presented to the phone (we are the TLS client). The well-known AA head unit
    // certificate that aasdk/AAWirelessDongle ship; valid until 2048.
    int hu_width_margin = 0, hu_height_margin = 0;   // head-unit role: margins advertised to the phone (AAP UI resolution = codec - margins); test knob until the real window is wired
    std::string hu_cert        = "/etc/aa-bridge/hu_cert.pem";
    std::string hu_key         = "/etc/aa-bridge/hu_key.pem";

    // Camera RTSP
    std::string camera_rtsp    = "rtsp://192.168.50.2:554/mjpeg/1";
    int camera_overlay_x       = 0;    // pixels from right edge
    int camera_overlay_y       = 0;    // pixels from bottom edge
    int camera_overlay_w       = 480;  // overlay width
    int camera_overlay_h       = 270;  // overlay height
    int camera_width           = 640;  // camera source size (for safe-zone crop maths)
    int camera_height          = 480;
    bool camera_test           = false; // use videotestsrc instead of RTSP (layout testing)
    // The head unit shows only a window of our video frame (Sync 4 centre-crops 1280x720 to its screen aspect, scaled uniformly).
    // 0 = derive from the SDR touchscreen aspect (centre crop); set explicitly to override. Touch coordinates arrive in WINDOW pixels.
    int visible_x = 0, visible_y = 0, visible_w = 0, visible_h = 0;
    int touch_w = 0, touch_h = 0;       // override of the touch coordinate space (0 = same as the visible window)
    bool calib                 = false; // calibration screen: labelled grid + raw touch readout (measures the Sync 4 visible area / touch space)
    bool phone_test            = false; // use videotestsrc as the phone video (layout testing)
    std::string camera_codec   = "mjpeg"; // mjpeg (ESP32-CAM / OV2640 only produces JPEG) | h264
    // USB (UVC) camera: camera_device = its node or a glob, e.g. /dev/v4l/by-id/*-video-index0 (stable name of any USB camera's capture node; /dev/videoN can
    // renumber). A usable USB camera wins over camera_test / camera_rtsp / camera_udp_port; if none is plugged in those are used as before. MJPEG is requested at camera_width x camera_height @ camera_fps (a rear-seat cam does not
    // need 30 fps; 15 halves the JPEG decode cost). The camera is hot-pluggable: plug/unplug at any time. (camera_wait_s is obsolete and ignored.)
    std::string camera_device;
    int camera_fps             = 15;
    // DIAGNOSTIC (default 0 = off): N seconds after the phone starts video, send it ONE UpdateUiConfigRequest (video msg 0x8009) with left/right margins that
    // reshape the UI to ~half the width, then log the reply (0x800A) and what the phone does to its video. Answers "will the Pixel re-layout live?" for split screen.
    int split_probe_s          = 0;
    // STAGED (default 0 = not shown): a "precondition battery for fast charging" toggle on the pill. Nothing is sent to the vehicle yet - the real trigger message has to be
    // captured from Google Maps first. With 1 the button appears and its on/off state is tracked and logged.
    bool precondition_button   = false;
    bool obd_enable            = false;   // OBD-II poller (vLinker FS on USB serial) + the Energy page in the strip (beta)
    std::string obd_port;                 // serial device; empty = first /dev/serial/by-id/*
    int  obd_baud              = 115200;
    bool obd_log_raw           = false;   // log every request/reply (first drive)
    // Truck-silence watchdog: Sync 4 can stop talking while the USB link stays "configured" (truck 29: engine start, 104 s of silence, never recovered). After this many seconds
    // without ANY frame from the truck the session ends and the USB gadget is re-attached (as if the cable were unplugged and replugged). 0 = off.
    int truck_silence_s        = 15;      // after the truck has been heard from (it pings once a second)
    bool pip_corners           = true;    // rounded PiP corners drawn by the overlay (splash/dark corner covers); cheap, replaces the A420 mixer path
    bool relay_audio           = false;   // carry phone media + guidance PCM to the vehicle and the vehicle microphone to the phone (default OFF until proven on the truck)
    bool relay_control         = true;    // carry raw control messages: battery status, call availability, audio focus, voice-session button
    bool nav_require_start     = false;   // navigation relay waits for the vehicle's Start before anything flows (strict); default: tolerant (see NavRelay.h)
    bool split_reshape         = true;    // Split layout: ask the phone to lay its UI out for a 16:9 top pane (UpdateUiConfig margins); no reply in 2 s -> scaled/letterboxed fallback
    int  split_touch_region    = 1;       // touch space the phone gets in the reshaped split: 1 = the pane's own size (the phone's UI region; truck 34: 0 sent y up to 1019 into a 586 px UI = no touch), 0 = the whole window
    int  mic_max_unacked       = 1;       // MicrophoneRequest.max_unacked sent to the vehicle
    bool bt_pair_proxy         = false;   // answer the vehicle's Bluetooth service channel with a Bluetooth address (the dongle if present, else the onboard radio) so it pairs/links a phone for calls (experimental)
    std::string bt_phone_adapter;         // Bluetooth address of the adapter that faces the PHONE (empty = the first non-USB adapter, i.e. the onboard radio)
    std::string bt_vehicle_adapter;       // Bluetooth address of the adapter that faces the VEHICLE (empty = the first USB adapter). With two USB dongles and the onboard
                                          // radio disabled (dtoverlay=disable-bt) both must be set, or the roles are assigned by address order and logged loudly.
    std::string bt_call_model  = "Android Phone";   // AT+CGMM model string the vehicle sees for the bridged "phone" (and manufacturer below)
    std::string bt_call_make   = "Android";
    bool bt_pair_initiate      = false;   // after announcing our address, the Pi itself pairs/connects to the vehicle's Bluetooth address (the car address from its service channel); needs bt_pair_proxy
    bool truck_mirror_phone    = false;   // introduce ourselves to the vehicle exactly like the real phone does (its recorded service-discovery request) and answer the version question as the phone does
    int  truck_ver_minor       = 5;       // protocol minor version we report to the vehicle (the real phone reports 7)
    std::string sdreq_file     = "/etc/aa-bridge/phone_sdreq.bin";   // the real phone's service-discovery request, recorded whenever it connects
    bool bt_release_phone      = false;   // when the Android Auto session is up over Wi-Fi, disconnect the phone's Bluetooth link to us so its hands-free goes to the vehicle (calls, signal, battery)
    int  bt_release_delay_s    = 6;       // wait this long after the session starts before releasing the phone
    bool bt_call_aa            = false;   // phone call audio over Android Auto: the Pi is the phone's hands-free unit, call audio -> the vehicle's GUIDANCE sink, vehicle microphone -> the call (no Bluetooth to the vehicle)
    int  bt_call_aa_gain       = 100;     // downlink gain percent (the phone's call audio is already loud; a soft limiter protects against clipping)
    int  bt_call_aa_mic_gain   = 100;     // microphone gain percent
    bool bt_call_aa_duck       = true;    // lower the microphone while the caller is talking (echo)
    bool bt_call_aa_agc        = true;    // automatic microphone gain (the vehicle microphone is very quiet)
    bool bt_call_bridge        = false;   // relay the phone's Bluetooth hands-free calls to the vehicle through a second (USB dongle) adapter: needs the dongle plugged in
    std::string bt_call_name   = "Bridge Phone";   // how the dongle appears in the vehicle's Bluetooth list
    int  bt_call_discoverable  = -1;      // dongle visible for pairing: -1 = only while it has no paired vehicle, 0 = never, 1 = always
    bool bt_call_sco_route     = true;
    int  bt_call_pkt_vehicle   = -1;     // SCO packet size written to the vehicle radio: -1 = auto (what it sends us; 48 on the USB dongle), 0 = kernel mtu, n = fixed
    int  bt_call_pkt_phone     = -1;     // same toward the phone radio (auto: 60 from the Pixel's eSCO)
    bool split_drag            = false;   // Split: draggable divider (OFF until the drag path is cheap enough: ui46 drive = 80-120 ms per touch, 824 ms overlay refresh)
    int  precond_power_w       = 325000;  // preconditioning request: charger power in the synthetic charging-stop block (observed value from Google Maps)
    int  precond_eta_min       = 15;      // ... ETA = now + this many minutes (seconds-of-day, local)
    int  precond_tz_min        = 0;       // ... local time offset from UTC in minutes for that ETA (the Pi has no time zone data; -300 = CDT)
    int  precond_period_s      = 2;       // ... how often the synthetic state is re-sent while the toggle is on (Maps sends its state about once a second)
    std::string precond_name   = "Battery preconditioning";
    std::string precond_mode   = "replay";
    // replay variants for the bisect (defaults reproduce the ui47a capture byte for byte; ui48a proved that triggers preconditioning):
    bool precond_replay_pos    = true;    // send NavigationCurrentPosition after each state
    bool precond_replay_steps  = true;    // state carries the DEPART step ("toward <road>")
    std::string precond_replay_name;      // destination text shown on the cluster; empty = a neutral default of the captured length
    std::string precond_replay_road;      // road text in the DEPART step; empty = a neutral default of the captured length
    long precond_replay_field1 = -1;      // charging-stop block field 1; -1 = captured 100709 (units unknown)
    bool precond_replay_field3 = true;    // include block field 3 (-1)
    int  precond_replay_plain  = 3;       // how many states without the charging stop go first (Maps sent 4)  // "replay" = resend, byte for byte, what Google Maps sent on the ui47a drive (status, state w/ steps + charging stop, position); "synth" = our own minimal destination (did NOT trigger on ui47a)   // ... the destination text (shows wherever Sync shows the AA destination)
    bool bt_monitor            = false;   // DIAG: run btmon -w /var/log/aa-bridge-btmon.log (full HCI capture of both radios; large during calls)
    std::string bt_call_veh_sco = "connect";   // vehicle SCO: "connect" = the AG (us) opens it when the phone's audio arrives; "wait" = give the vehicle bt_call_veh_sco_wait_ms to open it first
    int  bt_call_veh_sco_wait_ms = 2000;
    std::string bt_phone_profile = "";   // how the Pi connects to the phone: "hfp" (hands-free: calls, caller id, signal, battery), "hsp" (old headset wake-up), "" = hfp when a call path is on, else hsp
    int  bt_hf_keep_s          = 20;      // with a session up and no hands-free link to the phone: (re)connect it this often
    bool focus_reclaim         = true;    // vehicle took video focus (native popup) but keeps sending touches: ask for focus back / resume (fixes the frozen-image case)
    bool bt_call_sim           = false;   // TEST: with no phone, stage a test call toward the vehicle over the dongle (ring, answer, tone, microphone capture)
    int  bt_call_sim_delay_s   = 20;      // seconds after the vehicle hands-free link is up before the test call rings
    int  bt_call_sim_ring_s    = 25;      // ring this long, then answer by ourselves if the vehicle did not
    int  bt_call_sim_len_s     = 45;      // active call length unless the vehicle hangs up first
    std::string bt_io_cap      = "NoInputNoOutput";   // Bluetooth pairing IO capability we claim (Android phones: DisplayYesNo)    // Broadcom adapters: send call audio over USB (HCI) instead of the unused PCM pins
    bool pip_corner_restore    = true;    // rounded PiP corners over a LIVE phone picture show the phone's own pixels (copied in the video path, a few KB per frame) instead of a dark fill
    bool relay_keys            = true;    // steering-wheel / knob media keys from the vehicle -> the phone (the vehicle sent none before: our binding request to it was empty)
    bool relay_sensors         = true;    // forward the truck's speed / gear / parking brake / fuel / odometer / rpm / compass to the phone (it only got driving status + night mode before)
    bool relay_media_status    = true;    // carry the media playback status channel (now-playing title/artist/art for the vehicle's cluster and media screen), raw, logged as [Media]
    bool relay_nav             = true;    // carry the navigation channel (turn-by-turn, EV energy forecast, ...) between phone and vehicle, raw, and log it as [Nav]
    bool idle_skip             = false;   // drop most output frames while nothing on screen changes (A/B on the truck before enabling)
    int idle_hold_ms           = 400;     // stay at full rate this long after any change
    int idle_fps               = 5;       // heartbeat rate while idle
    bool window_relayout       = true;    // tell a live phone session the new layout when the vehicle's window changes (ui51a: AA laid out 1920 wide)
    std::string sensor_cache_path = "/etc/aa-bridge/sensors.conf";  // last sensor list + fuel/EV declaration the vehicle announced; same purpose as the window cache (empty = off)
    std::string window_cache_path = "/etc/aa-bridge/window.conf";   // last window the vehicle announced; used for a phone that connects before the vehicle does (empty = off)
    int cam_crop_align         = 16;      // camera zoom/zone crop: cropped width and height rounded down to a multiple of this (ui49a: an 814/688 px wide crop pushed the mixer's scaler off its fast path -> 5-13 s mixer lag); 2 = old behaviour
    int cam_zoom_lag_reset_ms  = 3000;    // zoom self-heal: mixer lag above this ...
    int cam_zoom_lag_reset_hold_s = 10;   // ... for this long while the camera is zoomed -> the zoom is reset and saved (a later lag-guard restart then starts clean); 0 = off
    int lag_guard_ms           = 3000;    // mixer lag above this for lag_guard_hold_s => restart the service (0 = off)
    int lag_guard_hold_s       = 20;
    int hang_watchdog_s        = 45;      // no encoded frame for this long => stop pinging the systemd watchdog (0 = off)
    bool wifi_watch            = true;    // echo hostapd station events (join/leave/AP up) into the main log
    int truck_silence_early_s  = 5;       // truck heard from but quiet this long while the session is younger than truck_silence_early_window_s (boot race, truck 33): dead already
    int truck_silence_early_window_s = 40;
    int truck_silence_start_s  = 30;      // truck never heard from since the session began
    // The Android Auto splash (phone absent) is the VIDEO base layer under the camera instead of art painted on the overlay with a hole cut for the camera: a hole has to be
    // redrawn whenever the PiP moves and visibly trailed the camera (truck 31). 0 = the old overlay painting.
    bool base_art              = true;
    // Rounded PiP corners: the camera carries an alpha mask and the mixer outputs A420 (needs base_art). 0 = square corners (old video path).
    bool pip_round             = false;   // A420 mixer path: cost ~1 core on the Pi (truck 32). Superseded by pip_corners (overlay); kept only for experiments
    bool truck_silence_exit    = true;    // last resort: exit the process (systemd restarts it) if the stuck reader does not wake 8 s after the truck was declared dead
    int camera_wait_s          = 10;
    int camera_udp_port        = 0;       // >0: receive RTP from udp://:PORT instead of rtspsrc (testing / bench)
    std::string prefs_path     = "/etc/aa-bridge/prefs.conf";
    std::string control_fifo   = "/run/aa-bridge/control";
    std::string status_path    = "/run/aa-bridge/status.json";

    // Video
    int video_width            = 1280;
    int video_height           = 720;
    int video_fps              = 30;
    // Video-path A/B switches (each is a separate change that is unverified on the Pi; flip one at a time to bisect a lag/CPU problem)
    int phone_queue            = 1;    // 1 = newest-frame-wins queue in front of the mixer (int13), 0 = plain queue
    int enc_queue              = 1;    // 1 = short leaky queue between mixer and encoder (mixer timeline never stalls on x264), 0 = none
    int mixer_alpha            = 1;    // 1 = A420 mixer output (UI layers translucent, costs CPU), 0 = I420 (UI layers draw OPAQUE: the old behaviour)
    int ui_blend               = 1;    // 1 = the UI overlay is blended into the frame just before the encoder (cheap); 0 = old: RGBA layers inside the compositor (costs ~140% CPU)
    int log_trace              = 0;    // 1 = log every AAP frame (hex dumps; ~9 KB/s of SD writes). 0 = only a 600-line budget per session start (handshake)
    int log_max_mb             = 20;   // aa-bridge.log is cut (previous half kept as .1) when it exceeds this; 0 = never
    int mix_latency_ms         = 50;   // compositor aggregation wait (lower = less delay, more risk of dropped late frames)
    int video_bitrate          = 4000000;

    static Config load(const std::string& path) {
        Config c;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "[Config] No config file at " << path << ", using defaults" << std::endl;
            return c;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            try {                                           // one bad number must not take the bridge down at boot (every other parser in this tree is guarded)
            if (key == "ap_ssid")           c.ap_ssid = val;
            else if (key == "ap_password")  c.ap_password = val;
            else if (key == "ap_iface")     c.ap_iface = val;
            else if (key == "bt_name")      c.bt_name = val;
            else if (key == "visible_x")    c.visible_x = std::stoi(val);
            else if (key == "visible_y")    c.visible_y = std::stoi(val);
            else if (key == "visible_w")    c.visible_w = std::stoi(val);
            else if (key == "visible_h")    c.visible_h = std::stoi(val);
            else if (key == "touch_w")      c.touch_w = std::stoi(val);
            else if (key == "touch_h")      c.touch_h = std::stoi(val);
            else if (key == "calib")        c.calib = (val == "1" || val == "true");
            else if (key == "bt_bootstrap") c.bt_bootstrap = (val == "1" || val == "true");
            else if (key == "camera_rtsp")  c.camera_rtsp = val;
            else if (key == "camera_overlay_x") c.camera_overlay_x = std::stoi(val);
            else if (key == "camera_overlay_y") c.camera_overlay_y = std::stoi(val);
            else if (key == "camera_overlay_w") c.camera_overlay_w = std::stoi(val);
            else if (key == "camera_overlay_h") c.camera_overlay_h = std::stoi(val);
            else if (key == "video_width")  c.video_width = std::stoi(val);
            else if (key == "video_height") c.video_height = std::stoi(val);
            else if (key == "video_fps")    c.video_fps = std::stoi(val);
            else if (key == "mix_latency_ms") c.mix_latency_ms = std::stoi(val);
            else if (key == "log_trace") c.log_trace = std::stoi(val);
            else if (key == "ui_blend") c.ui_blend = std::stoi(val);
            else if (key == "log_max_mb") c.log_max_mb = std::stoi(val);
            else if (key == "phone_queue") c.phone_queue = std::stoi(val);
            else if (key == "enc_queue") c.enc_queue = std::stoi(val);
            else if (key == "mixer_alpha") c.mixer_alpha = std::stoi(val);
            else if (key == "video_bitrate") c.video_bitrate = std::stoi(val);
            else if (key == "camera_width")  c.camera_width = std::stoi(val);
            else if (key == "camera_height") c.camera_height = std::stoi(val);
            else if (key == "camera_codec")  c.camera_codec = val;
            else if (key == "camera_udp_port") c.camera_udp_port = std::stoi(val);
            else if (key == "camera_device") c.camera_device = val;
            else if (key == "camera_fps") c.camera_fps = std::stoi(val);
            else if (key == "split_probe_s") c.split_probe_s = std::stoi(val);
            else if (key == "precondition_button") c.precondition_button = (val == "1" || val == "true");
            else if (key == "obd_enable") c.obd_enable = (val == "1" || val == "true");
            else if (key == "obd_port") c.obd_port = val;
            else if (key == "obd_baud") c.obd_baud = std::stoi(val);
            else if (key == "obd_log_raw") c.obd_log_raw = (val == "1" || val == "true");
            else if (key == "truck_silence_s") c.truck_silence_s = std::stoi(val);
            else if (key == "pip_corners") c.pip_corners = (val == "1" || val == "true");
            else if (key == "relay_audio") c.relay_audio = (val == "1" || val == "true");
            else if (key == "relay_control") c.relay_control = (val == "1" || val == "true");
            else if (key == "nav_require_start") c.nav_require_start = (val == "1" || val == "true");
            else if (key == "split_reshape") c.split_reshape = (val == "1" || val == "true");
            else if (key == "split_touch_region") c.split_touch_region = std::atoi(val.c_str());
            else if (key == "mic_max_unacked") c.mic_max_unacked = std::atoi(val.c_str());
            else if (key == "bt_pair_proxy") c.bt_pair_proxy = (val == "1" || val == "true");
            else if (key == "bt_pair_initiate") c.bt_pair_initiate = (val == "1" || val == "true");
            else if (key == "bt_call_model") c.bt_call_model = val;
            else if (key == "bt_call_make") c.bt_call_make = val;
            else if (key == "bt_phone_adapter") { c.bt_phone_adapter = val; for (auto& ch : c.bt_phone_adapter) ch = (char)toupper((unsigned char)ch); }
            else if (key == "bt_vehicle_adapter") { c.bt_vehicle_adapter = val; for (auto& ch : c.bt_vehicle_adapter) ch = (char)toupper((unsigned char)ch); }
            else if (key == "truck_mirror_phone") c.truck_mirror_phone = (val == "1" || val == "true");
            else if (key == "truck_ver_minor") c.truck_ver_minor = std::atoi(val.c_str());
            else if (key == "sdreq_file") c.sdreq_file = val;
            else if (key == "bt_release_phone") c.bt_release_phone = (val == "1" || val == "true");
            else if (key == "bt_release_delay_s") c.bt_release_delay_s = std::atoi(val.c_str());
            else if (key == "bt_call_aa") c.bt_call_aa = (val == "1" || val == "true");
            else if (key == "bt_call_aa_gain") c.bt_call_aa_gain = std::atoi(val.c_str());
            else if (key == "bt_call_aa_mic_gain") c.bt_call_aa_mic_gain = std::atoi(val.c_str());
            else if (key == "bt_call_aa_agc") c.bt_call_aa_agc = (val == "1" || val == "true");
            else if (key == "bt_call_aa_duck") c.bt_call_aa_duck = (val == "1" || val == "true");
            else if (key == "bt_call_bridge") c.bt_call_bridge = (val == "1" || val == "true");
            else if (key == "bt_call_name") c.bt_call_name = val;
            else if (key == "bt_call_discoverable") c.bt_call_discoverable = std::atoi(val.c_str());
            else if (key == "bt_phone_profile") c.bt_phone_profile = val;
            else if (key == "bt_hf_keep_s") c.bt_hf_keep_s = std::atoi(val.c_str());
            else if (key == "focus_reclaim") c.focus_reclaim = (val == "1" || val == "true");
            else if (key == "bt_call_sim") c.bt_call_sim = (val == "1" || val == "true");
            else if (key == "bt_call_sim_delay_s") c.bt_call_sim_delay_s = std::atoi(val.c_str());
            else if (key == "bt_call_sim_ring_s") c.bt_call_sim_ring_s = std::atoi(val.c_str());
            else if (key == "bt_call_sim_len_s") c.bt_call_sim_len_s = std::atoi(val.c_str());
            else if (key == "bt_io_cap") c.bt_io_cap = val;
            else if (key == "bt_call_pkt_vehicle") c.bt_call_pkt_vehicle = std::atoi(val.c_str());
            else if (key == "bt_call_pkt_phone") c.bt_call_pkt_phone = std::atoi(val.c_str());
            else if (key == "split_drag") c.split_drag = (val == "1" || val == "true");
            else if (key == "precond_power_w") c.precond_power_w = std::atoi(val.c_str());
            else if (key == "precond_eta_min") c.precond_eta_min = std::atoi(val.c_str());
            else if (key == "precond_tz_min") c.precond_tz_min = std::atoi(val.c_str());
            else if (key == "precond_period_s") c.precond_period_s = std::atoi(val.c_str());
            else if (key == "precond_name") c.precond_name = val;
            else if (key == "precond_mode") c.precond_mode = val;
            else if (key == "precond_replay_pos") c.precond_replay_pos = (val == "1" || val == "true");
            else if (key == "precond_replay_steps") c.precond_replay_steps = (val == "1" || val == "true");
            else if (key == "precond_replay_name") c.precond_replay_name = val;
            else if (key == "precond_replay_road") c.precond_replay_road = val;
            else if (key == "precond_replay_field1") c.precond_replay_field1 = std::atol(val.c_str());
            else if (key == "precond_replay_field3") c.precond_replay_field3 = (val == "1" || val == "true");
            else if (key == "precond_replay_plain") c.precond_replay_plain = std::atoi(val.c_str());
            else if (key == "bt_monitor") c.bt_monitor = (val == "1" || val == "true");
            else if (key == "bt_call_veh_sco") c.bt_call_veh_sco = val;
            else if (key == "bt_call_veh_sco_wait_ms") c.bt_call_veh_sco_wait_ms = std::atoi(val.c_str());
            else if (key == "bt_call_sco_route") c.bt_call_sco_route = (val == "1" || val == "true");
            else if (key == "pip_corner_restore") c.pip_corner_restore = (val == "1" || val == "true");
            else if (key == "relay_keys") c.relay_keys = (val == "1" || val == "true");
            else if (key == "relay_sensors") c.relay_sensors = (val == "1" || val == "true");
            else if (key == "relay_media_status") c.relay_media_status = (val == "1" || val == "true");
            else if (key == "relay_nav") c.relay_nav = (val == "1" || val == "true");
            else if (key == "idle_skip") c.idle_skip = (val == "1" || val == "true");
            else if (key == "idle_hold_ms") c.idle_hold_ms = std::stoi(val);
            else if (key == "idle_fps") c.idle_fps = std::stoi(val);
            else if (key == "window_relayout") c.window_relayout = (val == "1" || val == "true");
            else if (key == "window_cache_path") c.window_cache_path = val;
            else if (key == "sensor_cache_path") c.sensor_cache_path = val;
            else if (key == "cam_crop_align") c.cam_crop_align = std::max(2, std::stoi(val));
            else if (key == "cam_zoom_lag_reset_ms") c.cam_zoom_lag_reset_ms = std::stoi(val);
            else if (key == "cam_zoom_lag_reset_hold_s") c.cam_zoom_lag_reset_hold_s = std::stoi(val);
            else if (key == "lag_guard_ms") c.lag_guard_ms = std::stoi(val);
            else if (key == "lag_guard_hold_s") c.lag_guard_hold_s = std::stoi(val);
            else if (key == "hang_watchdog_s") c.hang_watchdog_s = std::stoi(val);
            else if (key == "wifi_watch") c.wifi_watch = (val == "1" || val == "true");
            else if (key == "truck_silence_early_s") c.truck_silence_early_s = std::stoi(val);
            else if (key == "truck_silence_early_window_s") c.truck_silence_early_window_s = std::stoi(val);
            else if (key == "truck_silence_start_s") c.truck_silence_start_s = std::stoi(val);
            else if (key == "base_art") c.base_art = (val == "1" || val == "true");
            else if (key == "pip_round") c.pip_round = (val == "1" || val == "true");
            else if (key == "truck_silence_exit") c.truck_silence_exit = (val == "1" || val == "true");
            else if (key == "camera_wait_s") c.camera_wait_s = std::stoi(val);
            else if (key == "camera_test")   c.camera_test = (val == "1" || val == "true");
            else if (key == "phone_test")    c.phone_test = (val == "1" || val == "true");
            else if (key == "prefs_path")    c.prefs_path = val;
            else if (key == "usb_two_stage") c.usb_two_stage = (val == "1" || val == "true");
            else if (key == "md_cert")      c.md_cert = val;
            else if (key == "hu_cert")      c.hu_cert = val;
            else if (key == "hu_key")       c.hu_key = val;
            else if (key == "md_key")       c.md_key = val;
            else if (key == "md_dhparam")   c.md_dhparam = val;
            else if (key == "md_ca")        c.md_ca = val;
            } catch (const std::exception& e) { std::cerr << "[Config] ignoring '" << key << "=" << val << "': " << e.what() << " (default kept)" << std::endl; }
        }
        return c;
    }
};
