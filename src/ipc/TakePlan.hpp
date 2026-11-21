#pragma once
// Which cameras a take records, and the lead of its slot (dual-sensor design §6.1, §6.2). Pure: std only, host-tested
// (take_plan_test.cpp).
//
// §6.1: the first match wins: `cameras` on record_start, then `recordCameras`, then (Follow preview) the views. A cam1 that
// is down is dropped and cam0 rolls, except an explicit `cameras=cam1`, which is refused. A cam1 that is writing, busy past
// the wait, recording alone or stalled refuses a take that wants it.
#include "ipc/Cam1State.hpp"
#include "ipc/Cam1Texts.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace apertar::takeplan {

inline constexpr int kCam0 = 1;
inline constexpr int kCam1 = 2;
inline constexpr int kBoth = 3;

// "cam0", "cam1", "both"; "none" for no camera.
inline const char *word(int mask)
{
    return mask == kBoth ? "both" : mask == kCam1 ? "cam1" : mask == kCam0 ? "cam0" : "none";
}

inline constexpr const char *kCamerasInvalid = "cameras must be cam0, cam1 or both.";
inline constexpr const char *kSoundRecorderNoCamera = "Sound Recorder mode records no camera.";
inline constexpr const char *kWritingRefusal = "Wait for CAM1 to finish writing to media.";
inline constexpr const char *kNoPictureRefusal = "CAM1 has no picture.";

// 0 for a value that is none of the three.
inline int maskOfCameras(std::string_view value)
{
    return value == "cam0" ? kCam0 : value == "cam1" ? kCam1 : value == "both" ? kBoth : 0;
}

// cam1 cannot take part: the words of §4.5 that are not ready, busy, writing or recording.
inline bool cam1Down(std::string_view word)
{
    return word == "not_found" || word == "offline" || word == "refused" || word == "lost" || word == "standby";
}

// One text per state word (§8.1, record_start).
inline std::string downRefusal(std::string_view word)
{
    if (word == "not_found") return "CAM1 is not found.";
    if (word == "offline") return "CAM1 is offline.";
    if (word == "refused") return "CAM1 refused the settings.";
    if (word == "lost") return "CAM1 link is lost.";
    return "CAM1 is in standby.";
}

struct PressInputs {
    std::string explicitCameras;      // the record_start field, "" when absent
    std::string recordCameras = "Both";
    std::string previewSource = "Both";
    std::string hdmi1Source = "Cam 1";
    std::string cam1Word = "offline"; // the state word
    bool busyDropped = false;         // cam1 has been busy for kBusyDropMs
    bool stalled = false;             // ready, but its stream stopped (§6.1)
};

struct Decision {
    bool refused = false;
    std::string refusal;
    int cameras = kCam0;
    bool droppedCam1 = false;         // the take wanted cam1 and rolls without it
};

// The cameras the policy asks for, whatever cam1's state (rules 1 to 3 of §6.1). In Follow preview, a down cam1 turns
// its sources into Cam 0.
inline int wanted(const PressInputs &in)
{
    if (const int mask = maskOfCameras(in.explicitCameras))
        return mask;
    const bool down = cam1Down(in.cam1Word);
    const auto preview = [&](const std::string &label) { return down ? kCam0 : cam1state::detail::previewMask(label); };
    if (in.recordCameras == "Cam 0") return kCam0;
    if (in.recordCameras == "Cam 1") return kCam1;
    if (in.recordCameras == "Follow preview") return preview(in.previewSource) | preview(in.hdmi1Source);
    return kBoth;
}

inline Decision decide(const PressInputs &in)
{
    Decision out;
    const int mask = wanted(in);
    out.cameras = mask;
    if (!(mask & kCam1)) {
        out.cameras = kCam0;
        return out;
    }
    const std::string_view cam1 = in.cam1Word;
    const auto refuse = [&](std::string text) {
        out.refused = true;
        out.refusal = std::move(text);
        return out;
    };
    const auto dropCam1 = [&]() {
        out.cameras = kCam0;
        out.droppedCam1 = true;
        return out;
    };
    if (cam1 == "ready") {
        if (in.stalled)
            return refuse(kNoPictureRefusal);
        return out;
    }
    if (cam1 == "busy") {
        if (in.busyDropped)   // 10 s of busy: cam0 rolls alone; a press that asks for cam1 alone is refused (§4.3)
            return mask == kCam1 ? refuse(cam1state::kBusyRefusal) : dropCam1();
        return refuse(cam1state::kBusyRefusal);
    }
    if (cam1 == "writing")
        return refuse(kWritingRefusal);
    if (cam1 == "recording")
        return refuse(cam1state::kCam1RecordingRefusal);
    // Down: cam0 rolls, except for an explicit `cameras=cam1`.
    if (in.explicitCameras == "cam1")
        return refuse(downRefusal(cam1));
    return dropCam1();
}

// ---- the slot of a take (§6.2 step 1) -------------------------------------------------------------------------------------

inline constexpr int kLockDelayFrames = 12;        // D in Clear HDR (§7.4), the worst of the measured cases
inline constexpr int64_t kStartFloorUs = 500000;   // max(500 ms, ...)
inline constexpr int64_t kLStartUs = 300000;       // L_start: T56 0.7 measured 0.281-0.302 s on the primary
inline constexpr int64_t kLCam1EstimateUs = 80000; // L_cam1, an ESTIMATE of 10-80 ms until T69; the relay logs the real one

// max(500 ms, L_start + T, (D + 2) T + L_cam1) with the larger of the two periods. The lock term only with the lock on.
inline int64_t leadUs(bool lockOn, int64_t periodUs)
{
    int64_t lead = std::max(kStartFloorUs, kLStartUs + periodUs);
    if (lockOn)
        lead = std::max(lead, (kLockDelayFrames + 2) * periodUs + kLCam1EstimateUs);
    return lead;
}

// The stop lead is 0.225 s + 3T (about 0.35 s at 25 fps); the relay's write wait is that minus 2T.
inline int64_t stopLeadUs(int64_t periodUs) { return 225000 + 3 * periodUs; }
inline int stopWaitMs(int64_t periodUs) { return static_cast<int>((stopLeadUs(periodUs) - 2 * periodUs) / 1000); }

} // namespace apertar::takeplan
