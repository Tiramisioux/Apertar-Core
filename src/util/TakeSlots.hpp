#pragma once

// Slot arithmetic and slots.txt for a take that starts and stops on the grid (dual-sensor design §6.2, §7.2).
// Pure: std only, host-tested (take_slots_test.cpp).
//
// The grid is k * T on CLOCK_MONOTONIC, T = den / num seconds with num / den = recordingFrameRate(fps) (25000/1000,
// 24000/1001). A frame's slot is the grid point nearest its SensorTimestamp, a tie to the earlier one, which is what
// FramePhaseLock::gridPoint answers. A take's boundaries sit mid-slot, start_us = (k0 - 1/2) * T and
// stop_us = (k1 - 1/2) * T, so the first frame is slot k0 and the last is k1 - 1. A boundary itself is read with the
// tie going up (tieUp), so a hand-made value on the exact half still means k0 and k1.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace apertar::takeslots {

struct Rate {
    int64_t num = 0;   // frames per second = num / den
    int64_t den = 0;
    bool valid() const { return num > 0 && den > 0; }
};

// The slot nearest tsNs (floor of x + 1/2, the tie earlier, or later with tieUp).
inline int64_t slotOfNs(int64_t tsNs, const Rate &rate, bool tieUp = false)
{
    if (!rate.valid())
        return 0;
    const __int128 period = static_cast<__int128>(1000000000) * rate.den;
    const __int128 scaled = static_cast<__int128>(tsNs) * rate.num;
    __int128 below = scaled / period;
    __int128 rest = scaled % period;
    if (rest < 0) {
        rest += period;
        --below;
    }
    if (2 * rest > period || (tieUp && 2 * rest == period))
        ++below;
    return static_cast<int64_t>(below);
}

inline int64_t slotOfUs(int64_t tsUs, const Rate &rate, bool tieUp = false)
{
    return slotOfNs(tsUs * 1000, rate, tieUp);
}

// The time of slot k on the grid, in microseconds, rounded to the nearest. 0 without a rate.
inline int64_t slotTimeUs(int64_t slot, const Rate &rate)
{
    if (!rate.valid())
        return 0;
    const __int128 numerator = static_cast<__int128>(slot) * 1000000 * rate.den;
    __int128 q = numerator / rate.num;
    __int128 r = numerator % rate.num;
    if (r < 0) {
        r += rate.num;
        --q;
    }
    if (2 * r >= rate.num)
        ++q;
    return static_cast<int64_t>(q);
}

// The mid-slot boundary before slot k: (k - 1/2) * T, in microseconds, rounded to the nearest.
inline int64_t boundaryUs(int64_t slot, const Rate &rate)
{
    if (!rate.valid())
        return 0;
    const __int128 numerator = (static_cast<__int128>(2) * slot - 1) * 1000000 * rate.den;
    const __int128 denominator = static_cast<__int128>(2) * rate.num;
    __int128 q = numerator / denominator;
    __int128 r = numerator % denominator;
    if (r < 0) {
        r += denominator;
        --q;
    }
    if (2 * r >= denominator)
        ++q;
    return static_cast<int64_t>(q);
}

// ceil(timeUs / T): the first slot at or after a time.
inline int64_t ceilSlotUs(int64_t timeUs, const Rate &rate)
{
    if (!rate.valid())
        return 0;
    const __int128 scaled = static_cast<__int128>(timeUs) * rate.num;
    const __int128 period = static_cast<__int128>(1000000) * rate.den;
    __int128 q = scaled / period;
    __int128 r = scaled % period;
    if (r < 0) {   // C++ truncates toward zero: q is already the ceiling of a negative quotient
        r = 0;
    }
    if (r > 0)
        ++q;
    return static_cast<int64_t>(q);
}

// The slot a take with this start_us should open on, ceil(start_us / T), and the one a take with this stop_us should end
// on, ceil(stop_us / T) - 1 (design 6.2). Not the nearest slot: a boundary at a fractional period (26 fps: T = 38461.5 us)
// is rounded to a whole us and can sit a hair below the half, which "nearest" reads as the slot before.
inline int64_t expectedFirstSlot(int64_t startUs, const Rate &rate) { return ceilSlotUs(startUs, rate); }
inline int64_t expectedLastSlot(int64_t stopUs, const Rate &rate) { return ceilSlotUs(stopUs, rate) - 1; }

// Step 1 of a take (design 6.2): k0 = ceil((now + lead) / T), start_us = (k0 - 1/2) * T. The primary's pick; T64
// sends it to cam1 as well.
inline int64_t startUsFor(int64_t nowUs, int64_t leadUs, const Rate &rate)
{
    return boundaryUs(ceilSlotUs(nowUs + leadUs, rate), rate);
}

// A stop with a lead: k1 = ceil((now + lead) / T), stop_us = (k1 - 1/2) * T. A lead of 0 gives the first boundary
// after now, the next slot's stop_us that a safety stop sends cam1 (design 6.2).
inline int64_t stopUsFor(int64_t nowUs, int64_t leadUs, const Rate &rate)
{
    int64_t k = ceilSlotUs(nowUs + leadUs, rate);
    while (boundaryUs(k, rate) <= nowUs)
        ++k;
    return boundaryUs(k, rate);
}

// Two frame rates at once make a two-camera take NOT SYNCED from its first frame (design 6.2, Q14).
inline bool fpsDiffer(double a, double b) { return std::fabs(a - b) > 0.0005; }

// Whole frames a slot lies after the expected one (0 when on time or early).
inline int64_t framesLate(int64_t actualSlot, int64_t expectedSlot)
{
    return actualSlot > expectedSlot ? actualSlot - expectedSlot : 0;
}

// What a take with a start_us leaves in its clip folder as slots.txt, and in get_state as recording_first_slot and
// recording_last_slot. Slots are unset until the first frame is recorded and the gate closes.
struct SlotLog {
    Rate rate;
    int64_t startUs = 0;   // 0 = the take had no start_us
    int64_t stopUs = 0;    // 0 = no stop_us
    std::optional<int64_t> firstSlot;
    std::optional<int64_t> lastSlot;
    std::optional<int64_t> stoppedLateFrames;   // set when a stop_us was already past
    // NOT SYNCED (§6.2, §7.6): why, and from which slot. No slot means from the first frame.
    bool unsynced = false;
    std::string unsyncedReason;
    std::optional<int64_t> unsyncedFromSlot;
};

inline std::string formatSlotsTxt(const SlotLog &log)
{
    std::string out;
    out += "# Apertar slot log. A slot is the frame whose SensorTimestamp is nearest k * period.\n";
    out += "# period = den / num seconds; the grid starts at 0 on the time base.\n";
    out += "time_base=CLOCK_MONOTONIC\n";
    out += "period=" + std::to_string(log.rate.num) + "/" + std::to_string(log.rate.den) + "\n";
    if (log.startUs > 0)
        out += "start_us=" + std::to_string(log.startUs) + "\n";
    if (log.stopUs > 0)
        out += "stop_us=" + std::to_string(log.stopUs) + "\n";
    if (log.firstSlot)
        out += "first_slot=" + std::to_string(*log.firstSlot) + "\n";
    if (log.lastSlot)
        out += "last_slot=" + std::to_string(*log.lastSlot) + "\n";
    if (log.firstSlot && log.lastSlot && *log.lastSlot >= *log.firstSlot)
        out += "slots=" + std::to_string(*log.lastSlot - *log.firstSlot + 1) + "\n";
    if (log.firstSlot && log.startUs > 0 && log.rate.valid()) {
        const int64_t late = framesLate(*log.firstSlot, expectedFirstSlot(log.startUs, log.rate));
        if (late > 0)
            out += "started_late_frames=" + std::to_string(late) + "\n";
    }
    if (log.stoppedLateFrames && *log.stoppedLateFrames > 0)
        out += "stopped_late_frames=" + std::to_string(*log.stoppedLateFrames) + "\n";
    if (log.unsynced) {
        out += "status=NOT SYNCED";
        if (!log.unsyncedReason.empty())
            out += " (" + log.unsyncedReason + ")";
        out += "\n";
        out += log.unsyncedFromSlot ? "unsynced_from_slot=" + std::to_string(*log.unsyncedFromSlot) + "\n"
                                    : std::string("unsynced_from=first frame\n");
    } else {
        out += "status=synced\n";
    }
    return out;
}

// The journal's wording for a stop that came after its stop_us.
inline std::string stoppedLateLine(int64_t frames)
{
    return "stopped " + std::to_string(frames) + (frames == 1 ? " frame late" : " frames late");
}

} // namespace apertar::takeslots
