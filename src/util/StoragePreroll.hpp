#pragma once

// The storage pre-roll's pure decisions (std only, so tools/storage_preroll_test.cpp runs on any host): the two
// [media] keys, the guard that says run or skip, and which trigger a media change is. The mechanism is in
// ApertarCoreApp::runStoragePreroll and CameraController::startStoragePreroll.

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

namespace apertar::storagepreroll {

constexpr unsigned kDefaultSeconds = 2;
constexpr unsigned kMinSeconds = 1;
constexpr unsigned kMaxSeconds = 10;

// [media] preroll=on|off (default on: the automatic triggers; the manual command always works)
// and preroll_seconds=1..10 (default 2). A missing or bad key is "not set".
struct PrerollConf {
    std::optional<bool> automatic;
    std::optional<unsigned> seconds;
};

// Why `value` is not a good value of a pre-roll key, or "" when it is (or when the key is not one of the two).
inline std::string prerollConfProblem(const std::string &key, const std::string &value)
{
    if (key == "preroll") {
        std::string lowered = value;
        for (char &c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (lowered == "on" || lowered == "off") ? "" : "is not on or off";
    }
    if (key == "preroll_seconds") {
        const bool digits = !value.empty() && value.size() <= 2 && value.find_first_not_of("0123456789") == std::string::npos;
        const unsigned long seconds = digits ? std::strtoul(value.c_str(), nullptr, 10) : 0;
        return (digits && seconds >= kMinSeconds && seconds <= kMaxSeconds) ? "" : "is not a whole number of seconds from 1 to 10";
    }
    return "";
}

// The [media] section of the file's text; the last good line of a key counts; a value with a trailing comment is ignored.
inline PrerollConf parsePrerollConf(const std::string &text)
{
    PrerollConf conf;
    bool inMedia = false;
    size_t pos = 0;
    const auto trim = [](std::string s) {
        const char *ws = " \t\r\f\v";
        const auto first = s.find_first_not_of(ws);
        if (first == std::string::npos)
            return std::string();
        return s.substr(first, s.find_last_not_of(ws) - first + 1);
    };
    const auto lower = [](std::string s) {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            inMedia = lower(trim(line.substr(1, line.size() - 2))) == "media";
            continue;
        }
        const auto eq = line.find('=');
        if (!inMedia || eq == std::string::npos)
            continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = lower(trim(line.substr(eq + 1)));
        if (value.find_first_of("#;") != std::string::npos || !prerollConfProblem(key, value).empty())
            continue;
        if (key == "preroll")
            conf.automatic = value == "on";
        else if (key == "preroll_seconds")
            conf.seconds = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
    }
    return conf;
}

// APERTAR_STORAGE_PREROLL=0|1 (anything else counts as unset) beats the file; absent both: on.
inline bool automaticEnabled(const char *envValue, const PrerollConf &conf)
{
    if (envValue && std::string(envValue) == "0")
        return false;
    if (envValue && std::string(envValue) == "1")
        return true;
    return conf.automatic.value_or(true);
}

// A requested length (the command's `seconds`, clamped by the caller's parse) else the file's, else 2.
inline unsigned effectiveSeconds(std::optional<unsigned> requested, const PrerollConf &conf)
{
    const unsigned value = requested.value_or(conf.seconds.value_or(kDefaultSeconds));
    return value < kMinSeconds ? kMinSeconds : (value > kMaxSeconds ? kMaxSeconds : value);
}

// The command's `seconds`: a whole number 1..10, else nullopt.
inline std::optional<unsigned> parseSecondsArgument(const std::string &value)
{
    if (!prerollConfProblem("preroll_seconds", value).empty())
        return std::nullopt;
    return static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
}

enum class Trigger { Manual, Start, Insert, Format };
inline const char *triggerName(Trigger trigger)
{
    switch (trigger) {
    case Trigger::Manual: return "manual";
    case Trigger::Start: return "start";
    case Trigger::Insert: return "mount";
    case Trigger::Format: return "format";
    }
    return "manual";
}

struct GuardInputs {
    bool recording = false;
    bool soundRecording = false;
    bool timelapse = false;
    bool noMedia = false;
    bool mediaUsable = true;     // mounted, answering, not full (the record gate)
    bool standby = false;
    bool cameraReady = true;     // the camera runs and is in Ready (not starting, not in error)
    bool deleteActive = false;
    bool formatActive = false;
    bool speedTest = false;
    bool prerollActive = false;
    bool stillMode = false;
    bool rawFormat = true;       // cDNG, Lossy DNG or MLV: what the DNG write queue carries
};
struct GuardDecision {
    bool run = true;
    std::string reason;          // "" when run; else the short reason ("recording", "no media", ...)
};

// One list, one order, for the command and the automatic triggers. Trigger does not change the answer; it is
// here so a future rule can differ (and the tests name it).
inline GuardDecision decideGuard(const GuardInputs &in, Trigger)
{
    const auto skip = [](const char *why) { return GuardDecision{false, why}; };
    if (in.prerollActive) return skip("a pre-roll is already running");
    if (in.recording) return skip("recording");
    if (in.soundRecording) return skip("sound recording");
    if (in.timelapse) return skip("time-lapse");
    if (in.standby) return skip("standby");
    if (in.deleteActive) return skip("deleting media");
    if (in.formatActive) return skip("formatting");
    if (in.speedTest) return skip("media speed test");
    if (in.noMedia) return skip("no media");
    if (!in.mediaUsable) return skip("media not usable");
    if (!in.cameraReady) return skip("camera not ready");
    if (in.stillMode) return skip("still mode");
    if (!in.rawFormat) return skip("not a raw format");
    return {};
}

// What a change of the medium means. The first cycle of the media watcher is the start-up mount (once per Core
// run); a later transition to mounted from not mounted, or to another device, is a card inserted. A format
// is requested by the format command itself, so a change seen while a format runs, or within a few seconds of
// one, is not an insert.
struct MountChange {
    bool havePrevious = false;     // the watcher has seen an earlier cycle
    bool previousMounted = false;
    std::string previousDevice;
    bool currentMounted = false;
    std::string currentDevice;
    bool startDone = false;        // the start trigger was already requested in this run
    bool formatRunning = false;
    int64_t msSinceFormat = -1;    // since the last format finished, -1 = never
};
constexpr int64_t kFormatQuietMs = 15000;

inline std::optional<Trigger> triggerForMountChange(const MountChange &c)
{
    if (!c.currentMounted)
        return std::nullopt;
    if (c.formatRunning || (c.msSinceFormat >= 0 && c.msSinceFormat < kFormatQuietMs))
        return std::nullopt;
    if (!c.havePrevious)
        return c.startDone ? std::nullopt : std::optional<Trigger>(Trigger::Start);
    if (!c.previousMounted || c.previousDevice != c.currentDevice)
        return Trigger::Insert;
    return std::nullopt;   // same medium, another property (fstype, card presence): not a new card
}

// Measured frame rate of the take: (frames - 1) / (last - first) from the sensor timestamps of the first and last
// frame the take enqueued. 0 = not measurable (fewer than two frames, or the timestamps do not advance).
inline double measuredFps(uint64_t frames, int64_t firstTimestampUs, int64_t lastTimestampUs)
{
    if (frames < 2 || lastTimestampUs <= firstTimestampUs)
        return 0.0;
    return static_cast<double>(frames - 1) * 1e6 / static_cast<double>(lastTimestampUs - firstTimestampUs);
}

// storage_preroll_last texts; measured = 0 leaves "(measured x)" out
inline std::string lastOk(double seconds, double fps, uint64_t frames, uint64_t bytes, double measured = 0.0)
{
    char fpsText[64];
    if (measured > 0.0)
        std::snprintf(fpsText, sizeof fpsText, "%g fps (measured %.1f)", fps, measured);
    else
        std::snprintf(fpsText, sizeof fpsText, "%g fps", fps);
    char text[224];
    std::snprintf(text, sizeof text, "ok: %.1f s at %s, %llu frames, %.1f GB written and deleted", seconds, fpsText,
                  static_cast<unsigned long long>(frames), static_cast<double>(bytes) / 1e9);
    return text;
}

// The state the web state, /api/buffer and the "buffer" event carry: storage_preroll_active (true from the moment the
// pre-roll takes the camera until its clip is deleted and the settings are restored) and storage_preroll_last.
// Plain atomic and one tiny mutex held only to copy a short string, so the lock-free events thread may read both.
struct PrerollState {
    std::atomic<bool> active{false};
    std::atomic<double> fps{0.0};   // the fps the pre-roll applied while active; 0 otherwise
    // The effective automatic setting and whether APERTAR_STORAGE_PREROLL decides it, published by the app whenever the
    // hardware file changes (publishAutomatic), so the events thread and get_state read two atomics, never the file.
    std::atomic<bool> automatic{true};
    std::atomic<bool> automaticFromEnv{false};
    std::mutex lastMutex;
    std::string last;
};
inline PrerollState &prerollState()
{
    static PrerollState state;
    return state;
}
// Same rule as automaticEnabled; env is "0" or "1" only when it overrides the file.
inline void publishAutomatic(const char *envValue, const PrerollConf &conf)
{
    const bool fromEnv = envValue && (std::string(envValue) == "0" || std::string(envValue) == "1");
    prerollState().automaticFromEnv.store(fromEnv, std::memory_order_release);
    prerollState().automatic.store(automaticEnabled(envValue, conf), std::memory_order_release);
}
inline bool prerollAutomatic() { return prerollState().automatic.load(std::memory_order_acquire); }
inline bool prerollAutomaticFromEnv() { return prerollState().automaticFromEnv.load(std::memory_order_acquire); }
inline bool prerollActive() { return prerollState().active.load(std::memory_order_acquire); }
inline double prerollFps() { return prerollState().fps.load(std::memory_order_acquire); }
// 60, 29.97: no trailing zeros, "0" when no pre-roll is active.
inline std::string prerollFpsText(double fps)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.3f", fps);
    std::string out = text;
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out.empty() ? "0" : out;
}
inline std::string prerollLast()
{
    std::lock_guard<std::mutex> lock(prerollState().lastMutex);
    return prerollState().last;
}
inline void setPrerollLast(const std::string &text)
{
    std::lock_guard<std::mutex> lock(prerollState().lastMutex);
    prerollState().last = text;
}

}  // namespace apertar::storagepreroll
