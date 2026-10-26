#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace apertar {

// Closed-loop frame-rate phase lock.
//
// The sensor's frame period is an integer number of lines of its own crystal,
// so a nominal 24.000 fps is never exact and a long take drifts against the
// Pi's clock, which is where the audio samples are stamped. This loop measures
// the accumulated phase error of the frame arrivals against "origin + k /
// fps" on the reference clock and nudges the requested frame duration by whole
// microseconds. The sensor can only realise whole lines, so the integer
// requests alternate between two adjacent line counts: the average cadence
// becomes exact (a first-order sigma-delta), the instantaneous error stays
// within the deadband.
//
// Pure state machine: no I/O. Call step() once per completed frame with the
// frame's SensorTimestamp (CLOCK_BOOTTIME ns in this stack). Everything is in
// the reference clock's units; only differences are used, so the epoch does
// not matter.
// Snapshot for state/telemetry consumers (controller -> App -> /api/state).
struct PhaseLockStatus {
    bool active = false;
    double errorUs = 0.0;      // last phase error the servo saw
    int64_t durationUs = 0;    // duration currently pinned, 0 when none
    uint64_t pushes = 0;       // FrameDurationLimits changes since the last reset
    bool saturated = false;    // the IPA echoed a different duration than the one pinned
};

struct FramePhaseLock {
    struct Tuning {
        double proportional = 0.06;  // µs of duration per µs of phase error
        double integral = 0.0015;    // µs of duration per µs·frame of accumulated error
        double deadbandUs = 6.0;     // hold the current request while |error| is below this
        double clampUs = 150.0;      // never request more than this away from nominal
    };

    struct Decision {
        bool active = false;         // the lock is tracking
        bool rearmed = false;        // this frame set a new datum, nothing requested
        bool servoRan = false;       // error and duration below are valid
        bool pushDuration = false;   // the integer request changed: send FrameDurationLimits {d, d}
        int64_t durationUs = 0;
        double phaseErrorUs = 0.0;   // > 0: frames arrive late (sensor slow), < 0: early
    };

    Tuning tuning{};

    bool active = false;
    double targetFps = 0.0;
    double nominalUs = 0.0;          // 1e6 / targetFps, fractional
    double requestedUs = 0.0;        // current fractional request
    double integralUs = 0.0;
    int64_t originNs = 0;            // datum: reference time of frame 0
    int64_t previousNs = 0;          // last reference time, for gap detection
    uint64_t frames = 0;             // frames since the datum, missed ones included
    int64_t lastPushedUs = -1;

    // Telemetry since the last reset.
    double maxAbsErrorUs = 0.0;
    uint64_t pushes = 0;

    void release()
    {
        active = false;
        previousNs = 0;
    }

    void resetTelemetry()
    {
        maxAbsErrorUs = 0.0;
        pushes = 0;
    }

    Decision step(bool enabled, bool recording, double fps, int64_t referenceNs)
    {
        Decision decision;
        if (!enabled || fps <= 1.0) {
            release();
            return decision;
        }

        const double periodNs = 1e9 / fps;
        const int64_t sincePrevious = (active && previousNs != 0) ? referenceNs - previousNs : 0;
        previousNs = referenceNs;
        const bool bigGap = active && sincePrevious > static_cast<int64_t>(1.5 * periodNs);

        // A new datum: first frame, an fps change, or (in preview only) a gap from a
        // stall or reconfigure. While recording a gap is a real drop: keep the datum
        // and count the missing frames so the phase does not jump.
        if (!active || std::fabs(fps - targetFps) > 1e-6 || (bigGap && !recording)) {
            arm(fps, referenceNs);
            decision.active = true;
            decision.rearmed = true;
            return decision;
        }
        if (bigGap) {
            const uint64_t slots = static_cast<uint64_t>((static_cast<double>(sincePrevious) + 0.5 * periodNs) / periodNs);
            if (slots > 1)
                frames += slots - 1;
        }
        ++frames;

        const double errorUs = (static_cast<double>(referenceNs - originNs) - static_cast<double>(frames) * periodNs) * 1e-3;

        // Preview only: a leak larger than half a frame would take tens of seconds to
        // bleed off through the clamp; start again instead. Recording tracks it.
        if (!recording && std::fabs(errorUs) > 0.5e6 / fps) {
            arm(fps, referenceNs);
            decision.active = true;
            decision.rearmed = true;
            return decision;
        }

        if (std::fabs(errorUs) > tuning.deadbandUs) {
            const double windup = tuning.clampUs / std::max(tuning.integral, 1e-9);
            integralUs = std::clamp(integralUs + errorUs, -windup, windup);
            requestedUs = nominalUs - (tuning.proportional * errorUs + tuning.integral * integralUs);
            requestedUs = std::clamp(requestedUs, nominalUs - tuning.clampUs, nominalUs + tuning.clampUs);
        }
        maxAbsErrorUs = std::max(maxAbsErrorUs, std::fabs(errorUs));

        // Whole microseconds: the integer rounding is the dither, and it keeps control
        // traffic down to the frames where the request really changes.
        const int64_t durationUs = std::llround(requestedUs);
        decision.active = true;
        decision.servoRan = true;
        decision.phaseErrorUs = errorUs;
        decision.durationUs = durationUs;
        if (durationUs != lastPushedUs) {
            lastPushedUs = durationUs;
            ++pushes;
            decision.pushDuration = true;
        }
        return decision;
    }

private:
    void arm(double fps, int64_t referenceNs)
    {
        targetFps = fps;
        nominalUs = 1e6 / fps;
        requestedUs = nominalUs;
        integralUs = 0.0;
        originNs = referenceNs;
        frames = 0;
        lastPushedUs = -1;
        active = true;
    }
};

} // namespace apertar
