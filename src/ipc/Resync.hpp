#pragma once
// The relay's resync, compare and clamp rules (dual-sensor design §4.4, amendment A2). Pure: std only, host-tested
// (resync_test.cpp). ControlClient keeps the clocks and the sockets; every decision about what to send or compare is here.
//
// A tied control is cam0's value: forwarded, resynced, compared. An untied one is per camera and left alone. `resolution`
// stands for the whole Recording group.
#include "CommandClasses.hpp"
#include "JsonKeys.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apertar::resync {

using commandclasses::Control;
using commandclasses::Fields;
using commandclasses::Ties;

// What the primary's command thread publishes for the relay (design 4.1 Locks): its settingsBody after each command, the
// preset built from the same settings, and its ladder definition (at start and after each ok set_mode_ladder).
struct PrimaryView {
    bool valid = false;                 // published at least once
    std::string settings = "{}";        // settingsBody as an object
    Fields preset;                      // the preset of those settings, flattened
    bool ladderKnown = false;
    std::string ladderResolutions;      // the ladder's `resolutions_text`
    bool ladderEnabled = false;
    bool ladderRampReady = false;
};

// ---- a preset as the primary saves it ------------------------------------------------------------------------------------

// `{"type":"apertar-preset",...,"settings":{...}}` flattened to its settings: strings unquoted, numbers and booleans as
// written. Empty when there is no settings object.
inline Fields flattenPreset(std::string_view payload)
{
    Fields out;
    const auto settings = jsonkeys::rawOf(payload, "settings");
    if (!settings || settings->empty() || (*settings)[0] != '{') return out;
    for (const jsonkeys::Member &member : jsonkeys::topLevel(*settings)) {
        const std::string key(member.key);
        const auto raw = settings->substr(member.valueBegin, member.end - member.valueBegin);
        if (raw.empty() || raw[0] == '{' || raw[0] == '[') continue;
        if (raw[0] == '"') {
            if (const auto text = jsonkeys::stringOf(*settings, key)) out[key] = *text;
        } else {
            out[key] = std::string(raw);
        }
    }
    return out;
}

// ---- the compare (§4.4) ----------------------------------------------------------------------------------------------

namespace detail {
inline bool isNumber(std::string_view text, double *value)
{
    if (text.empty() || text[0] == '"') return false;
    const std::string copy(text);
    char *end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') return false;
    *value = parsed;
    return true;
}

inline std::optional<std::string_view> raw(std::string_view body, std::string_view key) { return jsonkeys::rawOf(body, key); }

// Equal as written, or as numbers within half a thousandth (fps prints three decimals).
inline bool sameValue(std::string_view a, std::string_view b)
{
    double x = 0, y = 0;
    if (isNumber(a, &x) && isNumber(b, &y)) return std::fabs(x - y) < 0.0005;
    return a == b;
}

inline std::string lowerAlnum(std::string_view text)
{
    std::string out;
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
    }
    return out;
}

// ProRes and H.264 take no ladder (the rung is inactive there, CC:10954-10956).
inline bool processedFormat(std::string_view settings)
{
    const auto format = jsonkeys::stringOf(settings, "recording_format");
    if (!format) return false;
    const std::string name = lowerAlnum(*format);
    return name == "prores" || name == "h264";
}

// The primary's ladder is on but inactive in a processed format: cam1 refuses an enabled ladder there, so the ladder is
// skipped by the resync and the compare alike.
inline bool ladderInactiveHere(std::string_view primary)
{
    return jsonkeys::boolOf(primary, "ladder_enabled").value_or(false) && !jsonkeys::boolOf(primary, "ladder_active").value_or(false) &&
           processedFormat(primary);
}
} // namespace detail

inline bool ladderDiffers(std::string_view primary, std::string_view cam1)
{
    for (const char *key : {"ladder_enabled", "ladder_ramp_ready", "ladder_count", "ladder_tops"}) {
        const auto a = detail::raw(primary, key);
        const auto b = detail::raw(cam1, key);
        if (a && b && !detail::sameValue(*a, *b)) return true;
    }
    return false;
}

// The fields on which cam1's settingsBody differs from the primary's, by key, for the ties given. A key either body lacks
// is left out (an older build, or a cam1 that has not answered). It compares the resulting state, not the commands sent.
inline std::vector<std::string> differences(std::string_view primary, std::string_view cam1, const Ties &ties)
{
    std::vector<std::string> out;
    const auto plain = [&](const char *key) {
        const auto a = detail::raw(primary, key);
        const auto b = detail::raw(cam1, key);
        if (a && b && !detail::sameValue(*a, *b)) out.emplace_back(key);
    };
    const auto flag = [&](const char *key) { return jsonkeys::boolOf(primary, key).value_or(false); };

    // Always shared.
    for (const char *key : {"fps_phase_lock", "fps_speed_ramp", "fps_speed_ramp_keep_exposure", "fps_speed_ramp_stretch_audio",
                            "fps_speed_ramp_after", "timecode_mode"})
        plain(key);

    // Clear HDR forces auto ISO and auto WB off on cam1; that counts as a match (§4.4).
    const auto autoMatches = [&](const char *autoKey, const char *afterKey) {
        const auto a = jsonkeys::boolOf(primary, autoKey);
        const auto b = jsonkeys::boolOf(cam1, autoKey);
        if (!a || !b || *a == *b) return;
        if (!*b && jsonkeys::boolOf(cam1, afterKey).value_or(false)) return;
        out.emplace_back(autoKey);
    };

    if (ties.iso) {
        autoMatches("iso_auto", "iso_auto_after_clear_hdr");
        if (!flag("iso_auto")) {
            // ISO follows cam1's own caps: its iso against min(cam0's iso, cam1's iso_max).
            const auto a = jsonkeys::numberOf(primary, "iso");
            const auto b = jsonkeys::numberOf(cam1, "iso");
            const auto cap = jsonkeys::numberOf(cam1, "iso_max");
            if (a && b && std::fabs((cap && *cap > 0 ? std::min(*a, *cap) : *a) - *b) >= 0.5) out.emplace_back("iso");
        }
        plain("exposure_compensation");
        plain("exposure_metering");
    }
    if (ties.shutter) {
        plain("shutter_auto");
        if (!flag("shutter_auto")) plain("shutter_us");
    }
    if (ties.wb) {
        autoMatches("wb_auto", "wb_auto_after_clear_hdr");
        if (!flag("wb_auto")) plain("wb_kelvin");
    }
    if (ties.fps) {
        plain("fps");
        plain("conform_fps");
    }
    if (ties.resolution) {
        for (const char *key : {"resolution", "recording_format", "cdng_compression", "cdng_log", "cdng_log_bits", "lossy_dng_ratio",
                                "prores_profile", "prores_color_profile", "cdng_preview_color_profile", "tuning_file", "clear_hdr",
                                "hdr_threshold_high", "hdr_threshold_low", "hdr_blend", "hdr_gain_adder", "raw_bit_depth",
                                "h264_resolution", "af_mode", "af_range", "af_speed", "af_window_x", "af_window_y", "af_window_w",
                                "af_window_h"})
            plain(key);
        if (!detail::ladderInactiveHere(primary)) {
            for (const char *key : {"ladder_enabled", "ladder_ramp_ready", "ladder_override", "ladder_resolution", "ladder_count",
                                    "ladder_tops"})
                plain(key);
        }
    }
    return out;
}

// ---- the resync's steps (§4.4) -----------------------------------------------------------------------------------------

enum class Kind { None, LadderOff, Full };

struct Step {
    std::string name;
    Fields fields;
};

struct PlanInputs {
    Ties ties;
    Kind kind = Kind::Full;
    std::string primary = "{}";        // the primary's settingsBody as an object
    std::string cam1 = "{}";           // cam1's
    Fields primaryPreset;              // the primary's preset, flattened
    std::string ladderResolutions;     // the primary's ladder: its `resolutions_text`, "" when unknown
    bool ladderKnown = false;
    bool ladderEnabled = false;
    bool ladderRampReady = false;
};

inline const char *boolText(bool value) { return value ? "true" : "false"; }

// Up to three steps, each sent whatever the reply to the one before.
inline std::vector<Step> planResync(const PlanInputs &in)
{
    std::vector<Step> steps;
    const bool primaryLadderActive = jsonkeys::boolOf(in.primary, "ladder_active").value_or(false);
    const bool primaryOverride = jsonkeys::boolOf(in.primary, "ladder_override").value_or(false);
    const bool cam1Override = jsonkeys::boolOf(in.cam1, "ladder_override").value_or(false);

    // 1. The ladder. Resolution untied: cam1's own ladder stays off (it would refuse set_resolution camera=cam1 and rewrite
    // the mode at every fps change). Tied: it follows the primary's, unless that one is on but inactive in a processed format.
    if (!in.ties.resolution) {
        // A switch away from tied resolution always sends it, whatever the last poll said: that poll can be a second old.
        if (in.kind == Kind::LadderOff || jsonkeys::boolOf(in.cam1, "ladder_enabled").value_or(false))
            steps.push_back({"set_mode_ladder", {{"enabled", "false"}}});
    } else if (in.ladderKnown && !detail::ladderInactiveHere(in.primary) && ladderDiffers(in.primary, in.cam1)) {
        Fields fields = {{"enabled", boolText(in.ladderEnabled)}, {"ramp_ready", boolText(in.ladderRampReady)}};
        if (!in.ladderResolutions.empty()) fields["resolutions"] = in.ladderResolutions;
        steps.push_back({"set_mode_ladder", fields});
    }
    if (in.kind != Kind::Full) return steps;

    // 2. The camera keys of the tied controls and the always-shared ones. While the primary's ladder or override picks the
    // mode, no mode keys: cam1's ladder picks its own, and its check would refuse the primary's.
    const bool dropMode = in.ties.resolution && (primaryLadderActive || primaryOverride);
    Fields preset = commandclasses::presetKeysFor(in.primaryPreset, in.ties, commandclasses::PresetReach::Forward, dropMode);
    if (!preset.empty()) steps.push_back({"apply_preset", preset});

    // 3. The override's Clear HDR, when the primary has the override and cam1 does not.
    if (in.ties.resolution && primaryOverride && !cam1Override)
        steps.push_back({"set_clear_hdr", {{"enabled", boolText(jsonkeys::boolOf(in.primary, "clear_hdr").value_or(false))}}});
    return steps;
}

// ---- when to resync (§4.4) ------------------------------------------------------------------------------------------------

// One resync runs at the next moment both Cores are idle, after a trigger. Two differing compares in a row are a trigger;
// three resyncs in a row with no passing compare between them end in `refused`, and no trigger schedules another until a
// compare passes or the link comes back.
class Tracker {
public:
    static constexpr int kDiffsToResync = 2;
    static constexpr int kResyncCap = 3;

    void onLink()
    {
        resyncs_ = 0;
        diffStreak_ = 0;
        refused_ = false;
        scheduled_ = Kind::Full;
    }
    void onNonOkReply() { schedule(Kind::Full); }
    void onSatOutTake() { schedule(Kind::Full); }
    void onForwardedReconfigure() { schedule(Kind::Full); }   // set_record_format, apply_preset (CC:11213-11239)
    // A tie change. Untying `resolution` alone sends cam1 `set_mode_ladder enabled=false` and nothing else; tying any
    // control back runs a full resync, so cam1 takes cam0's value.
    void onTieChange(const Ties &from, const Ties &to)
    {
        if (from == to) return;
        const bool tiedAny = (!from.resolution && to.resolution) || (!from.iso && to.iso) || (!from.shutter && to.shutter) ||
                             (!from.fps && to.fps) || (!from.wb && to.wb);
        if (tiedAny) schedule(Kind::Full);
        else if (from.resolution && !to.resolution) schedule(Kind::LadderOff);
    }

    // A compare of cam1's get_state, taken only against a state sent after the last forward's reply. True when this pass
    // ended a `refused` state.
    bool onCompare(bool differs)
    {
        if (!differs) {
            const bool wasRefused = refused_;
            diffStreak_ = 0;
            resyncs_ = 0;
            refused_ = false;
            return wasRefused;
        }
        if (++diffStreak_ >= kDiffsToResync) schedule(Kind::Full);
        return false;
    }

    // At an idle moment for both Cores: the resync to run, if any.
    Kind take(bool bothIdle)
    {
        if (scheduled_ == Kind::None || !bothIdle) return Kind::None;
        if (resyncs_ >= kResyncCap) {
            refused_ = true;
            scheduled_ = Kind::None;
            return Kind::None;
        }
        ++resyncs_;
        diffStreak_ = 0;
        const Kind kind = scheduled_;
        scheduled_ = Kind::None;
        return kind;
    }
    // The resync could not go (a take is starting): it stays owed and does not count.
    void giveBack(Kind kind)
    {
        if (resyncs_ > 0) --resyncs_;
        if (kind == Kind::Full || scheduled_ == Kind::None) scheduled_ = kind;
    }

    bool refused() const { return refused_; }
    Kind scheduled() const { return scheduled_; }
    int resyncs() const { return resyncs_; }
    int diffStreak() const { return diffStreak_; }

private:
    void schedule(Kind kind)
    {
        if (refused_) return;
        if (kind == Kind::Full || scheduled_ == Kind::None) scheduled_ = kind;
    }
    Kind scheduled_ = Kind::None;
    int resyncs_ = 0;
    int diffStreak_ = 0;
    bool refused_ = false;
};

// A compare may only read a cam1 get_state sent after the last forward's reply (§4.4): an earlier one can show the value
// from before the setter.
inline bool compareEligible(long long pollSentMs, long long lastForwardReplyMs)
{
    return pollSentMs > lastForwardReplyMs;
}

// ---- the clamp rule (§4.4 Refusals and limits) -----------------------------------------------------------------------------

inline std::string numberText(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

// The fps a forward asked for, 0 when it asked for none.
inline double sentFps(std::string_view name, const Fields &fields)
{
    const char *key = name == "set_fps" ? "value" : name == "apply_preset" ? "cameraFps" : nullptr;
    if (!key) return 0;
    const auto it = fields.find(key);
    double value = 0;
    return it != fields.end() && detail::isNumber(it->second, &value) ? value : 0;
}

// cam1 clamps an fps above its mode's ceiling and still answers ok. `settings` is the settingsBody of the ok reply (the
// reply itself for set_fps, its "state" object for apply_preset). Returns cam1.message's text, or nullopt when cam1 holds
// the fps it was sent. The relay schedules nothing on it: the compare finds the difference and the cap ends the loop.
inline std::optional<std::string> clampMessage(double sent, std::string_view settings)
{
    const auto fps = jsonkeys::numberOf(settings, "fps");
    if (sent <= 0 || !fps || *fps >= sent - 0.0005) return std::nullopt;
    const auto max = jsonkeys::numberOf(settings, "fps_max");
    const std::string resolution = jsonkeys::stringOf(settings, "resolution").value_or("");
    return "CAM1 runs " + (resolution.empty() ? std::string("this mode") : resolution) + " at most at " +
           numberText(max && *max > 0 ? *max : *fps) + " fps.";
}

} // namespace apertar::resync
