#pragma once
// The live sync check (dual-sensor design §7.6): cam1's `preview_frame` timestamps against the primary's own, frames paired
// by gridPoint() slot. Pure: std only, host-tested (skew_test.cpp). ControlClient feeds it; the state builders read it.
//
// A pair is one slot with a frame from each Core in the last second. The skew is cam1's grid error minus cam0's, so it is
// their timestamp difference when both sit in the same slot. `locked` needs both Cores locked, equal fps and a median skew
// within kLockedBoundUs; with too few pairs, or at two fps, it is false and the skew is unknown (null in the state).
#include "../camera/FramePhaseLock.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace apertar::skew {

inline constexpr int64_t kWindowMs = 1000;        // "median of the last second"
inline constexpr size_t kMinPairs = 3;            // fewer pairs say nothing
inline constexpr double kLockedBoundUs = 1000.0;  // the lock's own "more than 1 ms off" threshold (FramePhaseLock)
inline constexpr size_t kKeep = 256;

struct Stamp {
    int64_t timestampUs = 0;   // the frame's SensorTimestamp
    int64_t atMs = 0;          // when the relay (or the preview publisher) saw it, steady clock
};

struct Result {
    bool known = false;        // enough pairs to say
    bool locked = false;
    double skewUs = 0;         // median of cam1 - cam0
    size_t pairs = 0;
};

class Meter {
public:
    void addCam1(Stamp stamp) { push(cam1_, stamp); }
    void clear() { cam1_.clear(); }

    // `primary` holds the primary's recent frames (oldest first or not: the pairing is by slot). `rateNum/rateDen` is the
    // shared frame rate (recordingFrameRate of the primary's fps). `equalFps`: both Cores run one rate (tied fps, or equal
    // by coincidence). `bothLocked`: each Core's phase lock reports locked.
    Result measure(const std::vector<Stamp> &primary, int64_t nowMs, int64_t rateNum, int64_t rateDen, bool equalFps, bool bothLocked) const
    {
        Result result;
        if (!equalFps || rateNum <= 0 || rateDen <= 0) return result;
        std::map<int64_t, int64_t> primaryError;   // slot -> grid error in ns
        for (const Stamp &stamp : primary) {
            if (nowMs - stamp.atMs > kWindowMs) continue;
            const auto point = FramePhaseLock::gridPoint(stamp.timestampUs * 1000, rateNum, rateDen);
            primaryError[point.slot] = point.errorNs;
        }
        std::vector<double> diffsUs;
        for (const Stamp &stamp : cam1_) {
            if (nowMs - stamp.atMs > kWindowMs) continue;
            const auto point = FramePhaseLock::gridPoint(stamp.timestampUs * 1000, rateNum, rateDen);
            const auto other = primaryError.find(point.slot);
            if (other != primaryError.end()) diffsUs.push_back(static_cast<double>(point.errorNs - other->second) / 1000.0);
        }
        result.pairs = diffsUs.size();
        if (diffsUs.size() < kMinPairs) return result;
        std::sort(diffsUs.begin(), diffsUs.end());
        const size_t mid = diffsUs.size() / 2;
        result.skewUs = diffsUs.size() % 2 ? diffsUs[mid] : (diffsUs[mid - 1] + diffsUs[mid]) / 2.0;
        result.known = true;
        result.locked = bothLocked && std::abs(result.skewUs) <= kLockedBoundUs;
        return result;
    }

private:
    static void push(std::deque<Stamp> &ring, Stamp stamp)
    {
        ring.push_back(stamp);
        while (ring.size() > kKeep) ring.pop_front();
    }
    std::deque<Stamp> cam1_;
};

} // namespace apertar::skew
