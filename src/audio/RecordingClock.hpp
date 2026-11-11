#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace apertar {

inline std::pair<int,int> recordingFrameRate(double fps)
{
    for (int numerator : {24000,30000,60000,120000})
        if (std::abs(fps - double(numerator) / 1001) < 0.001)
            return {numerator,1001};
    return {int(std::llround(std::max(1.0, fps) * 1000)),1000};
}

inline double recordingFps(double fps)
{
    const auto rate = recordingFrameRate(fps);
    return double(rate.first) / rate.second;
}

inline int64_t clockMicroseconds(clockid_t clock)
{
    timespec time{};
    return ::clock_gettime(clock, &time) == 0
        ? int64_t(time.tv_sec) * 1000000 + time.tv_nsec / 1000 : 0;
}

// A take that starts on a slot (design §6.2, §6.3) stamps each frame with its slot's time plus the primary's
// tc_offset_us instead of the frame's own timestamp, so two Cores give the same timecode for the same slot and
// jitter can never repeat or skip a number. periodNum == 0 means no slots: the timecode of a take without them.
// The grid is k * den / num seconds (num / den = recordingFrameRate(fps)), as in util/TakeSlots.hpp.
struct TimecodeSlot {
    int64_t offsetUs = 0;
    int64_t periodNum = 0;
    int64_t periodDen = 0;
    bool enabled() const { return periodNum > 0 && periodDen > 0; }
};

// The slot nearest bootUs and its time, in microseconds (the tie to the earlier slot, as FramePhaseLock::gridPoint).
inline int64_t slotTimeOfBootUs(int64_t bootUs, int64_t periodNum, int64_t periodDen)
{
    const __int128 period = static_cast<__int128>(1000000) * periodDen;
    const __int128 scaled = static_cast<__int128>(bootUs) * periodNum;
    __int128 slot = scaled / period;
    __int128 rest = scaled % period;
    if (rest < 0) { rest += period; --slot; }
    if (2 * rest > period) ++slot;
    const __int128 numerator = slot * period;
    __int128 q = numerator / periodNum;
    const __int128 r = numerator % periodNum;
    if (2 * r >= periodNum) ++q;
    return static_cast<int64_t>(q);
}

class RecordingClock {
public:
    RecordingClock()
    {
        const auto before = clockMicroseconds(CLOCK_BOOTTIME);
        const auto wall = clockMicroseconds(CLOCK_REALTIME);
        offsetUs_ = wall - (before + clockMicroseconds(CLOCK_BOOTTIME)) / 2;
    }
    explicit RecordingClock(const TimecodeSlot &slot) : RecordingClock()
    {
        if (slot.enabled()) {
            slot_ = slot;
            offsetUs_ = slot.offsetUs;
        }
    }
    uint64_t wallTime(int64_t bootUs) const
    {
        const int64_t base = slot_.enabled() ? slotTimeOfBootUs(bootUs, slot_.periodNum, slot_.periodDen) : bootUs;
        return uint64_t(std::max<int64_t>(0, base + offsetUs_));
    }
private:
    int64_t offsetUs_ = 0;
    TimecodeSlot slot_;
};

inline std::string recordingTimecode(bool freeRun, uint64_t epochUs, double fps)
{
    const int nominal = std::max(1, int(std::llround(fps)));
    if (!freeRun) return "00:00:00:00";
    const time_t seconds = time_t(epochUs / 1000000);
    tm local{};
    localtime_r(&seconds, &local);
    const int frame = std::min(nominal - 1, int((epochUs % 1000000) * fps / 1000000));
    char value[32];
    std::snprintf(value, sizeof(value), "%02d:%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec, frame);
    return value;
}

} // namespace apertar
