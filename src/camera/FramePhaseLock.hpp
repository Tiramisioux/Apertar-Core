#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace apertar {

// Closed-loop frame-rate phase lock on one absolute grid (design §7.2-§7.4).
//
// The sensor's frame period is a whole number of lines of its own crystal, so a
// nominal 25 fps is never exact and a long take drifts against the Pi's clock,
// which stamps the audio and the SensorTimestamp alike. The lock measures each
// frame's SensorTimestamp against the grid k * T, where T is the exact rational
// period of recordingFrameRate(fps) (num/den frames per second) and the grid's
// origin is the clock's zero. Two Cores at the same fps therefore lock to the
// same grid, and a Core that arms again (or restarts) rejoins it by itself.
//
// The actuator: the controller pins FrameDurationLimits {d, d+1}. min != max
// takes libcamera's default blanking (HBLANK 0, the mode's minimum line), which
// floors the frame to whole lines, and the IMX585 driver floors VMAX to even.
// So a frame lasts V * H, V a multiple of vmaxStep and H the line time. Q
// carries the remainder from frame to frame, so the mean period is the request
// exactly, and pins d = ceil((V + 1/2) * H), so the floor cannot give V - 1.
//
// The loop: the shipped PI (kp, ki, deadband, clamp) with conditional
// integration. In preview a lock that is not locked and is more than 1 ms off
// instead lengthens one request so that the frame after it starts on a slot,
// then holds the PI until the stretched frame has run plus 2 frames. A stretch
// in flight is tracked whatever the lock's state: a release or a re-arm leaves
// its request queued, and a take's first-frame gate waits for it all the same.
// A take in which the lock decides its phase more than 1 ms off the grid keeps
// that phase: the PI holds the offset, so the frames stay at T on the Pi's
// clock, which the sound follows, until the take ends. A lock that was locked waits a frame for it, so one
// stray timestamp keeps nothing. Three stretches in a row that are never seen end the stretches until the next
// arm: the PI then pulls in alone.
//
// Pure state machine: no I/O. Call step() once per completed frame that has a
// SensorTimestamp (CLOCK_BOOTTIME ns in this stack), with the lock on or off.

// GCC and Clang have it; __extension__ keeps -Wpedantic quiet.
__extension__ typedef __int128 PhaseLockInt128;

// Why the lock armed. The controller names the cause in release(); the lock
// itself sees only a period change while it is armed.
enum class PhaseLockArmReason { None, Enable, FpsChange, Stream, PhotoModeExit };

// The trace's arm=<name>.
inline const char *phaseLockArmReasonName(PhaseLockArmReason reason)
{
    switch (reason) {
    case PhaseLockArmReason::Enable: return "enable";
    case PhaseLockArmReason::FpsChange: return "fps";
    case PhaseLockArmReason::Stream: return "stream";
    case PhaseLockArmReason::PhotoModeExit: return "photo";
    case PhaseLockArmReason::None: break;
    }
    return "none";
}

// Snapshot for state/telemetry consumers (controller -> App -> get_state, /api/state).
struct PhaseLockStatus {
    bool active = false;
    double errorUs = 0.0;      // the last frame's error against the grid, lock on or off (0 before any)
    int64_t durationUs = 0;    // the lock pin in force, 0 when none
    uint64_t pushes = 0;       // lock pin changes since the last reset
    bool saturated = false;    // the IPA echoed a duration that was never pinned
    bool locked = false;       // |error| <= 100 µs for 25 frames in a row
};

struct FramePhaseLock {
    struct Tuning {
        double proportional = 0.06;  // µs of duration per µs of phase error
        double integral = 0.0015;    // µs of duration per µs·frame of accumulated error
        double deadbandUs = 6.0;     // hold the current request while |error| is below this
        double clampUs = 150.0;      // never request more than this away from nominal
    };

    // The design's thresholds (§7.4, D7, D9). Not settings.
    static constexpr double kStretchAboveUs = 1000.0;  // stretch, or keep a take's phase, only when
                                                       //   |error| > max(this, deadband)
    static constexpr double kLockedWithinUs = 100.0;   // locked: |error| <= this ...
    static constexpr int kLockedAfterFrames = 25;      // ... for this many frames in a row
    static constexpr int kHoldAfterLanding = 2;        // frames held after the stretched frame
    static constexpr int kHoldBoundOverRequests = 6;   // the hold ends N + 6 frames after the stretch at most
    static constexpr int kStopStretchesAfterNotSeen = 3;  // stretches not seen in a row that end the stretches
    // The landing test's tolerance without H: the lock then pins whole µs, which the sensor floors to lines of a
    // length Core does not know. More than one VMAX step of every mode this stack drives (IMX585 2 x 8.9 µs at
    // 720 MHz, IMX283 1 x 12.3 µs), and far less than a stretch stands out from the frames around it.
    static constexpr int64_t kLandingToleranceNoLineNs = 50000;

    // The grid point nearest a timestamp.
    struct GridPoint {
        int64_t slot = 0;     // k = round(ts * num / (1e9 * den))
        int64_t errorNs = 0;  // ts - k * 1e9 * den / num, in (-T/2, T/2], rounded toward zero to whole ns
    };

    // One completed frame, everything the controller knows about it.
    struct Input {
        bool enabled = false;          // fps_phase_lock on, photo mode off, fps > 1, past the fps-change grace,
                                       //   and no Speed Ramp take has changed its fps
        bool recording = false;        // takePending_ || recordingFramesEnabled_: no stretch starts, and a take
                                       //   keeps its own phase
        bool stretchAllowed = true;    // false while the saturated flag is latched or a storage pre-roll pin is set
        bool prerollPinned = false;    // a storage pre-roll pin owns every request's period: its take keeps no phase
        int64_t rateNum = 0;           // the period, exactly: recordingFrameRate(fps) = rateNum / rateDen
        int64_t rateDen = 1;           //   frames per second (25 -> 25000/1000, 23.976 -> 24000/1001)
        int64_t timestampNs = 0;       // the frame's SensorTimestamp
        double lineUs = 0.0;           // H = (mode width + HBLANK) / PIXEL_RATE; 0 = unknown: whole-µs pins, no Q
        int vmaxStep = 1;              // the VMAX quantum in lines: IMX585 2, others 1
        int64_t inFlightExcessNs = 0;  // what the frames already decided add: sum of (their period - T)
        // This frame is the stretched one: from the echo of the request that carried the stretch on, the first
        // frame whose FrameDuration is the stretch's period within landingToleranceNs().
        bool stretchLanded = false;
        int requestCount = 0;          // N = requests_.size(); bounds the stretch hold to N + 6 frames
    };

    struct Decision {
        bool active = false;           // the lock is tracking (enabled and armed)
        bool rearmed = false;          // this frame armed the lock; everything below is valid on it too
        PhaseLockArmReason armReason = PhaseLockArmReason::None;  // why, when rearmed
        int64_t slot = 0;              // this frame's grid slot, lock on or off (0 without a period)
        int64_t errorNs = 0;           // this frame against its slot: > 0 after it, < 0 before it
        double phaseErrorUs = 0.0;     // errorNs in µs
        bool locked = false;           // |error| <= 100 µs for 25 frames in a row, and no take phase of its own
        bool servoRan = false;         // the PI stepped on this frame (not a stretch or hold frame)
        // A stretch is in flight, whatever the lock's state: from the frame that requests it until the stretched
        // frame plus 2 frames, at most N + 6 frames. The PI is held (Q still dithers T), and a take's first-frame
        // gate must not open on this frame.
        bool holding = false;
        // The lock pin changed: send FrameDurationLimits {durationUs, durationUs + 1} on every request
        // from the next one on.
        bool pushDuration = false;
        int64_t durationUs = 0;        // the lock pin in force, 0 when inactive
        int64_t durationNs = 0;        // the period that pin gives: V * H (durationUs * 1000 without H)
        // Lengthen the next request only: FrameDurationLimits {stretchUs, stretchUs + 1}, then the lock
        // pin again. The controller remembers stretchUs to see its echo, and stretchNs to find the stretched frame.
        bool stretch = false;
        int64_t stretchUs = 0;
        int64_t stretchNs = 0;         // the period the stretched frame gets
        bool stretchLanded = false;    // this frame is the stretched one (Input::stretchLanded), within the hold
        bool stretchNotSeen = false;   // N + 6 frames passed without it: the hold ends after this frame
        bool stretchesStopped = false; // this was the third not seen in a row: no stretch until the next arm
        // This frame decided a take's phase more than 1 ms off the grid: from here to the take's end the PI holds
        // that error (takeOffsetNs), so the frames run at T, and the take is not synced.
        bool takePhaseKept = false;
    };

    // A pin from Q's rule.
    struct Pin {
        int64_t us = 0;        // FrameDurationLimits min; max = us + 1
        int64_t lines = 0;     // V, 0 without H
        int64_t periodNs = 0;  // V * H, or us * 1000 without H
    };

    Tuning tuning{};

    bool active = false;
    bool locked = false;
    int64_t rateNum = 0;             // the period armed on
    int64_t rateDen = 1;
    double nominalUs = 0.0;          // T in µs: 1e6 * den / num
    double requestedUs = 0.0;        // the PI's request, before Q
    double integralUs = 0.0;
    double carryUs = 0.0;            // Q: requested minus realised so far, added to the next request
    int64_t lastPushedUs = -1;
    uint64_t frames = 0;             // frames since the arm, the arm frame being 1
    int lockedFrames = 0;            // frames in a row with |error| <= 100 µs
    // The stretch in flight, kept through release(), arm() and disabled frames; cancelStretch() ends it.
    int stretchAge = -1;             // frames since the stretch was requested, -1: none in flight
    int stretchLandedAge = -1;       // stretchAge on the stretched frame, -1: not yet
    // A take's own phase: decided on the take's first frame outside a hold and again on every arm in it, held by
    // the PI until the take ends.
    bool takeRecording = false;      // Input::recording on the last frame
    bool takePhaseDue = false;       // the next frame outside a hold decides the take's phase
    int64_t takeOffsetNs = 0;        // the error the PI holds in this take, 0: none (the take is on the grid)
    int takeStraySign = 0;           // a locked lock's first frame past the threshold: its sign, 0: none waiting
    PhaseLockArmReason nextArmReason = PhaseLockArmReason::Stream;  // what the next arm reports
    // Stretches not seen in a row, kept through release() and cancelStretch(); a landed stretch and arm() clear it.
    int stretchesNotSeen = 0;
    bool stretchesStopped = false;   // no stretch is decided until the next arm

    // Telemetry since the last reset.
    double maxAbsErrorUs = 0.0;      // largest |error| on a frame the PI ran
    uint64_t pushes = 0;

    // Stop tracking. The next enabled frame arms again and reports `next` as the reason.
    void release(PhaseLockArmReason next = PhaseLockArmReason::Enable)
    {
        deactivate();
        nextArmReason = next;
    }

    void resetTelemetry()
    {
        maxAbsErrorUs = 0.0;
        pushes = 0;
    }

    // The stretch in flight is over without its frame: it never went out (its request could not carry it, or the
    // pre-roll pin took its place), or its request is gone with the stream. Nothing holds the PI or a take's gate
    // for it, and no "not seen" follows.
    void cancelStretch()
    {
        stretchAge = -1;
        stretchLandedAge = -1;
    }

    // The stretched frame's FrameDuration is the stretch's period within this (ns): half a line, or
    // kLandingToleranceNoLineNs without H.
    static int64_t landingToleranceNs(double lineUs)
    {
        return lineUs > 0.0 ? std::llround(lineUs * 500.0) : kLandingToleranceNoLineNs;
    }

    // {0, 0} without a period.
    static GridPoint gridPoint(int64_t timestampNs, int64_t rateNum, int64_t rateDen)
    {
        GridPoint point;
        if (rateNum <= 0 || rateDen <= 0)
            return point;
        PhaseLockInt128 remainder = 0;
        divide(timestampNs, rateNum, rateDen, point.slot, remainder);
        point.errorNs = static_cast<int64_t>(remainder / rateNum);
        return point;
    }

    // Q's rule for one frame of about durationUs, without the carry: V = vmaxStep *
    // round(durationUs / (vmaxStep * H)) lines, pinned as ceil((V + 1/2) * H) µs. Without H, whole µs.
    static Pin pinFor(double durationUs, double lineUs, int vmaxStep)
    {
        Pin pin;
        if (!(lineUs > 0.0)) {
            pin.us = std::llround(durationUs);
            pin.periodNs = pin.us * 1000;
            return pin;
        }
        const int64_t step = std::max(1, vmaxStep);
        pin.lines = step * std::llround(durationUs / (static_cast<double>(step) * lineUs));
        pin.us = static_cast<int64_t>(std::ceil((static_cast<double>(pin.lines) + 0.5) * lineUs));
        pin.periodNs = std::llround(static_cast<double>(pin.lines) * lineUs * 1000.0);
        return pin;
    }

    // Q (§7.3): the pin for this frame's request plus the carried remainder, so the
    // mean period is the request exactly.
    Pin quantise(double requestUs, double lineUs, int vmaxStep)
    {
        if (!(lineUs > 0.0)) {
            carryUs = 0.0;
            return pinFor(requestUs, lineUs, vmaxStep);
        }
        const double q = requestUs + carryUs;
        const Pin pin = pinFor(q, lineUs, vmaxStep);
        carryUs = q - static_cast<double>(pin.lines) * lineUs;
        return pin;
    }

    Decision step(const Input &in)
    {
        Decision decision;
        const bool periodKnown = in.rateNum > 0 && in.rateDen > 0;
        PhaseLockInt128 remainder = 0;  // the exact error, in ns * rateNum
        if (periodKnown) {
            divide(in.timestampNs, in.rateNum, in.rateDen, decision.slot, remainder);
            decision.errorNs = static_cast<int64_t>(remainder / in.rateNum);
            decision.phaseErrorUs = static_cast<double>(decision.errorNs) * 1e-3;
        }
        // A take's phase is decided on its first frame (below, outside a hold) and lasts until the take ends.
        if (!in.recording) {
            takeOffsetNs = 0;
            takeStraySign = 0;
        }
        takePhaseDue = in.recording && (takePhaseDue || !takeRecording);
        takeRecording = in.recording;
        // The stretch in flight, lock on or off.
        if (stretchAge >= 0) {
            const int bound = std::max(0, in.requestCount) + kHoldBoundOverRequests;
            ++stretchAge;
            if (in.stretchLanded && stretchLandedAge < 0) {
                stretchLandedAge = stretchAge;
                decision.stretchLanded = true;
                stretchesNotSeen = 0;
            }
            const int lastHeld = stretchLandedAge >= 0 ? std::min(stretchLandedAge + kHoldAfterLanding, bound) : bound;
            if (stretchAge <= lastHeld) {
                decision.holding = true;
                decision.stretchNotSeen = stretchLandedAge < 0 && stretchAge == bound;
                if (decision.stretchNotSeen && ++stretchesNotSeen >= kStopStretchesAfterNotSeen && !stretchesStopped) {
                    stretchesStopped = true;
                    decision.stretchesStopped = true;
                }
            } else {
                cancelStretch();
            }
        }
        if (!in.enabled || !periodKnown) {
            deactivate();
            return decision;
        }
        if (!active || in.rateNum != rateNum || in.rateDen != rateDen) {
            decision.armReason = active ? PhaseLockArmReason::FpsChange : nextArmReason;
            arm(in.rateNum, in.rateDen);
            decision.rearmed = true;
            takePhaseDue = takePhaseDue || in.recording;  // every arm in a take decides its phase again
        }
        ++frames;
        decision.active = true;

        const double errorUs = decision.phaseErrorUs;
        const double absErrorUs = std::fabs(errorUs);
        const double stretchAboveUs = std::max(kStretchAboveUs, tuning.deadbandUs);
        const bool wasLocked = locked;
        lockedFrames = absErrorUs <= kLockedWithinUs ? std::min(lockedFrames + 1, kLockedAfterFrames) : 0;
        locked = takeOffsetNs == 0 && lockedFrames >= kLockedAfterFrames;
        decision.locked = locked;

        if (decision.holding) {
            emit(quantise(requestedUs, in.lineUs, in.vmaxStep), decision);
            return decision;
        }

        // A take's own phase (in a take only the PI acts): a take whose lock arms, or that starts, more than 1 ms
        // off the grid would otherwise get up to T/2 of PI pull-in, frames up to the clamp off T and, past the
        // overshoot, a phantom hole in its time index. Holding that error instead keeps the frames at T on the
        // Pi's clock, which the sound follows. The storage pre-roll's take owns no period of its own. A lock that
        // was locked before this frame waits a frame, as the stretch below does: one stray timestamp must not keep
        // a phase for the whole take, so it takes two frames in a row past the threshold on the same side.
        if (takePhaseDue) {
            const bool past = !in.prerollPinned && absErrorUs > stretchAboveUs;
            const int sign = decision.errorNs > 0 ? 1 : -1;
            if (past && (wasLocked || takeStraySign != 0) && takeStraySign != sign) {
                takeStraySign = sign;  // decides again on the next frame
            } else {
                takePhaseDue = false;
                takeStraySign = 0;
            }
            if (!takePhaseDue && past) {
                takeOffsetNs = decision.errorNs;
                requestedUs = nominalUs;
                integralUs = 0.0;
                carryUs = 0.0;
                lockedFrames = 0;
                locked = false;
                decision.locked = false;
                decision.takePhaseKept = true;
            }
        }

        // Acquisition (preview only): lengthen one request so the frame after it starts on a
        // slot, counting what the requests already sent will add: ext = (-e - excess) mod T.
        // A lock that was locked before this frame waits a frame, so one stray timestamp does
        // not move it.
        if (!wasLocked && !in.recording && in.stretchAllowed && !stretchesStopped && absErrorUs > stretchAboveUs) {
            const PhaseLockInt128 period = static_cast<PhaseLockInt128>(1000000000) * in.rateDen;  // T, in ns * rateNum
            PhaseLockInt128 extra = (-remainder - static_cast<PhaseLockInt128>(in.inFlightExcessNs) * in.rateNum) % period;
            if (extra < 0)
                extra += period;
            // The stretched frame must stand out from every frame around it, or its echo and its landing could be
            // read off another one: the lock's own pins stay within the clamp of T plus Q's step. A shorter
            // extension takes one more period (one frame of about 2T).
            const double standOutNs =
                (tuning.clampUs + 2.0 * std::max(1, in.vmaxStep) * std::max(0.0, in.lineUs)) * 1000.0 +
                2.0 * static_cast<double>(landingToleranceNs(in.lineUs));
            if (static_cast<double>(static_cast<int64_t>(extra)) <= standOutNs * static_cast<double>(in.rateNum))
                extra += period;
            const double stretchedUs = static_cast<double>(static_cast<int64_t>(period + extra)) /
                (1000.0 * static_cast<double>(in.rateNum));
            const Pin stretched = pinFor(stretchedUs, in.lineUs, in.vmaxStep);
            decision.stretch = true;
            decision.stretchUs = stretched.us;
            decision.stretchNs = stretched.periodNs;
            decision.holding = true;
            stretchAge = 0;
            stretchLandedAge = -1;
            // The frames after the stretched one run at T: the PI starts again from nothing and Q
            // from a zero carry. The requests after it need the lock pin again, so send Q's first
            // pin now without spending it (the next frame's Q gives the same pin).
            requestedUs = nominalUs;
            integralUs = 0.0;
            carryUs = 0.0;
            emit(pinFor(requestedUs, in.lineUs, in.vmaxStep), decision);
            return decision;
        }

        // The PI acts on the error against the take's own phase while it has one, wrapped into (-T/2, T/2].
        double servoErrorUs = errorUs - static_cast<double>(takeOffsetNs) * 1e-3;
        if (servoErrorUs > nominalUs / 2.0)
            servoErrorUs -= nominalUs;
        else if (servoErrorUs <= -nominalUs / 2.0)
            servoErrorUs += nominalUs;
        if (std::fabs(servoErrorUs) > tuning.deadbandUs) {
            // Conditional integration: integrate only while the unclamped output stays inside the
            // clamp, or when this error moves it back in (integrating e moves the output by -ki * e).
            const double low = nominalUs - tuning.clampUs;
            const double high = nominalUs + tuning.clampUs;
            const double candidate = integralUs + servoErrorUs;
            const double unclamped = nominalUs - (tuning.proportional * servoErrorUs + tuning.integral * candidate);
            if ((unclamped >= low && unclamped <= high) || (unclamped > high && servoErrorUs > 0.0) ||
                (unclamped < low && servoErrorUs < 0.0))
                integralUs = candidate;
            requestedUs = std::clamp(nominalUs - (tuning.proportional * servoErrorUs + tuning.integral * integralUs),
                                     low, high);
        }
        maxAbsErrorUs = std::max(maxAbsErrorUs, absErrorUs);
        decision.servoRan = true;
        emit(quantise(requestedUs, in.lineUs, in.vmaxStep), decision);
        return decision;
    }

private:
    // ts * num = slot * 1e9 * den + remainder, with remainder in (-1e9 * den / 2, 1e9 * den / 2]:
    // the nearest slot, a tie going to the earlier one.
    static void divide(int64_t timestampNs, int64_t rateNum, int64_t rateDen, int64_t &slot, PhaseLockInt128 &remainder)
    {
        const PhaseLockInt128 period = static_cast<PhaseLockInt128>(1000000000) * rateDen;
        const PhaseLockInt128 scaled = static_cast<PhaseLockInt128>(timestampNs) * rateNum;
        PhaseLockInt128 below = scaled / period;
        PhaseLockInt128 rest = scaled % period;
        if (rest < 0) {
            rest += period;
            --below;
        }
        if (2 * rest > period) {
            rest -= period;
            ++below;
        }
        slot = static_cast<int64_t>(below);
        remainder = rest;
    }

    void emit(const Pin &pin, Decision &decision)
    {
        decision.durationUs = pin.us;
        decision.durationNs = pin.periodNs;
        if (pin.us != lastPushedUs) {
            lastPushedUs = pin.us;
            ++pushes;
            decision.pushDuration = true;
        }
    }

    // arm() resets the PI, Q, `locked` and a take's phase, never the grid (it has no origin of its own) nor a
    // stretch in flight (its request is still queued).
    void arm(int64_t num, int64_t den)
    {
        rateNum = num;
        rateDen = den;
        nominalUs = 1e6 * static_cast<double>(den) / static_cast<double>(num);
        requestedUs = nominalUs;
        integralUs = 0.0;
        carryUs = 0.0;
        lastPushedUs = -1;
        frames = 0;
        lockedFrames = 0;
        locked = false;
        takeOffsetNs = 0;
        takeStraySign = 0;
        stretchesNotSeen = 0;
        stretchesStopped = false;
        nextArmReason = PhaseLockArmReason::Enable;
        active = true;
    }

    // Not tracking; the reason for the next arm stays as release() left it, and a stretch in flight stays tracked.
    void deactivate()
    {
        active = false;
        locked = false;
        lockedFrames = 0;
    }
};

} // namespace apertar
