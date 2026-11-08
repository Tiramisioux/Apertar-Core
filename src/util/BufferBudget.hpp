#pragma once

// The byte budget of the write queue's "ram" mode (APERTAR_CDNG_BUFFER=ram). Pure arithmetic, std only, so
// tools/buffer_budget_test.cpp runs on any host.
//
// The mechanism and the numbers (a RAM pool sized from MemAvailable at take start, kept clear of the
// system's own needs) follow cinepi-raw's DNG encoder (cinepi/dng_encoder.cpp: RAM_FRACTION, ram_cv_.wait).
// Here the pool is the queue of already-copied frames; a full queue still drops, it never blocks capture.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

#include "cdng/FramePath.hpp"

namespace apertar {

struct BufferPlanInputs {
    uint64_t memAvailableBytes = 0;
    uint64_t memTotalBytes = 0;
    uint64_t frameBytes = 0;         // one queued entry of this take (the copy, in its container size)
    uint64_t framesModeFrames = 0;   // maxQueuedFrames_ that "frames" mode would use for this take
    double fraction = 0.6;           // APERTAR_CDNG_QUEUE_RAM_FRACTION
    uint64_t floorMb = 512;          // APERTAR_CDNG_QUEUE_RAM_FLOOR_MB, kept free
    double maxFraction = 0.5;        // APERTAR_CDNG_QUEUE_RAM_MAX_FRACTION of MemTotal
};

struct BufferPlan {
    bool ram = false;
    uint64_t budgetBytes = 0;
    // ram: the text after "ApertarCore buffer: "; refused: why the plan is "frames".
    std::string reason;
};

namespace bufferbudget {

constexpr uint64_t kMiB = 1024ull * 1024ull;

// Out-of-range or unparsable values fall back to a sane number; the clamp keeps a typo from taking all RAM.
inline double clampFraction(double value, double lo, double hi, double fallback)
{
    if (!std::isfinite(value))
        return fallback;
    return std::min(hi, std::max(lo, value));
}

inline double parseDouble(const char *text, double fallback)
{
    if (!text || !*text)
        return fallback;
    char *end = nullptr;
    const double parsed = std::strtod(text, &end);
    return (end && *end == '\0' && std::isfinite(parsed)) ? parsed : fallback;
}

inline uint64_t saturatingMul(uint64_t a, uint64_t b)
{
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        return std::numeric_limits<uint64_t>::max();
    return a * b;
}

// uint64 -> double -> uint64 without ever exceeding the input.
inline uint64_t scaleBytes(uint64_t bytes, double factor)
{
    const double scaled = static_cast<double>(bytes) * factor;
    if (!(scaled > 0.0))
        return 0;
    return std::min<uint64_t>(bytes, static_cast<uint64_t>(scaled));
}

// "Key:   123456 kB" from /proc/meminfo (or another file in that format), in bytes; 0 when missing.
inline uint64_t readMemInfoBytes(const char *key, const char *path = "/proc/meminfo")
{
    std::ifstream in(path);
    const std::string prefix = std::string(key) + ":";
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        const unsigned long long kb = std::strtoull(line.c_str() + prefix.size(), nullptr, 10);
        return saturatingMul(kb, 1024ull);
    }
    return 0;
}

// The [storage] keys of apertar-hardware.conf that set the same four things as the APERTAR_CDNG_* variables.
// A missing or bad key is "not set" (the default, or the variable, decides). The environment wins over the file.
struct BufferConf {
    std::optional<bool> ram;            // buffer=frames|ram
    std::optional<double> fraction;     // buffer_ram_fraction     0.05..0.9
    std::optional<unsigned> floorMb;    // buffer_ram_floor_mb     0..1048576
    std::optional<double> maxFraction;  // buffer_ram_max_fraction 0.1..0.75
    std::optional<unsigned> stopPercent;  // buffer_stop_percent    0 (never) or 50..99
    std::optional<unsigned> framesPerFolder;  // frames_per_folder 0 (one folder) or 100..100000 (framepath::)
};

// Why `value` is not a good value of a buffer key, or "" when it is (or when the key is not one of the five).
inline std::string bufferConfProblem(const std::string &key, const std::string &value)
{
    char *end = nullptr;
    auto number = [&](double lo, double hi) {
        const double parsed = std::strtod(value.c_str(), &end);
        return !value.empty() && *end == '\0' && std::isfinite(parsed) && parsed >= lo && parsed <= hi;
    };
    if (key == "buffer")
        return (value == "frames" || value == "ram") ? "" : "is not frames or ram";
    if (key == "buffer_ram_fraction")
        return number(0.05, 0.9) ? "" : "is not a number from 0.05 to 0.9";
    if (key == "buffer_ram_max_fraction")
        return number(0.1, 0.75) ? "" : "is not a number from 0.1 to 0.75";
    if (key == "buffer_stop_percent") {
        const bool digits = !value.empty() && value.size() <= 3 && value.find_first_not_of("0123456789") == std::string::npos;
        const unsigned long percent = digits ? std::strtoul(value.c_str(), nullptr, 10) : 0;
        return (digits && (percent == 0 || (percent >= 50 && percent <= 99))) ? "" : "is not 0 (never) or a whole number from 50 to 99";
    }
    if (key == "frames_per_folder")
        return framepath::framesPerFolderProblem(value);
    if (key == "buffer_ram_floor_mb") {
        const bool digits = !value.empty() && value.size() <= 7 && value.find_first_not_of("0123456789") == std::string::npos;
        return (digits && std::strtoul(value.c_str(), nullptr, 10) <= (1u << 20)) ? "" : "is not a whole number of MiB from 0 to 1048576";
    }
    return "";
}

// The [storage] section of the file's text. The last good line of a key counts; a value with a trailing
// comment is ignored (Core's Python reader keeps the comment in the value, and the validator says so).
inline BufferConf parseBufferConf(const std::string &text)
{
    BufferConf conf;
    bool inStorage = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const auto trim = [](std::string s) {
            const char *ws = " \t\r\f\v";
            const auto first = s.find_first_not_of(ws);
            if (first == std::string::npos)
                return std::string();
            return s.substr(first, s.find_last_not_of(ws) - first + 1);
        };
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            std::string name = trim(line.substr(1, line.size() - 2));
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            inStorage = name == "storage";
            continue;
        }
        const auto eq = line.find('=');
        if (!inStorage || eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        for (char &c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string value = trim(line.substr(eq + 1));
        if (value.find_first_of("#;") != std::string::npos || !bufferConfProblem(key, value).empty())
            continue;
        if (key == "buffer")
            conf.ram = value == "ram";
        else if (key == "buffer_ram_fraction")
            conf.fraction = std::strtod(value.c_str(), nullptr);
        else if (key == "buffer_ram_max_fraction")
            conf.maxFraction = std::strtod(value.c_str(), nullptr);
        else if (key == "buffer_stop_percent")
            conf.stopPercent = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "buffer_ram_floor_mb")
            conf.floorMb = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "frames_per_folder")
            conf.framesPerFolder = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
    }
    return conf;
}

// The safety stop: the take ends by itself (a clean stop, every queued frame is still written) when the active
// queue is this percent full. APERTAR_CDNG_BUFFER_STOP_PERCENT (same rules as the key; a bad value counts as unset)
// wins over [storage] buffer_stop_percent; absent both: 90 in ram mode, 0 (off, the original behaviour) in frames mode.
constexpr unsigned kDefaultRamStopPercent = 90;

inline std::optional<unsigned> parseStopPercent(const char *text)
{
    if (!text || !*text || !bufferConfProblem("buffer_stop_percent", text).empty())
        return std::nullopt;
    return static_cast<unsigned>(std::strtoul(text, nullptr, 10));
}
inline unsigned effectiveStopPercent(bool modeIsRam, const std::optional<unsigned> &confValue, const std::optional<unsigned> &envValue)
{
    if (envValue)
        return *envValue;
    if (confValue)
        return *confValue;
    return modeIsRam ? kDefaultRamStopPercent : 0u;
}
// used/capacity in the unit of the active mode; percent 0 = off. Integer maths, no rounding surprises.
inline bool safetyStopReached(uint64_t used, uint64_t capacity, unsigned percent)
{
    return percent > 0 && capacity > 0 && used * 100 >= capacity * percent;   // bytes <= 2^40: no overflow
}

// The safety stop's live state, read by the lock-free buffer-events thread and the state bodies: plain atomics.
// stopPercent = the threshold of the active/last take (0 = off); active = from the safety stop until the queue
// has drained to 0 or the next take starts.
struct SafetyStopState {
    std::atomic<unsigned> stopPercent{0};
    std::atomic<bool> active{false};
    // Frames of the previous clip thrown away because record was pressed during the safety-stop flush
    // (see abortSafetyStopFlush). 0 normally; reset at the next safety stop.
    std::atomic<uint64_t> flushDiscarded{0};
};
inline SafetyStopState &safetyStopState()
{
    static SafetyStopState state;
    return state;
}
// The running write queue registers here (under flushAbortMutex) while it can drain; the command path calls it
// without any controller lock. Returns the number of queued frames it discarded (frames already inside a writer
// still finish), or -1 when no queue is draining after a stop.
inline std::mutex &flushAbortMutex()
{
    static std::mutex m;
    return m;
}
inline std::function<int64_t()> &flushAbortFunction()
{
    static std::function<int64_t()> fn;
    return fn;
}
inline int64_t abortSafetyStopFlush()
{
    std::lock_guard<std::mutex> lock(flushAbortMutex());
    return flushAbortFunction() ? flushAbortFunction()() : -1;
}
// Called by the write queue under its own mutex, right after the push that reached the threshold. Must be
// lock-free and must never stop anything itself: ApertarCoreApp sets an atomic and notifies a condition variable.
struct SafetyStopHit {
    uint64_t used = 0;
    uint64_t capacity = 0;
    unsigned percent = 0;   // the configured threshold
    bool ram = false;       // unit: bytes (ram) or frames
};
inline std::function<void(const SafetyStopHit &)> &safetyStopHook()
{
    static std::function<void(const SafetyStopHit &)> hook;   // set once, before any take
    return hook;
}

// What the write buffer is asked to be, and why the last take did what it did. Written by the camera at each
// take's start and by Core when the hardware file changes; read by the state, the "buffer" event and /api/buffer.
// One tiny mutex, held only to copy two short strings, so the lock-free buffer-events thread may read it.
struct BufferSettingSnapshot {
    std::string setting = "frames";   // "frames" | "ram": the effective request (environment, else file, else frames)
    std::string note;                 // the last take's plan line when ram was requested, else ""
    bool operator==(const BufferSettingSnapshot &o) const { return setting == o.setting && note == o.note; }
};

inline BufferSettingSnapshot &bufferSettingStorage()
{
    static BufferSettingSnapshot value;
    return value;
}
inline std::mutex &bufferSettingMutex()
{
    static std::mutex mutex;
    return mutex;
}
inline BufferSettingSnapshot bufferSettingSnapshot()
{
    std::lock_guard<std::mutex> lock(bufferSettingMutex());
    return bufferSettingStorage();
}
inline void publishBufferSetting(const std::string &setting, const std::string &note)
{
    std::lock_guard<std::mutex> lock(bufferSettingMutex());
    bufferSettingStorage() = {setting, note};
}
// Only the request changed (the file was saved): the note belongs to the last take and stays.
inline void publishBufferRequest(const std::string &setting)
{
    std::lock_guard<std::mutex> lock(bufferSettingMutex());
    bufferSettingStorage().setting = setting;
}
// The effective request: a set APERTAR_CDNG_BUFFER wins (anything but "ram" is frames), else the file's buffer=.
inline std::string requestedBufferMode(const char *envValue, const BufferConf &conf)
{
    if (envValue && *envValue)
        return std::string(envValue) == "ram" ? "ram" : "frames";
    return conf.ram.value_or(false) ? "ram" : "frames";
}

// frames_per_folder of the next take: a set APERTAR_CDNG_FRAMES_PER_FOLDER wins (a bad value is 0, one folder), else the
// file's frames_per_folder=, else 0. Published as an atomic (state frames_per_folder); the camera reads it again at each take.
inline unsigned requestedFramesPerFolder(const char *envValue, const BufferConf &conf)
{
    if (envValue && *envValue)
        return framepath::parseFramesPerFolder(envValue).value_or(0);
    return conf.framesPerFolder.value_or(0);
}
inline std::atomic<unsigned> &framesPerFolderRequest()
{
    static std::atomic<unsigned> value{0};
    return value;
}

}  // namespace bufferbudget

inline BufferPlan planBuffer(const BufferPlanInputs &in)
{
    using namespace bufferbudget;
    const double fraction = clampFraction(in.fraction, 0.05, 0.9, 0.6);
    const double maxFraction = clampFraction(in.maxFraction, 0.1, 0.75, 0.5);
    const uint64_t floorBytes = saturatingMul(std::min<uint64_t>(in.floorMb, 1ull << 20), kMiB);

    // 1. a fraction of what is available now, minus a floor the system keeps; 2. never above a share of the
    // whole memory; both cannot underflow: below the floor the budget is 0.
    const uint64_t share = scaleBytes(in.memAvailableBytes, fraction);
    uint64_t budget = share > floorBytes ? share - floorBytes : 0;
    const uint64_t cap = scaleBytes(in.memTotalBytes, maxFraction);
    budget = std::min(budget, cap);

    // 3. "ram" must never hold less than the original frames queue would: a small board keeps "frames".
    const uint64_t framesModeBytes = saturatingMul(in.framesModeFrames, in.frameBytes);
    BufferPlan plan;
    plan.budgetBytes = budget;
    char text[256];
    if (in.frameBytes == 0) {
        plan.reason = "frame size unknown: staying on frames";
        return plan;
    }
    if (budget < framesModeBytes || budget < in.frameBytes) {
        std::snprintf(text, sizeof text, "ram budget %llu MiB below the frames-mode %llu MiB: staying on frames",
                      static_cast<unsigned long long>(budget / kMiB),
                      static_cast<unsigned long long>(framesModeBytes / kMiB));
        plan.reason = text;
        return plan;
    }
    std::snprintf(text, sizeof text,
                  "ram budget %llu MiB (MemAvailable %llu MiB, fraction %.2f, floor %llu MiB, cap %llu MiB, "
                  "frame %.1f MiB = %llu frames)",
                  static_cast<unsigned long long>(budget / kMiB),
                  static_cast<unsigned long long>(in.memAvailableBytes / kMiB), fraction,
                  static_cast<unsigned long long>(floorBytes / kMiB),
                  static_cast<unsigned long long>(cap / kMiB),
                  static_cast<double>(in.frameBytes) / static_cast<double>(kMiB),
                  static_cast<unsigned long long>(budget / in.frameBytes));
    plan.ram = true;
    plan.reason = text;
    return plan;
}

}  // namespace apertar
