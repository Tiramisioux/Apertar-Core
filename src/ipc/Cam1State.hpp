#pragma once
// cam1 as the primary shows it (dual-sensor design §4.3, §4.5, §6.1). Pure: std only, host-tested (cam1_state_test.cpp).
//
// The state word's ranking, the configured test (amendment A1), the busy window's verdict, the `cam1` object and the
// top-level `record_cameras`. ControlClient keeps the inputs; ApertarCoreApp writes the result into both state bodies.
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace apertar::cam1state {

inline constexpr int kBusyWaitMs = 1000;    // a record_start waits this long for cam1's reply (§4.3)
inline constexpr int kBusyDropMs = 10000;   // after this long busy, a record_start rolls without cam1

// ---- configured (§4.1, amendment A1) ------------------------------------------------------------------------------

struct ConfiguredInputs {
    bool switchOn = false;          // UI.conf dualSensorEnabled
    bool configNamesBoth = false;   // config.txt names a sensor on cam0 and on cam1
    bool socketAccepted = false;    // cam1's socket has accepted a connection since the switch went on (sticky)
    bool socketIsOurs = false;      // the socket path is our own socket (same inode): refused, but it counts
};

inline bool configured(const ConfiguredInputs &in)
{
    return in.switchOn && (in.configNamesBoth || in.socketAccepted || in.socketIsOurs);
}

// ---- the state word (§4.5) ----------------------------------------------------------------------------------------

struct RankInputs {
    bool linked = false;
    bool lost = false;          // cam1 left a running take unasked, or the link dropped during one
    bool sensorMissing = false; // config.txt names cam1, sysfs has none, and there is no link
    bool recording = false;     // cam1's take state
    bool writing = false;       // writes pending, or a slow poll while cam1's stop is unanswered
    bool refused = false;
    bool standby = false;
    int pending = 0;            // setters queued or in flight
};

// lost > recording > writing > refused > busy > ready. The link decides first: with none, `offline` or `not_found`
// (except `lost`). `standby` sits after `refused` and before `busy`: a standby cam1 being woken still reads standby
// until its get_state says otherwise, while the count gates record_start on its own (§4.5, "Two inputs, one word").
inline const char *word(const RankInputs &in)
{
    if (in.lost) return "lost";
    if (!in.linked) return in.sensorMissing ? "not_found" : "offline";
    if (in.recording) return "recording";
    if (in.writing) return "writing";
    if (in.refused) return "refused";
    if (in.standby) return "standby";
    if (in.pending > 0) return "busy";
    return "ready";
}

// ---- the busy window (§4.3) ---------------------------------------------------------------------------------------

enum class BusyVerdict {
    Go,         // not busy
    Wait,       // wait up to kBusyWaitMs for cam1's reply, then ask again
    Refuse,     // still busy after the wait: "CAM1 is applying a change. One moment."
    DropCam1,   // busy for kBusyDropMs: the take rolls without cam1 (a cam1-only press is refused instead)
};

inline BusyVerdict busyVerdict(int pending, int64_t busyForMs, bool alreadyWaited)
{
    if (pending <= 0) return BusyVerdict::Go;
    if (busyForMs >= kBusyDropMs) return BusyVerdict::DropCam1;
    return alreadyWaited ? BusyVerdict::Refuse : BusyVerdict::Wait;
}

inline constexpr const char *kBusyRefusal = "CAM1 is applying a change. One moment.";
inline constexpr const char *kCam1RecordingRefusal = "CAM1 is recording.";

// ---- record_cameras (§6.1) ------------------------------------------------------------------------------------------

struct RecordCamerasInputs {
    std::string recordCameras = "Both";       // UI.conf recordCameras: Both, Cam 0, Cam 1, Follow preview
    std::string previewSource = "Both";       // HDMI 0's view
    std::string hdmi1Source = "Cam 1";        // HDMI 1's view
    bool soundRecorder = false;               // Sound Recorder mode records no camera
    bool cam0Recording = false;               // part 1: a running take is cam0's alone (T62/T64 publish the take's cameras)
    std::string cam1State = "offline";        // the state word
    bool busyDropped = false;                 // cam1 has been busy for kBusyDropMs
};

namespace detail {
// Which cameras a preview label asks for: bit 0 = cam0, bit 1 = cam1.
inline int previewMask(std::string_view label)
{
    if (label == "Cam 0" || label == "PiP Cam 0") return 1;
    if (label == "Cam 1" || label == "PiP Cam 1") return 2;
    return 3;   // Both, Both (Cam 1 left)
}
} // namespace detail

inline const char *recordCamerasWord(const RecordCamerasInputs &in)
{
    if (in.soundRecorder) return "none";
    if (in.cam0Recording) return "cam0";
    // A ready cam1 is in; a busy one stays in until the drop. Anything else is down for this purpose (§6.1).
    const bool cam1In = in.cam1State == "ready" || (in.cam1State == "busy" && !in.busyDropped);
    int wanted;
    if (in.recordCameras == "Cam 0") wanted = 1;
    else if (in.recordCameras == "Cam 1") wanted = 2;
    else if (in.recordCameras == "Follow preview") {
        // While cam1 is down its sources count as Cam 0.
        wanted = (cam1In ? detail::previewMask(in.previewSource) : 1) | (cam1In ? detail::previewMask(in.hdmi1Source) : 1);
    } else wanted = 3;
    if (!cam1In) return "cam0";
    if (wanted == 3) return "both";
    return wanted == 2 ? "cam1" : "cam0";
}

// ---- the cam1 object (§4.5, §8.1) -----------------------------------------------------------------------------------

struct Cam1View {
    std::string state = "offline";
    std::string message;
    long long droppedFrames = 0;
    bool locked = false;
    double skewUs = 0;
    bool skewKnown = false;        // enough frame pairs to say; skew_us reads null otherwise (§7.6)
    std::string settings = "{}";   // cam1's settingsBody from its last get_state, as an object
    std::string sensor;
    bool sensorMono = false;
};

namespace detail {
inline std::string escape(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}
} // namespace detail

// "cam1":{...} without a leading comma.
inline std::string objectJson(const Cam1View &view)
{
    return "\"cam1\":{\"state\":\"" + detail::escape(view.state) + "\",\"message\":\"" + detail::escape(view.message) +
           "\",\"recording_dropped_frames\":" + std::to_string(view.droppedFrames) +
           ",\"locked\":" + (view.locked ? "true" : "false") +
           ",\"skew_us\":" + (view.skewKnown ? std::to_string(static_cast<long long>(std::llround(view.skewUs))) : std::string("null")) +
           ",\"settings\":" + (view.settings.empty() ? "{}" : view.settings) +
           ",\"sensor\":\"" + detail::escape(view.sensor) + "\",\"sensor_mono\":" + (view.sensorMono ? "true" : "false") + "}";
}

} // namespace apertar::cam1state
