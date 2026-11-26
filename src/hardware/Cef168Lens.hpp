#pragma once
// Pinefeat CEF168 lens controller (Canon EF), the Linux driver "cef168": a V4L2 subdevice on the camera's I2C bus.
// This header is the pure part (the 15-byte data frame, its CRC, the third-stop iris table, the stepping and the
// get_lens JSON): no I/O, host-tested by tools/lens_test.cpp. The device part is Cef168Lens.cpp.
#include "hardware/LensLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apertar::hardware::cef168 {

// ---- the data frame ("<BBHHHHHHB", cef168.h struct cef168_data) -------------------------------------------------------

inline constexpr size_t kFrameLength = 15;
inline constexpr unsigned kInfiniteCm = 65535;   // focus_distance_max for infinity

struct Data {
    uint8_t lensId = 0;
    bool moving = false;
    unsigned calibrating = 0;     // bits 1-2 of the flags byte, 0 = idle
    unsigned movingTime = 0;
    int focusMin = 0;
    int focusMax = 0;
    int focusCurrent = 0;
    unsigned distanceMinCm = 0;
    unsigned distanceMaxCm = 0;   // kInfiniteCm = infinity
    uint8_t crc = 0;

    int span() const { return focusMax - focusMin; }
    // A range of 0..0, ..32767 or ..65535 is a lens the board has not calibrated yet; after a run it holds the real range.
    bool rangeValid() const { return focusRangeValid(focusMin, focusMax); }
};

// CRC-8, MSB first, polynomial 0xA8 (168), initial value 0xFF: the kernel's crc8() with CRC8_INIT_VALUE, as cef168.c uses it.
inline uint8_t crc8(const uint8_t *bytes, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xA8) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

// The frame as the Data control returns it (little-endian). Nothing for a wrong length or a CRC that does not match.
inline std::optional<Data> decode(const uint8_t *raw, size_t length, std::string *error = nullptr)
{
    auto fail = [&](const std::string &message) -> std::optional<Data> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (length != kFrameLength)
        return fail("The lens data frame is " + std::to_string(length) + " bytes, expected 15.");
    if (crc8(raw, kFrameLength - 1) != raw[kFrameLength - 1])
        return fail("The lens data frame fails its CRC.");
    auto word = [&](size_t at) { return static_cast<unsigned>(raw[at] | (raw[at + 1] << 8)); };
    Data data;
    data.lensId = raw[0];
    data.moving = (raw[1] & 0x01) != 0;
    data.calibrating = (raw[1] >> 1) & 0x03;
    data.movingTime = word(2);
    data.focusMin = static_cast<int>(word(4));
    data.focusMax = static_cast<int>(word(6));
    data.focusCurrent = static_cast<int>(word(8));
    data.distanceMinCm = word(10);
    data.distanceMaxCm = word(12);
    data.crc = raw[14];
    return data;
}

// ---- the subdev name: "cef168 <bus>-000d" ----------------------------------------------------------------------------

// The bus of a subdev name, or nothing when it is not a cef168. -1 = a cef168 whose bus the name does not give.
inline std::optional<int> busOfSubdevName(std::string_view name)
{
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.remove_suffix(1);
    constexpr std::string_view prefix = "cef168";
    if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
    if (name.size() > prefix.size() && name[prefix.size()] != ' ') return std::nullopt;
    std::string_view rest = name.substr(prefix.size());
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    int bus = 0;
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9' && digits < 4) bus = bus * 10 + (rest[digits++] - '0');
    if (digits == 0 || digits >= rest.size() || rest[digits] != '-') return -1;
    return bus;
}

// ---- settings (apertar-hardware.conf [lens]) ---------------------------------------------------------------------------

// What the controls use: the two steps are global ([lens] in apertar-hardware.conf), the rest is the working lens.
struct Settings {
    double apertureMin = 0.0;       // the widest f-number the lens opens to, entered by the operator; 0 = not entered
    double apertureMax = 0.0;       // the narrowest; 0 = not entered
    double focusStepPercent = 1.0;  // one focus step, in percent of the lens's focus range
    int irisStepThirds = 1;         // one iris step, in third stops
    double iris = 0.0;              // the working lens's last commanded f-number; 0 = none
};

inline constexpr double kApertureLowest = 0.5;    // what an entered f-number may be
inline constexpr double kApertureHighest = 128.0;

// ---- iris --------------------------------------------------------------------------------------------------------------

// Third stops. The lens takes f x 100 and cannot be read back, so these are the values Apertar offers.
inline const std::vector<double> &irisTable()
{
    static const std::vector<double> table = {1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5, 2.8, 3.2, 3.5, 4.0, 4.5, 5.0,
        5.6, 6.3, 7.1, 8.0, 9.0, 10.0, 11.0, 13.0, 14.0, 16.0, 18.0, 20.0, 22.0, 25.0, 29.0, 32.0};
    return table;
}

inline double roundF(double value) { return std::round(value * 100.0) / 100.0; }

// The configured range, ordered; nothing when neither end is entered. One end entered leaves the other open.
inline std::optional<std::pair<double, double>> apertureRange(const Settings &settings)
{
    if (settings.apertureMin <= 0.0 && settings.apertureMax <= 0.0) return std::nullopt;
    const double low = settings.apertureMin > 0.0 ? settings.apertureMin : irisTable().front();
    const double high = settings.apertureMax > 0.0 ? settings.apertureMax : irisTable().back();
    return std::pair<double, double>{std::min(low, high), std::max(low, high)};
}

// The f-numbers on offer, ascending: the table inside the range, with the range's own ends (a lens that stops down to
// f/22.6 offers 22.6). No range: the whole table.
inline std::vector<double> irisSteps(const Settings &settings)
{
    const auto range = apertureRange(settings);
    if (!range) return irisTable();
    constexpr double eps = 1e-6;
    std::vector<double> steps;
    for (const double stop : irisTable())
        if (stop >= range->first - eps && stop <= range->second + eps) steps.push_back(stop);
    steps.push_back(roundF(range->first));
    steps.push_back(roundF(range->second));
    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), steps.end());
    return steps;
}

// Inside the configured range, else inside the whole table's ends.
inline double clampIris(double fNumber, const Settings &settings)
{
    const auto range = apertureRange(settings);
    const double low = range ? range->first : irisTable().front();
    const double high = range ? range->second : irisTable().back();
    return roundF(std::clamp(fNumber, low, high));
}

// `count` third stops from `current` along irisSteps(): positive = a higher f-number (closing). The result is always
// a listed step. A current value off the list first snaps to the nearest step in the direction of travel, so one step
// up from f/2.9 is 3.2 and one step down is 2.8. Nothing commanded yet answers the widest step.
inline double stepIris(std::optional<double> current, int count, const Settings &settings)
{
    const std::vector<double> steps = irisSteps(settings);
    if (!current) return steps.front();
    constexpr double eps = 1e-6;
    long index = 0;
    if (count > 0) {
        const auto later = std::find_if(steps.begin(), steps.end(), [&](double s) { return s > *current + eps; });
        index = later == steps.end() ? static_cast<long>(steps.size()) - 1 : (later - steps.begin()) + count - 1;
    } else if (count < 0) {
        long earlier = -1;
        for (size_t i = 0; i < steps.size(); ++i)
            if (steps[i] < *current - eps) earlier = static_cast<long>(i);
        index = earlier < 0 ? 0 : earlier + count + 1;
    } else {
        return clampIris(*current, settings);
    }
    return steps[static_cast<size_t>(std::clamp<long>(index, 0, static_cast<long>(steps.size()) - 1))];
}

// What a pot at 0..1 means: the listed step it sits on.
inline double irisAtFraction(double fraction, const Settings &settings)
{
    const std::vector<double> steps = irisSteps(settings);
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    return steps[std::min(steps.size() - 1, static_cast<size_t>(std::llround(clamped * static_cast<double>(steps.size() - 1))))];
}

// IRIS_ABSOLUTE takes the f-number x 100. -1 for something that is no aperture.
inline int irisControlValue(double fNumber)
{
    if (!std::isfinite(fNumber) || fNumber < kApertureLowest || fNumber > kApertureHighest) return -1;
    return static_cast<int>(std::lround(fNumber * 100.0));
}

// ---- focus -------------------------------------------------------------------------------------------------------------

// One focus step in motor units: focusStepPercent of the range low..high (the working lens's calibrated range), at least 1.
// 0 when there is no range.
inline int focusStepSize(int low, int high, double percent)
{
    if (high <= low) return 0;
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(high - low) * percent / 100.0)));
}

// `count` focus steps from `current`, kept inside low..high. Positive = farther (towards infinity).
inline int stepFocus(int current, int low, int high, int count, double percent)
{
    const long target = static_cast<long>(current) + static_cast<long>(count) * focusStepSize(low, high, percent);
    return static_cast<int>(std::clamp<long>(target, low, high));
}

inline int focusAtFraction(double fraction, int low, int high)
{
    return low + static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(high - low)));
}

// ---- state and its JSON ------------------------------------------------------------------------------------------------

struct Control {
    uint32_t id = 0;
    std::string name;
    std::string type;                 // integer, boolean, menu, button, u8, ...
    long long minimum = 0, maximum = 0, step = 0, defaultValue = 0;
    std::optional<long long> value;   // nothing for a write-only or compound control, or one that failed to read
    bool readOnly = false, writeOnly = false, isVolatile = false, executeOnWrite = false;
};

// One calibration run of the board (the Calibrate button control): press, then Data is polled every 0.5 ms on a worker
// thread until `calibrating` clears (the whole run takes ~0.3 s), as Pinefeat's calibrate.cpp does. The result is Core's own.
struct Calibration {
    std::string state = "idle";       // idle, running, done, failed
    std::string message;
    std::string startedAt, finishedAt;   // ISO UTC
    unsigned movingTimeMs = 0;
    std::optional<int> focusMin, focusMax;
    std::optional<unsigned> distanceMinCm, distanceMaxCm;
    std::vector<double> pwlMap;       // inverse distance (m^-1), position, ...: the rpi.af "map"; empty = none
    std::optional<unsigned> stepFrames;
};

struct Status {
    Settings settings;
    Library library;
    Calibration calibration;
    bool present = false;             // the subdev exists
    std::string subdev;               // /dev/v4l-subdevN
    int bus = -1;
    std::optional<Data> data;         // nothing when the board did not answer
    std::optional<double> iris;       // the last commanded f-number
    std::vector<Control> controls;
    std::string error;
};

inline std::string jsonString(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

inline std::string jsonNum(double value, int precision = 2)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text;
}

// absent (no adapter), error (the board does not answer), calibrating, no_lens (lens id 0: assumed, not confirmed on
// hardware), unknown_lens (nothing saved selected), uncalibrated (the working lens has no range from Core's own calibration run), ready.
// The focus range the controls use: the working lens's (Core's own run), else the board's when valid.
inline std::optional<std::pair<int, int>> statusFocusRange(const Status &status)
{
    const Data *data = status.data ? &*status.data : nullptr;
    return effectiveFocusRange(status.library.working, data ? data->focusMin : 0, data ? data->focusMax : 0, data != nullptr);
}

inline std::string lensState(const Status &status)
{
    if (!status.present) return "absent";
    if (!status.data) return "error";
    if (status.calibration.state == "running" || status.data->calibrating != 0) return "calibrating";
    if (status.data->lensId == 0) return "no_lens";
    if (!find(status.library, status.library.selected)) return "unknown_lens";
    if (!statusFocusRange(status)) return "uncalibrated";
    return "ready";
}

inline std::string pwlJson(const std::vector<double> &map)
{
    std::string out = "[";
    for (size_t i = 0; i < map.size(); ++i) out += (i ? "," : "") + jsonNum(map[i], 5);
    return out + "]";
}
inline std::string uintOrNull(const std::optional<unsigned> &value) { return value ? std::to_string(*value) : "null"; }

inline std::string cmJson(const std::optional<unsigned> &value)
{
    return value && *value != kInfiniteCm ? std::to_string(*value) : "null";
}
inline std::string boolJson(const std::optional<bool> &value) { return value ? (*value ? "true" : "false") : "null"; }
inline std::string optJson(const std::optional<int> &value) { return value ? std::to_string(*value) : "null"; }
inline std::string strOrNull(const std::string &text) { return text.empty() ? "null" : jsonString(text); }

// The small `lens` object of get_state: what a status bar needs.
inline std::string statusSummaryJson(const Status &status)
{
    const Data *data = status.data ? &*status.data : nullptr;
    return "{\"present\":" + std::string(status.present ? "true" : "false") + ",\"state\":" + jsonString(lensState(status)) +
        ",\"lens_name\":" + jsonString(status.library.working.name) +
        ",\"iris\":" + (status.iris ? jsonNum(*status.iris) : "null") +
        ",\"focus_position\":" + (data ? std::to_string(data->focusCurrent) : "null") + "}";
}

// The members of the `lens` object of get_lens and set_lens, without the braces.
inline std::string statusJson(const Status &status, const std::string &afMode)
{
    std::ostringstream out;
    const Settings &settings = status.settings;
    const Library &library = status.library;
    const auto b = [](bool value) { return value ? "true" : "false"; };
    out << "\"present\":" << b(status.present)
        << ",\"state\":" << jsonString(lensState(status))
        << ",\"subdev\":" << (status.subdev.empty() ? "null" : jsonString(status.subdev))
        << ",\"bus\":" << (status.bus >= 0 ? std::to_string(status.bus) : "null");
    const Data *data = status.data ? &*status.data : nullptr;
    // focus_min/max: the working lens's calibrated range (Core's own run), else the board's when valid, else the board's raw
    // values (board_focus_min/max always give those). focus_calibrated says which of the first two applies.
    const auto range = statusFocusRange(status);
    const bool ownRange = focusRange(library.working).has_value();
    out << ",\"lens_id\":" << (data ? std::to_string(data->lensId) : "null")
        << ",\"moving\":" << (data ? b(data->moving) : "null")
        << ",\"calibrating\":" << (data ? b(data->calibrating != 0) : "null")
        << ",\"focus_calibrated\":" << b(range.has_value())
        << ",\"focus_min\":" << (range ? std::to_string(range->first) : data ? std::to_string(data->focusMin) : "null")
        << ",\"focus_max\":" << (range ? std::to_string(range->second) : data ? std::to_string(data->focusMax) : "null")
        << ",\"board_focus_min\":" << (data ? std::to_string(data->focusMin) : "null")
        << ",\"board_focus_max\":" << (data ? std::to_string(data->focusMax) : "null")
        << ",\"focus_position\":" << (data ? std::to_string(data->focusCurrent) : "null")
        << ",\"distance_min_cm\":" << (ownRange ? cmJson(library.working.distanceMinCm) : data ? cmJson(data->distanceMinCm) : "null")
        << ",\"distance_max_cm\":" << (ownRange ? cmJson(library.working.distanceMaxCm) : data ? cmJson(data->distanceMaxCm) : "null")
        << ",\"pwl_map\":" << pwlJson(library.working.pwlMap)
        << ",\"step_frames\":" << uintOrNull(library.working.stepFrames)
        << ",\"iris\":" << (status.iris ? jsonNum(*status.iris) : "null")
        << ",\"aperture_min\":" << (settings.apertureMin > 0.0 ? jsonNum(settings.apertureMin) : "null")
        << ",\"aperture_max\":" << (settings.apertureMax > 0.0 ? jsonNum(settings.apertureMax) : "null")
        << ",\"focus_step_percent\":" << jsonNum(settings.focusStepPercent)
        << ",\"iris_step_thirds\":" << settings.irisStepThirds
        << ",\"af_mode\":" << jsonString(afMode)
        << ",\"lens_key\":" << strOrNull(library.selected)
        << ",\"lens_name\":" << jsonString(library.working.name)
        << ",\"unsaved\":" << b(unsaved(library))
        << ",\"capabilities\":{\"iris\":" << boolJson(library.working.capIris) << ",\"focus\":" << boolJson(library.working.capFocus)
        << ",\"autofocus\":" << boolJson(library.working.capAutofocus) << "}"
        << ",\"calibration\":{\"state\":" << jsonString(status.calibration.state)
        << ",\"message\":" << strOrNull(status.calibration.message)
        << ",\"started_at\":" << strOrNull(status.calibration.startedAt)
        << ",\"finished_at\":" << strOrNull(status.calibration.finishedAt)
        << ",\"moving_time_ms\":" << status.calibration.movingTimeMs
        << ",\"focus_min\":" << optJson(status.calibration.focusMin)
        << ",\"focus_max\":" << optJson(status.calibration.focusMax)
        << ",\"distance_min_cm\":" << cmJson(status.calibration.distanceMinCm)
        << ",\"distance_max_cm\":" << cmJson(status.calibration.distanceMaxCm)
        << ",\"pwl_map\":" << pwlJson(status.calibration.pwlMap)
        << ",\"step_frames\":" << uintOrNull(status.calibration.stepFrames) << "}"
        << ",\"error\":" << strOrNull(status.error)
        << ",\"lenses\":[";
    bool first = true;
    for (const Entry &e : library.entries) {
        out << (first ? "" : ",") << "{\"key\":" << jsonString(e.key) << ",\"name\":" << jsonString(e.name)
            << ",\"lens_id\":" << (e.lensId >= 0 ? std::to_string(e.lensId) : "null")
            << ",\"aperture_min\":" << (e.apertureMin > 0.0 ? jsonNum(e.apertureMin) : "null")
            << ",\"aperture_max\":" << (e.apertureMax > 0.0 ? jsonNum(e.apertureMax) : "null")
            << ",\"calibrated\":" << b(e.calibrated())
            << ",\"focus_min\":" << optJson(e.focusMin) << ",\"focus_max\":" << optJson(e.focusMax)
            << ",\"distance_min_cm\":" << cmJson(e.distanceMinCm) << ",\"distance_max_cm\":" << cmJson(e.distanceMaxCm)
            << ",\"pwl_map\":" << pwlJson(e.pwlMap) << ",\"step_frames\":" << uintOrNull(e.stepFrames)
            << ",\"last_used\":" << strOrNull(e.lastUsed)
            << ",\"capabilities\":{\"iris\":" << boolJson(e.capIris) << ",\"focus\":" << boolJson(e.capFocus)
            << ",\"autofocus\":" << boolJson(e.capAutofocus) << "}"
            << ",\"in_use\":" << b(e.key == library.selected) << "}";
        first = false;
    }
    out << "],\"controls\":[";
    first = true;
    for (const Control &c : status.controls) {
        out << (first ? "" : ",") << "{\"id\":" << c.id << ",\"name\":" << jsonString(c.name)
            << ",\"type\":" << jsonString(c.type) << ",\"min\":" << c.minimum << ",\"max\":" << c.maximum
            << ",\"step\":" << c.step << ",\"default\":" << c.defaultValue
            << ",\"value\":" << (c.value ? std::to_string(*c.value) : "null")
            << ",\"flags\":{\"read_only\":" << b(c.readOnly) << ",\"write_only\":" << b(c.writeOnly)
            << ",\"volatile\":" << b(c.isVolatile) << ",\"execute_on_write\":" << b(c.executeOnWrite) << "}}";
        first = false;
    }
    out << "]";
    return out.str();
}

// ---- the device (Cef168Lens.cpp) ---------------------------------------------------------------------------------------

class Cef168Lens {
public:
    Cef168Lens();
    ~Cef168Lens();

    // Looks for the subdev, reads the data frame (and, with `controls`, every control), follows a calibration run and a
    // lens appearing or being swapped (the library picks its entry; the entry's last iris goes to the lens again).
    // Never throws; a lens that is not there is present=false. Cheap enough for a 2 s tick and for the WebUI's poll.
    Status read(bool controls);
    void poll();

    // Each returns "" or what went wrong. The f-number / position are what is sent: callers clamp.
    std::string setFocus(int position);
    std::string setIris(double fNumber);       // remembered in the working lens and the selected entry
    // Presses the board's Calibrate button and returns at once; a worker thread (its own fd) polls Data every 0.5 ms like
    // Pinefeat's calibrate.cpp and stores the result in `calibration` and the working lens. read() reports the run.
    std::string calibrate();
    // Any writable control, by id ("0x009809aa", "10027434") or name ("focus_absolute", "Focus, Absolute").
    std::string setControl(const std::string &idOrName, long long value);

    std::string summaryJson() const;           // the lens object of get_state, from the last look (the 2 s tick keeps it fresh)
    Settings settings();                       // [lens] steps + the working lens
    std::optional<double> iris() const;
    // Changes the library under the lock and saves lenses.conf when `change` returns "". Gets the time as ISO text.
    std::string modify(const std::function<std::string(Library &, const std::string &now)> &change);

private:
    struct Impl;
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

// [lens] in apertar-hardware.conf: the two global step sizes (focus_step_percent, iris_step_thirds).
Settings loadStepSettings();
// Writes the given [lens] keys; an empty value removes the key. "" or the error.
std::string saveStepSettings(const std::vector<std::pair<std::string, std::string>> &values);

} // namespace apertar::hardware::cef168
