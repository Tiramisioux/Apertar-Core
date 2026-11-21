#pragma once
// The four dual-sensor UI.conf settings (design §5.1, §6.1, §4.4; T63). Pure: std only, host-tested.
//
// previewSource, recordCameras and cam1Mode copy the externalMonitorMode quartet's normaliser: lower-case match on the
// trimmed text, any other value gives the default. The command's strict check is then "trimmed text equals the
// normalised label", so "pip_cam0", "both" and "Cam0" are refused. cam1Settings is a list of shared controls.
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace apertar::dualsettings {

namespace detail {
inline std::string trim(std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
        --end;
    return std::string(text.substr(begin, end - begin));
}

inline std::string lower(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

template <size_t N>
inline std::string normalizeLabel(std::string_view value, const std::array<const char *, N> &labels)
{
    const std::string key = lower(trim(value));
    for (const char *label : labels)
        if (key == lower(label))
            return label;
    return labels[0];   // the default is the first label
}
} // namespace detail

inline constexpr std::array<const char *, 6> kPreviewSources = {
    "Both", "Cam 0", "Cam 1", "PiP Cam 0", "PiP Cam 1", "Both (Cam 1 left)"};
inline constexpr std::array<const char *, 4> kRecordCameras = {"Both", "Cam 0", "Cam 1", "Follow preview"};
inline constexpr std::array<const char *, 2> kCam1Modes = {"Closest to Cam 0", "Set on Cam 1"};
// "unison": CAM1 follows CAM0 in everything (the Dual Sensor card's Unison switch). A list names the controls tied
// one by one with Unison off: all five listed is not Unison, so the card's switches stay as set.
inline constexpr const char *kCam1SettingsUnison = "unison";
inline constexpr const char *kCam1SettingsDefault = kCam1SettingsUnison;

inline std::string normalizePreviewSource(std::string_view value)
{
    return detail::normalizeLabel(value, kPreviewSources);
}

// HDMI 1's view while dual sensor is on (design §5.4's hdmi1Source): the same six labels; its default is Cam 1.
// With one sensor, or dual sensor off, both ports show CAM0's preview and this is kept for later.
inline constexpr const char *kHdmi1SourceDefault = "Cam 1";

inline std::string normalizeHdmi1Source(std::string_view value)
{
    const std::string label = normalizePreviewSource(value);
    return detail::trim(value).empty() || (label == kPreviewSources[0] && detail::lower(detail::trim(value)) != "both")
        ? std::string(kHdmi1SourceDefault) : label;
}

inline std::string normalizeRecordCameras(std::string_view value)
{
    return detail::normalizeLabel(value, kRecordCameras);
}

inline std::string normalizeCam1Mode(std::string_view value)
{
    return detail::normalizeLabel(value, kCam1Modes);
}

// `set_preview_source swap=true` on HDMI 0's view: Cam 0 <-> Cam 1, PiP Cam 0 <-> PiP Cam 1,
// Both <-> Both (Cam 1 left). Takes a normalised label.
inline std::string swapPreviewSource(const std::string &label)
{
    static const char *const pairs[][2] = {
        {"Cam 0", "Cam 1"}, {"PiP Cam 0", "PiP Cam 1"}, {"Both", "Both (Cam 1 left)"}};
    for (const auto &pair : pairs) {
        if (label == pair[0])
            return pair[1];
        if (label == pair[1])
            return pair[0];
    }
    return label;
}

// Wipe and stack in a single-camera view (design v2.5 A5): previewWipe 0..100, previewStack, previewOpacity 0..100,
// previewWipeDirection Horizontal|Vertical. The command's fields are strict, the UI.conf reader clamps.
inline constexpr int kPreviewWipeDefault = 0;
inline constexpr int kPreviewOpacityDefault = 50;

// Strict 0..100 (a number, no trailing text); a fraction rounds. nullopt for anything else.
inline std::optional<int> parsePercent(std::string_view value)
{
    const std::string text = detail::trim(value);
    if (text.empty())
        return std::nullopt;
    try {
        size_t used = 0;
        const double number = std::stod(text, &used);
        if (used != text.size() || !(number >= 0.0) || number > 100.0)
            return std::nullopt;
        return static_cast<int>(number + 0.5);
    } catch (...) {
        return std::nullopt;
    }
}

// UI.conf reader: unreadable gives the default, out of range clamps.
inline int normalizePercent(std::string_view value, int fallback)
{
    const std::string text = detail::trim(value);
    try {
        size_t used = 0;
        const double number = std::stod(text, &used);
        if (used != text.size() || number != number)
            return fallback;
        return number < 0.0 ? 0 : (number > 100.0 ? 100 : static_cast<int>(number + 0.5));
    } catch (...) {
        return fallback;
    }
}

// Strict boolean of the command: true|false|1|0, case-insensitive.
inline std::optional<bool> parseStackFlag(std::string_view value)
{
    const std::string key = detail::lower(detail::trim(value));
    if (key == "true" || key == "1")
        return true;
    if (key == "false" || key == "0")
        return false;
    return std::nullopt;
}

// "Horizontal" (default) or "Vertical", case-insensitive; anything else gives the default.
inline std::string normalizeWipeDirection(std::string_view value)
{
    return detail::lower(detail::trim(value)) == "vertical" ? "Vertical" : "Horizontal";
}

// The command's strict form: horizontal|vertical, case-insensitive, to the UI.conf label; nullopt otherwise.
inline std::optional<std::string> parseWipeDirection(std::string_view value)
{
    const std::string key = detail::lower(detail::trim(value));
    if (key == "horizontal")
        return std::string("Horizontal");
    if (key == "vertical")
        return std::string("Vertical");
    return std::nullopt;
}

// cam1Settings: "unison" alone, or the shared controls as a comma list. Tokens are exact lower case, in any order, no
// repeats; spaces around a token are trimmed. Returns "unison", the list in the fixed order, "" for none, or nullopt
// for anything else (an unknown token, a repeat, "unison" in a list, an empty token such as "iso,,fps" or a trailing
// comma).
inline std::optional<std::string> canonicalCam1Settings(std::string_view value)
{
    static const char *const order[] = {"resolution", "iso", "shutter", "fps", "wb"};
    constexpr size_t count = sizeof(order) / sizeof(order[0]);
    const std::string text = detail::trim(value);
    if (text.empty())
        return std::string();
    if (text == kCam1SettingsUnison)
        return std::string(kCam1SettingsUnison);

    bool seen[count] = {false, false, false, false, false};
    size_t start = 0;
    while (true) {
        const size_t comma = text.find(',', start);
        const std::string token =
            detail::trim(std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        size_t index = count;
        for (size_t i = 0; i < count; ++i)
            if (token == order[i])
                index = i;
        if (index == count || seen[index])
            return std::nullopt;
        seen[index] = true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }

    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (!seen[i])
            continue;
        if (!out.empty())
            out += ',';
        out += order[i];
    }
    return out;
}

// The reader's form: an unreadable list gives the default. UI.conf is also written by Qt's QSettings, which rewrites a
// comma value as a list ("resolution, iso") or as a quoted string ("\"resolution,iso\""), and an empty list as
// @Invalid(): all three read back as written by Core.
inline std::string normalizeCam1Settings(std::string_view value)
{
    std::string text = detail::trim(value);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    if (text == "@Invalid()")
        text.clear();
    const auto canonical = canonicalCam1Settings(text);
    return canonical ? *canonical : std::string(kCam1SettingsDefault);
}

inline bool cam1SettingsHasResolution(const std::string &canonical)
{
    // Unison ties everything; else the fixed order puts resolution first.
    return canonical == kCam1SettingsUnison || canonical.rfind("resolution", 0) == 0;
}

} // namespace apertar::dualsettings
