#pragma once

// Apertar playback, design sketch v2 (not in any repository).
// The hold rule as one pure function, and the yield window that stops a decode in flight without
// any lock (critic M8).
//
// Why a pure function: isRecording(), state() and mediaWritesPending() each take
// CameraController::mutex_ (CameraController.cpp:9844-9848, :9868-9872, :9908-9912), and
// stopRecording() holds that mutex across the whole writer drain (:9785-9798). buildWebStateJson
// already reads those values once (ApertarCoreApp.cpp:7859-7866); it passes them in here instead of
// asking the camera a second time. The routes fill the same struct with one read each.
//
// Why a yield window: the decode between chunks must not take the camera's mutex (it is held while
// a take starts, :9508-9545, so the check would block exactly when it matters). handleCommand sets
// the window at its top, before any camera call, for record_start, capture_photo, timelapse_start,
// run_media_speed_test, enter_standby, eject_media and format_media; the media watcher sets it when
// media goes away. A decode asks only `yield.active(now)`, an atomic load, so it stops before the
// take's first frame is written.
// The command opens it for 3 s: by then the real signal (recording_ = true) is visible, or the command
// failed and playback may go on. While any hold lasts, the media watcher (every 250 ms in a take, 1 s
// otherwise, ApertarCoreApp.cpp:9383) extends it by 1.5 s, so a long-running stream that checks only
// the atomic also stops for a take that began while its client was stalled. It closes at most 1.5 s
// after the hold ends. A clear-on-next-request flag (the critic's sketch) would let a request that
// arrives between the command's start and recording_ = true clear it and read during the take.

#include <atomic>
#include <cstdint>

namespace apertar::playback {

enum class Hold { None = 0, Recording, Writing, MediaTest, Standby, Media };

inline const char *holdName(Hold h)
{
    switch (h) {
    case Hold::Recording: return "recording";
    case Hold::Writing: return "writing";
    case Hold::MediaTest: return "media_test";
    case Hold::Standby: return "standby";
    case Hold::Media: return "media";
    case Hold::None: break;
    }
    return "";
}

struct HoldSignals {
    bool recording = false;          // camera_.isRecording()
    bool soundRecording = false;     // soundRecordingActive()
    bool timelapse = false;          // camera_.timelapseActive()
    bool writesPending = false;      // mediaWriteStatus().pending (stills, the stop drain, H.264 finalise)
    bool speedTest = false;          // mediaSpeedTestRunning_
    bool standby = false;            // state() == CameraState::Standby
    bool mediaUsable = true;         // mediaSnapshot().mounted() || mediaGateBypassed()
    Hold yieldReason = Hold::None;   // PlaybackYield::reasonIfActive(now)
};

// Order matters: the first match names the hold the viewer shows.
inline Hold playbackHoldFor(const HoldSignals &s)
{
    if (s.recording || s.soundRecording || s.timelapse) return Hold::Recording;
    if (s.yieldReason != Hold::None) return s.yieldReason;
    if (s.writesPending) return Hold::Writing;
    if (s.speedTest) return Hold::MediaTest;
    if (s.standby) return Hold::Standby;
    if (!s.mediaUsable) return Hold::Media;
    return Hold::None;
}

class PlaybackYield {
public:
    // Opens or extends the window; never shortens it.
    void yieldFor(Hold reason, int64_t nowMs, int64_t windowMs = 3000)
    {
        reason_.store(int(reason), std::memory_order_relaxed);
        const int64_t until = nowMs + windowMs;
        int64_t current = untilMs_.load(std::memory_order_relaxed);
        while (current < until && !untilMs_.compare_exchange_weak(current, until, std::memory_order_release))
            ;
    }
    bool active(int64_t nowMs) const { return nowMs < untilMs_.load(std::memory_order_acquire); }
    Hold reasonIfActive(int64_t nowMs) const
    {
        return active(nowMs) ? Hold(reason_.load(std::memory_order_relaxed)) : Hold::None;
    }

private:
    std::atomic<int64_t> untilMs_{0};
    std::atomic<int> reason_{0};
};

} // namespace apertar::playback
