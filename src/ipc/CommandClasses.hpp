#pragma once
// Command classes (dual-sensor design §4.2). Pure: std only, host-tested (command_classes_test.cpp).
//
// One list is both the secondary's allowlist and, from T61 on, the primary's forward list: a command that is not here
// stays refused on cam1 until someone adds it. The classes are the table's rows: Both (forwarded after the primary's
// ok), Coordinated (record_start / record_stop, §6), Each its own (reads). Everything else, the "Primary only" row
// included, is Primary.
#include <cstddef>
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

} // namespace apertar::commandclasses
