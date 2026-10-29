#pragma once
// One vocabulary for hardware actions: the aliases the three input drivers accept, the parameterised
// tokens ApertarCoreApp parses, and the labels/groups list_actions serves. Pure: no I/O, no libcamera.
#include "hardware/ButtonActionList.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace apertar::hardware::actions {

enum Source : unsigned { Button = 1, Rotation = 2, Both = 3 };
enum class Arg { None, Iso, Fps, Kelvin, ShutterAngle, ShutterSpeed, ShutterSeconds, Ev, Resolution };

struct Spec {
    std::string_view id;      // canonical token written to the file, or a family id for Arg != None
    std::string_view group;
    std::string_view label;
    std::string_view aliases; // comma separated, every spelling the old normalizers accept (id included)
    unsigned sources = Both;
    bool ui = false;          // handled by the touch UI through a hardware_action event
    Arg arg = Arg::None;
    std::string_view token = {}; // file form of a parameterised family, {} = the value
};

inline constexpr Spec kSpecs[] = {
    {"capture", "Record", "Record or photo (main button)", "record,capture,camera_button,take"},
    {"record_toggle", "Record", "Start / stop recording", "record_toggle,toggle_record,rec"},
    {"photo", "Record", "Take a photo", "still,photo,take_photo"},
    {"timelapse", "Record", "Start / stop timelapse", "timelapse,time_lapse"},
    {"still_mode", "Record", "Still mode on / off", "still_mode,photo_mode"},
    {"iso", "ISO", "ISO: next, wraps", "iso,iso_cycle"},
    {"iso_up", "ISO", "ISO up", "iso_up,iso_next,iso_plus"},
    {"iso_down", "ISO", "ISO down", "iso_down,iso_prev,iso_previous,iso_minus"},
    {"iso_value", "ISO", "Set ISO", "", Both, false, Arg::Iso, "iso{}"},
    {"shutter", "Shutter", "Shutter: next, wraps", "shutter,shutter_cycle"},
    {"shutter_up", "Shutter", "Shutter up", "shutter_up,shutter_next,shutter_plus"},
    {"shutter_down", "Shutter", "Shutter down", "shutter_down,shutter_prev,shutter_previous,shutter_minus"},
    {"shutter_angle_value", "Shutter", "Set shutter angle", "", Both, false, Arg::ShutterAngle, "shutter_angle{}"},
    {"shutter_speed_value", "Shutter", "Set shutter speed 1/x", "", Both, false, Arg::ShutterSpeed, "shutter1/{}"},
    {"shutter_seconds_value", "Shutter", "Set exposure in seconds", "", Both, false, Arg::ShutterSeconds, "shutter{}s"},
    {"fps", "Frame rate", "FPS: next, wraps", "fps,fps_cycle"},
    {"fps_up", "Frame rate", "FPS up", "fps_up,fps_next,fps_plus"},
    {"fps_down", "Frame rate", "FPS down", "fps_down,fps_prev,fps_previous,fps_minus"},
    {"fps_value", "Frame rate", "Set FPS", "", Both, false, Arg::Fps, "fps{}"},
    {"wb", "White balance", "WB: next, wraps", "wb,white_balance,whitebalance,wb_cycle,white_balance_cycle"},
    {"wb_up", "White balance", "WB up", "wb_up,white_balance_up,whitebalance_up,wb_next,wb_plus"},
    {"wb_down", "White balance", "WB down", "wb_down,white_balance_down,whitebalance_down,wb_prev,wb_previous,wb_minus"},
    {"wb_value", "White balance", "Set WB (K)", "", Both, false, Arg::Kelvin, "wb{}"},
    {"ev_up", "Exposure", "EV +1/3", "ev_up"},
    {"ev_down", "Exposure", "EV -1/3", "ev_down"},
    {"ev_value", "Exposure", "Set EV", "", Both, false, Arg::Ev, "ev{}"},
    {"resolution", "Resolution", "Resolution: next (IMX585: Clear HDR toggle)", "resolution,resolution_cycle,res,res_cycle"},
    {"resolution_up", "Resolution", "Resolution up", "resolution_up,resolution_next,resolution_plus,res_up,res_next,res_plus"},
    {"resolution_down", "Resolution", "Resolution down",
     "resolution_down,resolution_prev,resolution_previous,resolution_minus,res_down,res_prev,res_previous,res_minus"},
    {"resolution_value", "Resolution", "Set resolution", "", Both, false, Arg::Resolution, "resolution{}"},
    {"focus_peaking", "Monitoring", "Focus peaking on / off", "focus_peaking,peaking"},
    {"zebra", "Monitoring", "Zebra on / off", "zebra,zebras"},
    {"false_color", "Monitoring", "False color on / off", "false_color,falsecolour"},
    {"guides", "Monitoring", "Guides on / off", "guides"},
    {"center_marker", "Monitoring", "Center marker on / off", "center_marker,centre_marker"},
    {"grayscale", "Monitoring", "Grayscale on / off", "grayscale,greyscale"},
    {"smpte", "Monitoring", "Color bars on / off", "smpte,smpte_bars,color_bars,colour_bars"},
    {"external_monitor", "Monitoring", "External monitor on / off", "external_monitor,external_monitoring,hdmi"},
    {"date_time_overlay", "Monitoring", "Date and time overlay on / off", "date_time,date_time_overlay"},
    {"audio_meter", "Monitoring", "Audio meter on / off", "audio_meter,vu_meter,vumeter"},
    {"zoom_toggle", "Screens", "Preview zoom", "zoom,zoom_toggle,preview_zoom", Both, true},
    {"settings", "Screens", "Open settings", "settings,settings_page", Both, true},
    {"clip_browser", "Screens", "Open clip browser", "clip_browser,browser,playback", Both, true},
    {"quick_menu", "Screens", "Open quick menu", "quick_menu,quickmenu", Both, true},
    {"eject", "Media", "Eject media", "eject,unmount,eject_media,unmount_media"},
    {"standby", "System", "Standby (button only, keep it last)", "standby,standby_toggle", Button},
    {"run_script", "Scripts", "Run all scripts", "run_script,script"},
    {"run_script1", "Scripts", "Run script 1", "run_script1,run_script_1,script1,script_1"},
    {"run_script2", "Scripts", "Run script 2", "run_script2,run_script_2,script2,script_2"},
    {"run_script3", "Scripts", "Run script 3", "run_script3,run_script_3,script3,script_3"},
    {"run_script4", "Scripts", "Run script 4", "run_script4,run_script_4,script4,script_4"},
    {"run_script5", "Scripts", "Run script 5", "run_script5,run_script_5,script5,script_5"},
};

inline std::string normalizeToken(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n\f\v");
    std::string token(text.substr(first, last - first + 1));
    for (char &ch : token) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-' || ch == ' ' || ch == '.') ch = '_';
    }
    return token;
}

inline bool listHas(std::string_view list, std::string_view item)
{
    size_t start = 0;
    while (start <= list.size()) {
        const auto end = list.find(',', start);
        if (list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start) == item)
            return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

inline const Spec *findSpec(std::string_view id)
{
    for (const auto &spec : kSpecs)
        if (spec.id == id) return &spec;
    return nullptr;
}

// Alias -> canonical; unknown names pass through. The input drivers share this one.
inline std::string canonicalAction(std::string_view text)
{
    const std::string action = normalizeToken(text);
    if (action.empty() || action == "none" || action == "off" || action == "disabled") return {};
    if (isOutputAction(action)) return {};
    if (const auto script = normalizeScriptAction(action); !script.empty()) return script;
    for (const auto &spec : kSpecs)
        if (spec.arg == Arg::None && listHas(spec.aliases, action)) return std::string(spec.id);
    return action;
}

// The parameterised forms, as ApertarCoreApp::handleSingleHardwareInputAction parses them.
inline std::optional<std::string> suffixAfter(const std::string &action, const std::vector<std::string_view> &prefixes)
{
    for (const auto prefix : prefixes) {
        if (action.rfind(prefix, 0) != 0) continue;
        std::string suffix = action.substr(prefix.size());
        while (!suffix.empty() && suffix.front() == '_') suffix.erase(suffix.begin());
        if (!suffix.empty()) return suffix;
    }
    return std::nullopt;
}

inline std::optional<double> number(std::string text)
{
    std::replace(text.begin(), text.end(), '_', '.');
    if (text.empty()) return std::nullopt;
    char *end = nullptr;
    errno = 0;   // like App's std::stod: out of range is no number. Unlike it, inf and nan are refused too
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(value)) return std::nullopt;
    return value;
}

inline std::optional<double> numberAfter(const std::string &action, const std::vector<std::string_view> &prefixes)
{
    auto suffix = suffixAfter(action, prefixes);
    if (!suffix) return std::nullopt;
    if (!suffix->empty() && (suffix->back() == 'k' || suffix->back() == 'p')) suffix->pop_back();
    return number(*suffix);
}

struct Classified {
    bool known = false;
    std::string id;     // spec id
    std::string value;  // the argument as text, for a parameterised family
    std::string note;   // what Core will really do, when it surprises
};

inline std::string compact(double value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

// Is a single (already canonical) token something Core's dispatcher acts on?
inline Classified classify(const std::string &token)
{
    Classified out;
    if (token.empty()) return out;
    if (const auto *spec = findSpec(token); spec && spec->arg == Arg::None) {
        out.known = true;
        out.id = token;
        return out;
    }
    auto family = [&](std::string_view id, double value, std::string note = {}) {
        out.known = true;
        out.id = std::string(id);
        out.value = compact(value);
        out.note = std::move(note);
        return out;
    };
    if (const auto ev = numberAfter(token, {"evm", "ev"}))
        return family("ev_value", token.rfind("evm", 0) == 0 ? -*ev : *ev);
    if (const auto fps = numberAfter(token, {"fps"}); fps && *fps > 0.0) return family("fps_value", *fps);
    if (auto res = suffixAfter(token, {"resolution", "res"})) {
        std::replace(res->begin(), res->end(), '*', 'x');
        if (res->find('x') == std::string::npos) {
            if (const auto underscore = res->find('_'); underscore != std::string::npos) (*res)[underscore] = 'x';
        }
        const auto x = res->find('x');
        if (x != std::string::npos && x > 0 && x + 1 < res->size() &&
            std::all_of(res->begin(), res->begin() + static_cast<long>(x), ::isdigit) &&
            std::all_of(res->begin() + static_cast<long>(x) + 1, res->end(), ::isdigit)) {
            out.known = true;
            out.id = "resolution_value";
            out.value = *res;
            out.note = "not checked against this sensor's modes";
            return out;
        }
    }
    if (const auto iso = numberAfter(token, {"iso"}); iso && *iso > 0.0) return family("iso_value", std::round(*iso));
    if (!token.empty() && token.back() == 's') {
        if (auto seconds = suffixAfter(token, {"shutter_seconds", "shutterseconds", "shutter_speed", "shutterspeed",
                                                "shutter_s", "shutter"})) {
            seconds->pop_back();
            if (const auto value = number(*seconds); value && *value > 0.0 && *value <= 30.0)
                return family("shutter_seconds_value", *value);
        }
    }
    if (const auto angle = numberAfter(token, {"shutter_angle", "shutterangle"}); angle && *angle > 0.0)
        return family("shutter_angle_value", *angle);
    auto denominator = [&](const std::vector<std::string_view> &prefixes) -> std::optional<double> {
        auto suffix = suffixAfter(token, prefixes);
        if (!suffix) return std::nullopt;
        if (suffix->rfind("1/", 0) == 0 || suffix->rfind("1_", 0) == 0) suffix->erase(0, 2);
        return number(*suffix);
    };
    if (const auto speed = denominator({"shutter_speed", "shutterspeed", "shutter_s"}); speed && *speed > 0.0)
        return family("shutter_speed_value", *speed);
    if (const auto value = denominator({"shutter"}); value && *value > 0.0) {
        const auto suffix = suffixAfter(token, {"shutter"});
        const bool speed = suffix && (suffix->rfind("1/", 0) == 0 || suffix->rfind("1_", 0) == 0);
        return speed ? family("shutter_speed_value", *value)
                     : family("shutter_angle_value", *value, "an angle in video; 1/x s in photo mode or timelapse");
    }
    if (const auto kelvin = numberAfter(token, {"wb", "white_balance", "whitebalance"}); kelvin && *kelvin > 0.0) {
        const double clamped = std::clamp(std::round(*kelvin), 2500.0, 8000.0);
        return family("wb_value", clamped,
                      clamped != std::round(*kelvin) ? "Core clamps " + compact(*kelvin) + " K to " + compact(clamped) + " K" : "");
    }
    return out;
}

// Edit distance for "did you mean".
inline size_t distance(std::string_view a, std::string_view b)
{
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

inline std::string suggestion(std::string_view token)
{
    std::string best;
    size_t bestDistance = 3;
    for (const auto &spec : kSpecs) {
        if (spec.arg != Arg::None) continue;
        if (const size_t d = distance(token, spec.id); d < bestDistance) {
            bestDistance = d;
            best = std::string(spec.id);
        }
    }
    return best;
}

// What a knob turn should carry. Stepping actions move one step per detent;
// the "turn" ones start or stop something on every detent, which is almost never meant.
inline bool isStepping(std::string_view id)
{
    for (const std::string_view family : {"iso", "shutter", "fps", "wb", "ev", "resolution"})
        for (const std::string_view dir : {"_up", "_down"})
            if (id.size() == family.size() + dir.size() && id.substr(0, family.size()) == family &&
                id.substr(family.size()) == dir)
                return true;
    return false;
}

inline bool warnOnTurn(std::string_view id)
{
    return id == "capture" || id == "record_toggle" || id == "photo" || id == "timelapse" || id == "still_mode" ||
        id == "eject";
}

// The analog targets (Grove's normalizer also maps none/off/disabled to "no target"; App only sees its result).
inline constexpr std::string_view kAnalogTargets[] = {"iso", "fps", "resolution", "wb", "shutter",
    "focus_peaking_threshold", "zebra_threshold", "input_volume", "headphone_volume", "camera_controls_opacity"};

inline std::string canonicalAnalogTarget(std::string_view text)
{
    const std::string target = normalizeToken(text);
    if (target.empty() || target == "none" || target == "off" || target == "disabled") return {};
    if (target == "white_balance" || target == "whitebalance") return "wb";
    if (target == "focus_peaking" || target == "focus_peaking_value" || target == "focus") return "focus_peaking_threshold";
    if (target == "zebra" || target == "zebras") return "zebra_threshold";
    if (target == "volume" || target == "input_gain") return "input_volume";
    if (target == "headphone" || target == "headphones") return "headphone_volume";
    if (target == "opacity" || target == "controls_opacity") return "camera_controls_opacity";
    if (target == "res") return "resolution";
    return target;
}

// Trigger events. The Outputs card reads this list.
struct TriggerEventSpec {
    std::string_view id;
    std::string_view label;
    std::string_view aliases;
};
inline constexpr TriggerEventSpec kTriggerEvents[] = {
    {"recording_start", "Record start", "record,record_start,recording,recording_start,rec_start"},
    {"still", "Photo", "still,photo,photo_capture,capture_photo,still_capture"},
    {"timelapse_start", "Timelapse start", "timelapse,timelapse_start,time_lapse_start"},
    {"timelapse_frame", "Timelapse frame", "timelapse_frame,timelapse_shot,timelapse_still,time_lapse_frame"},
};

inline std::string canonicalTriggerEvent(std::string_view text)
{
    const std::string token = normalizeToken(text);
    for (const auto &event : kTriggerEvents)
        if (listHas(event.aliases, token)) return std::string(event.id);
    return {};
}

inline std::string catalogJson()
{
    std::ostringstream out;
    out << "{\"actions\":[";
    bool first = true;
    for (const auto &spec : kSpecs) {
        out << (first ? "" : ",") << "{\"id\":\"" << spec.id << "\",\"group\":\"" << spec.group << "\",\"label\":\""
            << spec.label << "\",\"button\":" << ((spec.sources & Button) ? "true" : "false")
            << ",\"rotation\":" << ((spec.sources & Rotation) ? "true" : "false")
            << ",\"ui\":" << (spec.ui ? "true" : "false")
            << ",\"stepping\":" << (isStepping(spec.id) ? "true" : "false");
        if (spec.arg != Arg::None) out << ",\"token\":\"" << spec.token << "\"";
        out << "}";
        first = false;
    }
    out << "],\"analog_targets\":[";
    for (size_t i = 0; i < std::size(kAnalogTargets); ++i) out << (i ? "," : "") << "\"" << kAnalogTargets[i] << "\"";
    out << "],\"trigger_events\":[";
    for (size_t i = 0; i < std::size(kTriggerEvents); ++i)
        out << (i ? "," : "") << "{\"id\":\"" << kTriggerEvents[i].id << "\",\"label\":\"" << kTriggerEvents[i].label << "\"}";
    out << "],\"max_per_mapping\":" << maxButtonActions << "}";
    return out.str();
}

} // namespace apertar::hardware::actions
