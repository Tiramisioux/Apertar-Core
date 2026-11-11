#pragma once
// Command classes (dual-sensor design §4.2). Pure: std only, host-tested (command_classes_test.cpp).
//
// One list is both the secondary's allowlist and, from T61 on, the primary's forward list: a command that is not here
// stays refused on cam1 until someone adds it. The classes are the table's rows: Both (forwarded after the primary's
// ok), Coordinated (record_start / record_stop, §6), Each its own (reads). Everything else, the "Primary only" row
// included, is Primary.
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace apertar::commandclasses {

enum class Class { Primary, Both, Coordinated, EachItsOwn };

// What the secondary answers to a command outside the list, and to every POST on its web port.
inline constexpr const char *kSecondaryRefusal = "cam1 takes commands from the primary.";

namespace detail {
struct Entry {
    const char *name;   // exact name, or a prefix when `prefix` is set
    bool prefix;
};

inline constexpr Entry kBoth[] = {
    {"set_iso", false},          {"set_iso_auto", false},     {"set_shutter_", true},      {"set_exposure_", true},
    {"set_wb", false},           {"set_wb_auto", false},      {"set_fps", false},          {"set_resolution", false},
    {"set_record_format", false}, {"set_cdng_", true},        {"set_lossy_dng_ratio", false}, {"set_prores_", true},
    {"set_tuning_file", false},  {"set_raw_bit_depth", false}, {"set_clear_hdr", false},   {"set_hdr_", true},
    {"set_h264_resolution", false}, {"set_timecode_mode", false}, {"set_conform_fps", false},
    {"set_frames_per_folder", false}, {"set_fps_phase_lock", false}, {"set_fps_speed_ramp", false}, {"set_fps_speed_ramp_", true},
    {"set_af_", true},           {"set_mode_ladder", false},  {"apply_preset", false},     {"enter_standby", false},
    {"wake", false},
};
inline constexpr Entry kCoordinated[] = {{"record_start", false}, {"record_stop", false}};
inline constexpr Entry kEachItsOwn[] = {
    {"get_state", false}, {"ping", false}, {"help", false}, {"list_", true},
    {"get_mode_ladder", false}, {"preview_subscribe", false}, {"preview_clip_name", false},
};

template <size_t N>
inline bool listed(const Entry (&entries)[N], std::string_view name)
{
    for (const Entry &entry : entries) {
        const std::string_view listedName(entry.name);
        if (entry.prefix ? (name.size() > listedName.size() && name.substr(0, listedName.size()) == listedName)
                         : name == listedName)
            return true;
    }
    return false;
}
} // namespace detail

inline Class classOf(std::string_view name)
{
    if (detail::listed(detail::kBoth, name)) return Class::Both;
    if (detail::listed(detail::kCoordinated, name)) return Class::Coordinated;
    if (detail::listed(detail::kEachItsOwn, name)) return Class::EachItsOwn;
    return Class::Primary;
}

// The secondary accepts the Both, Coordinated and Each-its-own rows.
inline bool allowedOnSecondary(std::string_view name)
{
    return classOf(name) != Class::Primary;
}


// ---- T61: what the relay does with a command ---------------------------------------------------------------------------

// The exposure setters (§4.2): the ones a cam1-only take or a busy cam1 still lets through.
inline bool isExposureSetter(std::string_view name)
{
    return name == "set_iso" || name == "set_iso_auto" || name == "set_wb" || name == "set_wb_auto" ||
           (name.size() > 12 && name.substr(0, 12) == "set_shutter_") || (name.size() > 13 && name.substr(0, 13) == "set_exposure_");
}

// The control a setter belongs to, for the tie texts (amendment A2): the five tie names, plus the always-shared setters.
enum class Control { None, Resolution, Iso, Shutter, Fps, Wb, Shared };

inline Control controlOf(std::string_view name)
{
    if (name == "set_iso" || name == "set_iso_auto" || (name.size() > 13 && name.substr(0, 13) == "set_exposure_")) return Control::Iso;
    if (name.size() > 12 && name.substr(0, 12) == "set_shutter_") return Control::Shutter;
    if (name == "set_fps" || name == "set_conform_fps") return Control::Fps;
    if (name == "set_wb" || name == "set_wb_auto") return Control::Wb;
    if (name == "set_fps_phase_lock" || name == "set_fps_speed_ramp" || (name.size() > 18 && name.substr(0, 18) == "set_fps_speed_ramp") ||
        name == "set_timecode_mode" || name == "set_frames_per_folder" || name == "enter_standby" || name == "wake")
        return Control::Shared;
    // The Recording group (§4.4): resolution, format and its fields, Clear HDR, bit depth, tuning, the HDR knobs, h264,
    // prores, autofocus and the ladder.
    if (name == "set_resolution" || name == "set_record_format" || name == "set_lossy_dng_ratio" || name == "set_tuning_file" ||
        name == "set_raw_bit_depth" || name == "set_clear_hdr" || name == "set_h264_resolution" || name == "set_mode_ladder" ||
        (name.size() > 9 && name.substr(0, 9) == "set_cdng_") || (name.size() > 11 && name.substr(0, 11) == "set_prores_") ||
        (name.size() > 8 && name.substr(0, 8) == "set_hdr_") || (name.size() > 7 && name.substr(0, 7) == "set_af_"))
        return Control::Resolution;
    return Control::None;
}

inline const char *controlText(Control control)
{
    switch (control) {
    case Control::Resolution: return "resolution";
    case Control::Iso: return "ISO";
    case Control::Shutter: return "shutter";
    case Control::Fps: return "frame rate";
    case Control::Wb: return "white balance";
    default: return "";
    }
}

// The commands that take a `camera` field (§4.2): the camera setters, apply_preset and the ladder pair. On any other
// command the field means nothing and is ignored, as today.
inline bool takesCameraField(std::string_view name)
{
    return classOf(name) == Class::Both || name == "get_mode_ladder";
}

// The refusal for a `camera` field, or nullopt to carry on. `cam1Configured` is the relay's configured test (§4.1).
// T61 part 1: a `camera=cam1` setter is always refused, because every control is treated as tied. Part 2 replaces the
// last branch with the tie lookup (cam1Settings, amendment A2) and lets an untied control through.
inline std::optional<std::string> cameraFieldRefusal(std::string_view name, std::string_view camera, bool cam1Configured)
{
    if (!takesCameraField(name) || camera.empty() || camera == "cam0") return std::nullopt;
    if (camera != "cam1") return std::string("camera must be cam0 or cam1.");
    if (!cam1Configured) return std::string("No CAM1 is configured.");
    const Control control = controlOf(name);
    if (control == Control::Shared) return std::string("Both cameras share this setting.");
    if (control == Control::None)   // apply_preset, get_mode_ladder: part 2 gives them their own rules
        return std::string("CAM1 follows CAM0. Untie a control in System > Dual Sensor first.");
    return std::string("CAM1 follows CAM0 for ") + controlText(control) + ". Untie it in System > Dual Sensor first.";
}

// §4.3, "cam1 records alone": the primary accepts reads, the exposure setters, display keys, user_activity and
// record_stop. Everything that starts a capture or reconfigures cam0 is refused "CAM1 is recording.".
inline bool refusedWhileCam1Records(std::string_view name)
{
    if (name == "record_start" || name == "capture_photo" || name == "timelapse_start") return true;
    return classOf(name) == Class::Both && !isExposureSetter(name);
}

// What goes to cam1 after the primary's ok reply (§4.1 The hook). The seam for part 2: the tie filter goes here, and
// nowhere else. Part 1 treats everything as Unison: the whole Both row is forwarded.
struct ForwardPlan {
    bool send = false;
    std::map<std::string, std::string> fields;   // without id, cmd and camera
};

inline ForwardPlan planForward(std::string_view name, const std::map<std::string, std::string> &fields, std::string_view primaryReply)
{
    ForwardPlan plan;
    if (classOf(name) != Class::Both) return plan;
    if (primaryReply.rfind("{\"ok\":true", 0) != 0) return plan;
    plan.fields = fields;
    for (const char *drop : {"id", "cmd", "camera"}) plan.fields.erase(drop);
    if (name == "enter_standby") {
        // Only when the primary did enter standby, and without the idle timer's idle_timeout (§4.2).
        if (primaryReply.find("\"standby\":true") == std::string_view::npos) return plan;
        plan.fields.erase("idle_timeout");
    } else if (name == "set_mode_ladder") {
        // The aspects filter is each Core's own (§4.4): it is never forwarded, and a call left with nothing else is not
        // sent at all. A dry run only validates.
        plan.fields.erase("aspects");
        const auto dry = plan.fields.find("dry_run");
        if (dry != plan.fields.end() && (dry->second == "true" || dry->second == "1")) return plan;
        plan.fields.erase("dry_run");
        if (plan.fields.empty()) return plan;
    }
    plan.send = true;
    return plan;
}

} // namespace apertar::commandclasses
