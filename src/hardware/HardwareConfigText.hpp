#pragma once
// The text of ~/apertar-hardware.conf as lines, the canonical names Core writes, key-level edits that
// keep every other byte, the per-key merge check, a line diff, and the validator. Pure: no I/O.
#include "hardware/ActionCatalog.hpp"
#include "util/BufferBudget.hpp"
#include "util/StoragePreroll.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace apertar::hwconf {

using hardware::actions::normalizeToken;

// Canonical order = the order Core inserts a new section in (pcf8574 before trigger/LED: they inherit its bus).
// Each section carries the comment rule its reader applies, the bus and the "on"
// default the reader uses when the file says nothing, and whether easy mode may edit it.
enum class ValueRule {
    FirstMark,     // inputs and IMU: the value ends at the first # or ; (GpioInput.cpp, Mpu6050RecordingSession.cpp)
    SpacedMark,    // trigger and tally: only a # or ; at the start or after whitespace (TriggerOutput.cpp, RecordingIndicatorOutput.cpp)
    IntegerStrict, // ina219, cfexpress_hat: Core's integers take no comment (ApertarCoreApp.cpp); enabled= cuts at the first mark
    Raw,           // storage: Core's Python helper (configparser) keeps the whole value
    Quoted,        // run_script: a quoted path ends at its quote, else at the first mark (HardwareScriptRunner.cpp)
    Other,         // display, network, camera: the touch UI's QSettings and others
};

struct SectionInfo {
    std::string_view name;
    std::string_view aliases;
    bool device;              // has bus/address and an "enabled" meaning
    ValueRule rule = ValueRule::FirstMark;
    int defaultBus = -1;      // the bus the reader uses with no bus= key (-1: none, or the IMU's auto-probe)
    bool onByDefault = false; // on when the section exists and says no enabled=
    bool onWithoutSection = false; // on with no section at all (INA219)
    bool easy = true;         // set.<section>.<key> allowed (text mode only otherwise)
};
inline constexpr SectionInfo kSections[] = {
    {"network", "network", false, ValueRule::Other, -1, false, false, false},
    {"run_script", "run_script", false, ValueRule::Quoted, -1, false, false, false},
    {"ina219", "ina219", true, ValueRule::IntegerStrict, 4, true, true, true},
    {"pcf8574", "pcf8574,pcf8574_buttons", true, ValueRule::FirstMark, 1, true, false, true},
    {"trigger_output", "trigger_output,trigger,capture_trigger,hotshoe", false, ValueRule::SpacedMark, -1, false, false, true},
    {"recording_indicator", "recording_indicator,rec_indicator,record_led,recording_led,tally_led", false, ValueRule::SpacedMark, -1, false, false, true},
    {"neorotary4", "neorotary4,neo_rotary4,neo_rotary_4", true, ValueRule::FirstMark, 1, true, false, true},
    {"gpio_buttons", "gpio_buttons,gpio,grove_digital,grove_buttons", false, ValueRule::FirstMark, -1, false, false, true},
    {"gpio_encoders", "gpio_encoders,grove_encoders", false, ValueRule::FirstMark, -1, false, false, true},
    {"gpio_switches", "gpio_switches", false, ValueRule::FirstMark, -1, false, false, true},
    {"grove_analog", "grove_analog,grove_adc,analog_controls", true, ValueRule::FirstMark, 1, true, false, true},
    {"camera", "camera", false, ValueRule::Other, -1, false, false, false},
    {"imu", "imu", false, ValueRule::FirstMark, -1, true, false, true},
    {"mpu6050", "mpu6050", true, ValueRule::FirstMark, -1, true, false, true},
    {"bmi160", "bmi160", true, ValueRule::FirstMark, -1, true, false, true},
    {"bmi270", "bmi270", true, ValueRule::FirstMark, -1, true, false, true},
    {"icm42688p", "icm42688p", true, ValueRule::FirstMark, -1, true, false, true},
    {"gyro", "gyro", true, ValueRule::FirstMark, -1, true, false, true},
    {"cfexpress_hat", "cfexpress_hat", true, ValueRule::IntegerStrict, 10, true, false, true},
    {"storage", "storage", false, ValueRule::Raw, -1, false, false, false},
    {"media", "media", false, ValueRule::Raw, -1, false, false, false},
    {"display", "display", false, ValueRule::Other, -1, false, false, false},
};

inline const SectionInfo *sectionInfo(std::string_view name)
{
    for (const auto &info : kSections)
        if (info.name == name) return &info;
    return nullptr;
}

inline ValueRule valueRule(std::string_view section)
{
    const auto *info = sectionInfo(section);
    return info ? info->rule : ValueRule::Other;
}

inline int sectionOrder(std::string_view name)
{
    for (size_t i = 0; i < std::size(kSections); ++i)
        if (kSections[i].name == name) return static_cast<int>(i);
    return -1;
}

inline std::string canonicalSection(std::string_view raw, bool *known = nullptr)
{
    const std::string token = normalizeToken(raw);
    for (const auto &info : kSections) {
        if (hardware::actions::listHas(info.aliases, token)) {
            if (known) *known = true;
            return std::string(info.name);
        }
    }
    if (known) *known = false;
    return token;
}

inline bool allDigits(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

inline std::string stripSuffix(std::string key, std::string_view suffix)
{
    if (key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
        key.resize(key.size() - suffix.size());
    return key;
}

// Gesture suffixes (T44): written by Core as <control>_hold / <control>_double. Old Core ignores them.
inline std::string splitGesture(std::string &key)
{
    for (const std::string_view gesture : {"_single", "_double", "_triple", "_hold"}) {
        if (key.size() > gesture.size() && key.compare(key.size() - gesture.size(), gesture.size(), gesture) == 0) {
            key.resize(key.size() - gesture.size());
            return std::string(gesture);
        }
    }
    return {};
}

inline std::string canonicalEncoderKey(const std::string &key)
{
    for (const std::string_view prefix : {"encoder", "enc", "rotary"}) {
        if (key.rfind(prefix, 0) != 0) continue;
        std::string rest = key.substr(prefix.size());
        if (!rest.empty() && rest.front() == '_') rest.erase(rest.begin());
        if (rest.size() < 2 || !std::isdigit(static_cast<unsigned char>(rest.front()))) continue;
        const char index = rest.front();
        rest.erase(rest.begin());
        if (!rest.empty() && rest.front() == '_') rest.erase(rest.begin());
        std::string gesture = splitGesture(rest);
        std::string field = rest;
        static const std::pair<std::string_view, std::string_view> aliases[] = {
            {"gpio_a", "a"}, {"pin_a", "a"}, {"clk", "a"}, {"gpio_b", "b"}, {"pin_b", "b"}, {"dt", "b"},
            {"clockwise", "cw"}, {"right", "cw"}, {"up", "cw"}, {"plus", "cw"},
            {"counter_clockwise", "ccw"}, {"counterclockwise", "ccw"}, {"left", "ccw"}, {"down", "ccw"}, {"minus", "ccw"},
            {"button_gpio", "press_gpio"}, {"push_gpio", "press_gpio"}, {"switch_gpio", "press_gpio"}, {"sw", "press_gpio"},
            {"button", "press"}, {"push", "press"}, {"click", "press"},
            {"invert_direction", "invert"}, {"reverse_direction", "invert"},
            {"steps", "steps_per_event"}, {"detent_steps", "steps_per_event"},
        };
        for (const auto &[from, to] : aliases)
            if (field == from) field = std::string(to);
        if (!gesture.empty() && field == "press") field.clear();      // encoder2_hold = the push button's hold
        return "encoder" + std::string(1, index) + (field.empty() ? "" : "_" + field) + gesture;
    }
    return {};
}

// The one spelling Core writes for a key, given its section. Aliases are read, never written.
inline std::string canonicalKey(std::string_view section, std::string_view raw)
{
    std::string key = normalizeToken(raw);
    if (section == "grove_analog") {
        if (key == "channel" || key == "i2c_bus") return "bus";
        for (const std::string_view prefix : {"channel", "chan", "ch", "analog", "a"}) {
            if (key.rfind(prefix, 0) == 0 && key.size() > prefix.size() &&
                std::isdigit(static_cast<unsigned char>(key[prefix.size()])))
                return "a" + key.substr(prefix.size());
        }
    }
    if (key == "channel" || key == "i2c_bus" || key == "bus_number" || key == "pcf_bus") return "bus";
    if (key == "addr" || key == "pcf_address") return "address";
    if (key == "poll_interval_ms") return "poll_ms";
    if (key == "gpiochip" || key == "gpio_chip") return "chip";
    if (section == "neorotary4" && (key == "invert" || key == "reverse_direction")) return "invert_direction";
    if (section == "trigger_output" || section == "recording_indicator") {
        if (key == "type" || key == "backend" || key == "output") return "target";
        if (key == "gpio" || key == "gpio_line") return "line";
        if (key == "gpio_open_drain") return "open_drain";
        if (key == "pcf8574_pin") return "pcf_pin";
        if (key == "duration_ms") return "pulse_ms";
        if (key == "event") return "events";
        return key;
    }
    if (section == "neorotary4" || section == "gpio_encoders") {
        if (const auto encoder = canonicalEncoderKey(key); !encoder.empty()) return encoder;
        return key;
    }
    if (section == "gpio_buttons") {
        if (key == "bias") return "pull";
        std::string pin = stripSuffix(stripSuffix(key, "_action"), "_function");
        const std::string gesture = splitGesture(pin);
        for (const std::string_view prefix : {"grove_d_", "grove_d", "gpio_", "gpio", "pin_", "pin", "bcm_", "bcm", "d_", "d"}) {
            if (pin.rfind(prefix, 0) == 0 && allDigits(std::string_view(pin).substr(prefix.size())))
                return "d" + std::to_string(std::stoi(pin.substr(prefix.size()))) + gesture;
        }
        return key;
    }
    if (section == "pcf8574") {
        std::string pin = stripSuffix(stripSuffix(key, "_action"), "_function");
        const std::string gesture = splitGesture(pin);
        for (const std::string_view prefix : {"pin_", "pin", "gpio_", "gpio", "p"}) {
            if (pin.rfind(prefix, 0) == 0 && allDigits(std::string_view(pin).substr(prefix.size())) &&
                (prefix != "p" || pin.size() == 2))
                return "pin" + std::to_string(std::stoi(pin.substr(prefix.size()))) + gesture;
        }
        return key;
    }
    return key;
}

enum class Kind { Blank, Comment, Section, Key, Junk };

struct Line {
    std::string raw;
    Kind kind = Kind::Blank;
    size_t number = 0;          // 1-based
    std::string section;        // canonical name of the enclosing section ("" before the first)
    bool knownSection = false;
    std::string key;            // canonical
    std::string value;          // trimmed, inline comment cut off
    std::string comment;        // the inline comment, if any (cut by the section's ValueRule)
    bool headerTrailing = false;// a section header with text after ']'
    bool headerTrailingComment = false; // ... and that text is a # or ; comment
};

struct Document {
    std::vector<Line> lines;
    bool finalNewline = true;
};

inline std::string trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n\f\v");
    return std::string(text.substr(first, last - first + 1));
}

// Where the section's reader ends a value IntegerStrict and Raw cut at the first mark too, so
// line.value is what the author meant; the validator then says that Core's reader keeps its default instead.
inline size_t commentMark(ValueRule rule, const std::string &value)
{
    if (rule == ValueRule::SpacedMark) {
        for (size_t i = 0; i < value.size(); ++i)
            if ((value[i] == '#' || value[i] == ';') && (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))))
                return i;
        return std::string::npos;
    }
    if (rule == ValueRule::Quoted) {
        const auto first = value.find_first_not_of(" \t");
        if (first != std::string::npos && (value[first] == '\'' || value[first] == '"')) {
            const auto close = value.find(value[first], first + 1);
            return close == std::string::npos ? std::string::npos : value.find_first_of("#;", close + 1);
        }
    }
    return value.find_first_of("#;");
}

inline Document parse(const std::string &text)
{
    Document doc;
    doc.finalNewline = text.empty() || text.back() == '\n';
    std::string current;
    bool known = false;
    size_t start = 0;
    size_t number = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        Line line;
        line.raw = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        line.number = ++number;
        const std::string t = trim(line.raw);
        if (t.empty()) {
            line.kind = Kind::Blank;
        } else if (t[0] == '#' || t[0] == ';') {
            line.kind = Kind::Comment;
        } else if (t[0] == '[' && t.find(']') != std::string::npos) {
            line.kind = Kind::Section;
            const auto close = t.find(']');
            current = canonicalSection(t.substr(1, close - 1), &known);
            const std::string trailing = trim(std::string_view(t).substr(close + 1));
            line.headerTrailing = !trailing.empty();
            line.headerTrailingComment = !trailing.empty() && (trailing[0] == '#' || trailing[0] == ';');
        } else if (const auto eq = t.find('='); eq != std::string::npos) {
            line.kind = Kind::Key;
            line.key = canonicalKey(current, t.substr(0, eq));
            std::string value = t.substr(eq + 1);
            if (const auto mark = commentMark(valueRule(current), value); mark != std::string::npos) {
                line.comment = trim(std::string_view(value).substr(mark));
                value.resize(mark);
            }
            line.value = trim(value);
        } else {
            line.kind = Kind::Junk;
        }
        line.section = current;
        line.knownSection = known;
        doc.lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return doc;
}

inline std::string join(const Document &doc)
{
    std::string out;
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        out += doc.lines[i].raw;
        if (i + 1 < doc.lines.size() || doc.finalNewline) out += '\n';
    }
    return out;
}

// The revision of "no file at ~/apertar-hardware.conf". A base of this value means the page saw no file.
inline constexpr std::string_view kAbsentRevision = "h0000000000000000";

// "h" + 16 hex digits of FNV-1a 64: never all digits, so core.py does not turn it into a number.
inline std::string revision(const std::string &text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    char out[18];
    std::snprintf(out, sizeof(out), "h%016llx", static_cast<unsigned long long>(hash));
    return out;
}

// The value the readers use: the last line of that key (every C++ reader assigns in file order, so a later
// line or a later copy of the section wins).
inline const Line *lastKeyLine(const Document &doc, std::string_view section, std::string_view key)
{
    const Line *found = nullptr;
    for (const auto &line : doc.lines)
        if (line.kind == Kind::Key && line.section == section && line.key == key) found = &line;
    return found;
}

inline std::optional<std::string> valueOf(const Document &doc, std::string_view section, std::string_view key)
{
    if (const auto *line = lastKeyLine(doc, section, key)) return line->value;
    return std::nullopt;
}

// What Core's reader really uses: nullopt = its default, because the line is unreadable to it.
inline std::optional<std::string> effectiveValue(const Document &doc, std::string_view section, std::string_view key)
{
    const auto *line = lastKeyLine(doc, section, key);
    if (!line) return std::nullopt;
    const ValueRule rule = valueRule(section);
    if (!line->comment.empty() && (rule == ValueRule::Raw || (rule == ValueRule::IntegerStrict && key != "enabled")))
        return std::nullopt;
    return line->value;
}

inline std::vector<size_t> sectionHeaders(const Document &doc, std::string_view section)
{
    std::vector<size_t> out;
    for (const auto &line : doc.lines)
        if (line.kind == Kind::Section && line.section == section) out.push_back(line.number);
    return out;
}

inline bool hasSection(const Document &doc, std::string_view section)
{
    return std::any_of(doc.lines.begin(), doc.lines.end(),
                       [&](const Line &l) { return l.kind == Kind::Section && l.section == section; });
}

inline std::string sectionText(const Document &doc, std::string_view section)
{
    std::string out;
    for (const auto &line : doc.lines)
        if (line.section == section && (line.kind == Kind::Key || line.kind == Kind::Section))
            out += line.key + "=" + line.value + "\n";
    return out;
}

// ---- edits ---------------------------------------------------------------------------------------

struct Edit {
    enum class Op { Set, Unset, UnsetSection } op = Op::Set;
    std::string section;
    std::string key;
    std::string value;
};

// From set_controls' flat fields: set.<section>.<key>=<value>, unset.<section>.<key>=true, unset.<section>=true.
inline std::string parseEdits(const std::map<std::string, std::string> &fields, std::vector<Edit> &out)
{
    for (const auto &[name, value] : fields) {
        Edit edit;
        std::string rest;
        if (name.rfind("set.", 0) == 0) {
            edit.op = Edit::Op::Set;
            rest = name.substr(4);
        } else if (name.rfind("unset.", 0) == 0) {
            edit.op = Edit::Op::Unset;
            rest = name.substr(6);
        } else {
            continue;
        }
        const auto dot = rest.find('.');
        bool known = false;
        edit.section = canonicalSection(dot == std::string::npos ? rest : rest.substr(0, dot), &known);
        if (!known) return "Unknown section in '" + name + "'.";
        if (dot == std::string::npos) {
            if (edit.op == Edit::Op::Set) return "'" + name + "' names no key.";
            edit.op = Edit::Op::UnsetSection;
        } else {
            edit.key = canonicalKey(edit.section, rest.substr(dot + 1));
            if (edit.key.empty()) return "'" + name + "' names no key.";
        }
        if (edit.op == Edit::Op::Set) {
            edit.value = trim(value);
            if (edit.value == "null") return "'" + name + "' is null: send unset." + rest + "=true to remove a key.";
            if (edit.value.empty()) return "'" + name + "' is empty: use none, or unset." + edit.section + "." + edit.key + ".";
            if (edit.value.find_first_of("#;\n\r") != std::string::npos)
                return "'" + name + "': a value cannot hold # ; or a line break.";
        }
        out.push_back(std::move(edit));
    }
    // A new section reads like the templates: enabled, target, bus, address first.
    auto rank = [](const Edit &e) {
        static const std::string_view first[] = {"enabled", "target", "bus", "address"};
        for (size_t i = 0; i < std::size(first); ++i) if (e.key == first[i]) return static_cast<int>(i);
        return 4;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Edit &a, const Edit &b) {
        return a.section != b.section ? sectionOrder(a.section) < sectionOrder(b.section) : rank(a) < rank(b);
    });
    return {};
}

// The keys easy mode may write, per section. Text mode and Core's own IMU writes are not limited.
inline bool indexedKey(std::string_view key, std::string_view prefix, int maxIndex, std::initializer_list<std::string_view> fields)
{
    if (key.substr(0, prefix.size()) != prefix) return false;
    std::string_view rest = key.substr(prefix.size());
    size_t digits = 0;
    while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits]))) ++digits;
    if (digits == 0 || digits > 2) return false;
    if (std::stoi(std::string(rest.substr(0, digits))) > maxIndex) return false;
    rest = rest.substr(digits);
    for (const auto field : fields)
        if (rest == field) return true;
    return false;
}

inline bool knownKey(std::string_view section, std::string_view key)
{
    auto in = [&](std::initializer_list<std::string_view> keys) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    if (section == "gpio_buttons")
        return in({"enabled", "chip", "active_low", "pull", "poll_ms", "debounce_ms", "hold_ms", "double_ms"}) ||
            indexedKey(key, "d", 63, {"", "_single", "_double", "_triple", "_hold"}) ||
            indexedKey(key, "remote", 15, {"", "_single", "_double", "_triple", "_hold"});   // remote only: no pin
    if (section == "gpio_encoders")
        return key == "enabled" || indexedKey(key, "encoder", 7, {"_a", "_b", "_press_gpio", "_cw", "_ccw", "_press",
            "_invert", "_steps_per_event", "_single", "_double", "_triple", "_hold"});
    if (section == "gpio_switches")
        return key == "enabled" || indexedKey(key, "switch", 15, {"_pins", "_on", "_off", "_position0", "_position1", "_position2"});
    if (section == "pcf8574")
        return in({"enabled", "bus", "address", "poll_ms", "debounce_ms", "active_low", "hold_ms", "double_ms"}) ||
            indexedKey(key, "pin", 7, {"", "_single", "_double", "_triple", "_hold"});
    if (section == "neorotary4")
        return in({"enabled", "bus", "address", "poll_ms", "debounce_ms", "invert_direction", "reset_on_start",
                   "max_events_per_poll", "hold_ms", "double_ms"}) ||
            indexedKey(key, "encoder", 3, {"_cw", "_ccw", "_press", "_single", "_double", "_triple", "_hold"});
    if (section == "grove_analog")
        return in({"enabled", "bus", "address", "poll_ms", "deadband", "emit_on_start"}) ||
            indexedKey(key, "a", 7, {"", "_min", "_max", "_step", "_deadband", "_invert", "_mode", "_presets"});
    if (section == "recording_indicator" || section == "trigger_output")
        return in({"enabled", "target", "chip", "line", "pcf_pin", "bus", "address", "active_high", "open_drain"}) ||
            (section == "trigger_output" && in({"pulse_ms", "events"}));
    if (section == "ina219" || section == "cfexpress_hat") return in({"enabled", "bus", "address"});
    if (section == "imu") return in({"enabled", "driver"});
    if (const auto *info = sectionInfo(section); info && info->device) return in({"enabled", "bus", "address", "orientation"});
    return false;
}

// A set_controls key edit that Core refuses before it changes anything, or "".
inline std::string checkEdits(const Document &doc, const std::vector<Edit> &edits)
{
    for (const auto &edit : edits) {
        const auto *info = sectionInfo(edit.section);
        if (!info || !info->easy)
            return "[" + edit.section + "] is edited in Settings > Config (the text view), not key by key.";
        const auto headers = sectionHeaders(doc, edit.section);
        if (headers.size() > 1)
            return "[" + edit.section + "] appears " + std::to_string(headers.size()) + " times (lines " +
                std::to_string(headers[0]) + " and " + std::to_string(headers[1]) +
                "). Merge the copies in Settings > Config first: the readers use the later lines.";
        if (edit.op != Edit::Op::UnsetSection && !knownKey(edit.section, edit.key))
            return "'" + edit.key + "' is not a key of [" + edit.section + "] that Apertar reads. Use Settings > Config for anything else.";
    }
    return {};
}

inline std::vector<std::string> rawLines(const Document &doc)
{
    std::vector<std::string> out;
    for (const auto &line : doc.lines) out.push_back(line.raw);
    return out;
}

inline std::string fromRaw(const std::vector<std::string> &lines, bool finalNewline)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || finalNewline) out += '\n';
    }
    return out;
}

inline std::string applyEdit(const std::string &text, const Edit &edit)
{
    const Document doc = parse(text);
    std::vector<std::string> lines = rawLines(doc);
    const bool finalNewline = doc.finalNewline || doc.lines.empty();
    const auto &L = doc.lines;
    if (edit.op == Edit::Op::UnsetSection) {
        std::vector<std::string> kept;
        for (size_t i = 0; i < L.size(); ++i)
            if (L[i].section != edit.section) kept.push_back(lines[i]);
        return fromRaw(kept, finalNewline);
    }
    if (edit.op == Edit::Op::Unset) {
        std::vector<std::string> kept;
        for (size_t i = 0; i < L.size(); ++i)
            if (!(L[i].kind == Kind::Key && L[i].section == edit.section && L[i].key == edit.key)) kept.push_back(lines[i]);
        return fromRaw(kept, finalNewline);
    }
    const std::string newLine = edit.key + "=" + edit.value;
    // 1. Replace the first line of this key in the first section of this name; drop later copies.
    std::optional<size_t> firstHeader, lastInSection, replaced;
    std::vector<size_t> drop;
    for (size_t i = 0; i < L.size(); ++i) {
        if (L[i].kind == Kind::Section && L[i].section == edit.section && !firstHeader) firstHeader = i;
        if (!firstHeader || L[i].section != edit.section) continue;
        if (L[i].kind == Kind::Section && i != *firstHeader) break;   // a second copy of the section: leave it
        if (L[i].kind != Kind::Blank) lastInSection = i;
        if (L[i].kind == Kind::Key && L[i].key == edit.key) {
            if (replaced) drop.push_back(i);
            else replaced = i;
        }
    }
    if (replaced) {
        const size_t i = *replaced;
        const std::string indent = lines[i].substr(0, lines[i].find_first_not_of(" \t"));
        std::vector<std::string> out;
        for (size_t j = 0; j < lines.size(); ++j) {
            if (std::find(drop.begin(), drop.end(), j) != drop.end()) continue;
            if (j == i) {
                if (!L[i].comment.empty())   // keep the comment, on its own line (inline comments break two readers)
                    out.push_back(indent + (L[i].comment[0] == '#' || L[i].comment[0] == ';' ? "" : "# ") + L[i].comment);
                out.push_back(indent + newLine);
            } else {
                out.push_back(lines[j]);
            }
        }
        return fromRaw(out, finalNewline);
    }
    if (firstHeader) {   // 2. A new key goes after the section's last non-blank line.
        lines.insert(lines.begin() + static_cast<long>(*lastInSection + 1), newLine);
        return fromRaw(lines, finalNewline);
    }
    // 3. A new section goes before the first section that comes later in the canonical order.
    const int order = sectionOrder(edit.section);
    for (size_t i = 0; i < L.size(); ++i) {
        if (L[i].kind == Kind::Section && sectionOrder(L[i].section) > order) {
            size_t at = i;   // keep a comment block with the header it describes
            while (at > 0 && L[at - 1].kind == Kind::Comment) --at;
            lines.insert(lines.begin() + static_cast<long>(at), {"[" + edit.section + "]", newLine, ""});
            return fromRaw(lines, finalNewline);
        }
    }
    if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[" + edit.section + "]");
    lines.push_back(newLine);
    return fromRaw(lines, true);
}

inline std::string applyEdits(std::string text, const std::vector<Edit> &edits)
{
    for (const auto &edit : edits) text = applyEdit(text, edit);
    return text;
}

struct Conflict {
    std::string section, key, base, current;
};

// Easy-mode saves name the revision they started from. A key edited here that also changed in the file
// since then (by hand, the IMU page, another browser) is a conflict; every other key merges.
inline std::vector<Conflict> conflicts(const std::string &baseText, const std::string &currentText,
                                       const std::vector<Edit> &edits)
{
    const Document base = parse(baseText), current = parse(currentText);
    std::vector<Conflict> out;
    for (const auto &edit : edits) {
        if (edit.op == Edit::Op::UnsetSection) {
            const auto b = sectionText(base, edit.section), c = sectionText(current, edit.section);
            if (b != c) out.push_back({edit.section, "", b, c});
            continue;
        }
        const auto b = valueOf(base, edit.section, edit.key), c = valueOf(current, edit.section, edit.key);
        if (b != c) out.push_back({edit.section, edit.key, b.value_or("(absent)"), c.value_or("(absent)")});
    }
    return out;
}

// ---- diff ----------------------------------------------------------------------------------------

inline std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

// Unified diff, 2 lines of context. Common head and tail trimmed, LCS on the middle (edits are local).
inline std::string diff(const std::string &before, const std::string &after)
{
    const auto a = splitLines(before), b = splitLines(after);
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    const size_t n = a.size() - head - tail, m = b.size() - head - tail;
    if (n == 0 && m == 0) return {};
    std::vector<std::pair<char, std::string>> ops;
    if (n * m <= 4'000'000) {
        std::vector<std::vector<uint32_t>> lcs(n + 1, std::vector<uint32_t>(m + 1, 0));
        for (size_t i = n; i-- > 0;)
            for (size_t j = m; j-- > 0;)
                lcs[i][j] = a[head + i] == b[head + j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        size_t i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[head + i] == b[head + j]) { ops.push_back({' ', a[head + i]}); ++i; ++j; }
            else if (i < n && (j == m || lcs[i + 1][j] >= lcs[i][j + 1])) { ops.push_back({'-', a[head + i]}); ++i; }
            else { ops.push_back({'+', b[head + j]}); ++j; }
        }
    } else {
        for (size_t i = 0; i < n; ++i) ops.push_back({'-', a[head + i]});
        for (size_t j = 0; j < m; ++j) ops.push_back({'+', b[head + j]});
    }
    const size_t context = std::min<size_t>(2, head);
    const size_t after_ctx = std::min<size_t>(2, tail);
    std::string out = "@@ -" + std::to_string(head - context + 1) + " +" + std::to_string(head - context + 1) + " @@\n";
    for (size_t i = head - context; i < head; ++i) out += " " + a[i] + "\n";
    for (const auto &[op, text] : ops) out += std::string(1, op) + text + "\n";
    for (size_t i = 0; i < after_ctx; ++i) out += " " + a[a.size() - tail + i] + "\n";
    return out;
}

// ---- validation ----------------------------------------------------------------------------------

struct Facts {
    bool known = false;               // false: bus existence is not checked
    std::vector<int> buses;           // /dev/i2c-N that exist
    std::vector<size_t> dpiLines;     // active DPI dtoverlay lines in config.txt
};

struct Issue {
    size_t line = 0;      // 0 = the file as a whole
    bool error = true;
    std::string message;
    std::string id;       // rule|section|key|detail: the same problem after lines moved
    std::string rule;
    std::string section;  // lets the easy editor hang an issue on its row
    std::string key;
};

inline std::optional<long> integer(const std::string &text)
{
    if (text.empty()) return std::nullopt;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    if (end != text.c_str() + text.size()) return std::nullopt;
    return value;
}

// A switch's pins, "17" or "22,23": the BCM numbers 0..27, or nothing when any of them is not one.
inline std::vector<long> pinList(const std::string &text)
{
    std::vector<long> pins;
    for (size_t start = 0; start <= text.size();) {
        const auto end = text.find(',', start);
        const auto pin = integer(trim(std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (!pin || *pin < 0 || *pin > 27) return {};
        pins.push_back(*pin);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return pins;
}

inline bool truthy(const std::string &value, bool fallback)
{
    const std::string t = normalizeToken(value);
    if (t == "true" || t == "yes" || t == "on" || t == "1" || t == "enabled") return true;
    if (t == "false" || t == "no" || t == "off" || t == "0" || t == "disabled") return false;
    return fallback;
}

inline std::string busList(const std::vector<int> &buses)
{
    std::string out;
    for (const int bus : buses) out += (out.empty() ? "" : ", ") + std::to_string(bus);
    return out.empty() ? "none" : out;
}

inline std::string hex(long value)
{
    char out[8];
    std::snprintf(out, sizeof(out), "0x%02lx", value);
    return out;
}

// Is a device section on, the way its reader decides?
inline bool deviceOn(const Document &doc, const SectionInfo &info)
{
    if (!hasSection(doc, info.name)) return info.onWithoutSection;
    const auto enabled = effectiveValue(doc, info.name, "enabled");
    return enabled ? truthy(*enabled, info.onByDefault) : info.onByDefault;
}

inline std::vector<Issue> validate(const std::string &text, const Facts &facts)
{
    namespace act = hardware::actions;
    const Document doc = parse(text);
    std::vector<Issue> issues;
    auto add = [&](size_t line, bool error, std::string rule, std::string section, std::string key, std::string detail,
                   std::string message) {
        std::string id = rule + "|" + section + "|" + key + "|" + detail;
        issues.push_back({line, error, std::move(message), std::move(id), std::move(rule), std::move(section), std::move(key)});
    };
    const auto N = [](size_t n) { return std::to_string(n); };

    if (const auto crlf = text.find("\r\n"); crlf != std::string::npos)
        add(0, false, "crlf", "", "", "", "This file has Windows line ends (CRLF). Core reads them, but a save from the Config "
            "panel's text box writes plain line ends, so its diff shows every line changed.");

    std::map<std::string, size_t> sectionLine;
    std::map<std::pair<std::string, std::string>, size_t> keyLine;
    for (const auto &line : doc.lines) {
        // Core's NVMe reinsert helper reads the whole file with Python's configparser and catches only
        // OSError: a junk line, a key above the first section or a duplicate kills it.
        if (line.kind == Kind::Junk)
            add(line.number, true, "junk", line.section, "", trim(line.raw), "Line " + N(line.number) +
                " is not a section, a key=value or a comment: the C++ readers skip it, but Core's NVMe reinsert helper (Python) stops on it.");
        if (line.kind == Kind::Key && line.section.empty())
            add(line.number, true, "key_above", "", line.key, "", "Line " + N(line.number) +
                " is a key above the first [section]: Core's NVMe reinsert helper (Python) stops on it.");
        if (line.kind == Kind::Key && !line.raw.empty() && (line.raw[0] == ' ' || line.raw[0] == '\t'))
            add(line.number, false, "indented", line.section, line.key, "", "Line " + N(line.number) +
                " is indented: Python's configparser reads it as part of the line above.");
        if (line.kind == Kind::Section) {
            if (line.headerTrailing) {
                const ValueRule rule = valueRule(line.section);
                const bool strict = !line.headerTrailingComment || rule == ValueRule::IntegerStrict || rule == ValueRule::Raw;
                add(line.number, strict, "header_text", line.section, "", "", "Text after ']' on line " + N(line.number) + (strict
                    ? ": Core's " + line.section + " reader does not see this header, so the keys below it do not count. Put the comment on its own line."
                    : ": Apertar's readers handle this differently. Put the comment on its own line."));
            }
            if (!line.knownSection) {
                std::string best;
                for (const auto &info : kSections)
                    if (act::distance(line.section, info.name) <= 2) best = std::string(info.name);
                add(line.number, false, "unknown_section", line.section, "", "", "Unknown section [" + line.section +
                    "] on line " + N(line.number) + ": no reader uses it." + (best.empty() ? "" : " Did you mean [" + best + "]?"));
            }
            if (auto [it, fresh] = sectionLine.emplace(line.section, line.number); !fresh)
                add(line.number, true, "duplicate_section", line.section, "", "", "[" + line.section + "] appears twice (lines " +
                    N(it->second) + " and " + N(line.number) + "): the C++ readers use the later lines and Core's Python helper "
                    "may stop on it. Merge them.");
        }
        if (line.kind != Kind::Key) continue;
        if (auto [it, fresh] = keyLine.emplace(std::make_pair(line.section, line.key), line.number); !fresh)
            add(line.number, true, "duplicate_key", line.section, line.key, "", line.key + " is set twice in [" + line.section +
                "] (lines " + N(it->second) + " and " + N(line.number) + "): the C++ readers use the later one, Core's Python helper stops on it.");
        if (!line.comment.empty()) {
            const ValueRule rule = valueRule(line.section);
            const bool strict = rule == ValueRule::Raw || (rule == ValueRule::IntegerStrict && line.key != "enabled");
            add(line.number, strict, "inline_comment", line.section, line.key, "", "Comment after the value on line " + N(line.number) +
                (strict ? ": Core's " + line.section + " reader then ignores " + line.key + "=" + line.value +
                              " and keeps its default. Put the comment on its own line."
                        : ": Apertar's readers handle this differently. Put the comment on its own line."));
        }
        if (line.section == "storage") {
            // The write-buffer keys (CameraController reads them at each take); mount_path and pcie_controller stay Raw.
            const std::string problem = bufferbudget::bufferConfProblem(line.key, line.value);
            if (!problem.empty())
                add(line.number, true, "buffer_value", line.section, line.key, line.value, line.key + "=" + line.value + " on line " +
                    N(line.number) + " " + problem + ": Core uses the default for " + line.key +
                    (line.key == "buffer" ? " (frames)." : line.key == "frames_per_folder" ? " (0, one folder)." : "."));
        }
        if (line.section == "media") {
            // The storage pre-roll's keys (Core reads them at each request).
            const std::string problem = storagepreroll::prerollConfProblem(line.key, line.value);
            if (!problem.empty())
                add(line.number, true, "preroll_value", line.section, line.key, line.value, line.key + "=" + line.value + " on line " +
                    N(line.number) + " " + problem + ": Core uses the default for " + line.key + (line.key == "preroll" ? " (on)." : " (2)."));
        }
        if (line.key == "bus") {
            const auto bus = integer(line.value);
            if (!bus || *bus < 0 || *bus > 255)
                add(line.number, true, "bus_range", line.section, "bus", line.value, "bus=" + line.value + " on line " +
                    N(line.number) + " is not a bus number (0-255).");
        }
        if (line.key == "address") {
            const auto address = integer(line.value);
            if (!address || *address < 0x03 || *address > 0x77)
                add(line.number, true, "address_range", line.section, "address", line.value, "address=" + line.value +
                    " on line " + N(line.number) + " is not a 7-bit I2C address (0x03-0x77).");
        }
    }

    // Devices on buses that do not exist, with the bus each reader really uses (its default when the file
    // gives none or gives one it cannot read).
    if (facts.known) {
        for (const auto &info : kSections) {
            if (!info.device || !deviceOn(doc, info)) continue;
            const auto busText = effectiveValue(doc, info.name, "bus");
            const auto explicitBus = busText ? integer(*busText) : std::nullopt;
            const long bus = explicitBus ? *explicitBus : info.defaultBus;
            if (bus < 0) continue;   // the IMU probes buses itself when none is named
            if (std::find(facts.buses.begin(), facts.buses.end(), static_cast<int>(bus)) != facts.buses.end()) continue;
            size_t at = 0;
            if (const auto *l = lastKeyLine(doc, info.name, "bus")) at = l->number;
            else if (const auto headers = sectionHeaders(doc, info.name); !headers.empty()) at = headers.back();
            std::string why;
            if (bus == 1 && !facts.dpiLines.empty())
                why = " GPIO 2/3 are held by the DPI overlay on config.txt line " + N(facts.dpiLines.front()) + ".";
            const std::string b = std::to_string(bus);
            const std::string where = !hasSection(doc, info.name)
                ? "[" + std::string(info.name) + "] is not in this file, so Core looks on bus " + b + " (its default), which"
                : (explicitBus ? "[" + std::string(info.name) + "] bus " + b
                               : "[" + std::string(info.name) + "] has no usable bus=, so Core uses bus " + b + ", which");
            add(at, false, "bus_missing", std::string(info.name), "bus", b, where + " does not exist on this Pi (buses: " +
                busList(facts.buses) + ")." + why + " The device stays silent until the bus exists, or turn it off (enabled=false).");
        }
    }

    // GPIO lines: one owner each.
    std::map<long, std::pair<std::string, size_t>> claims;
    auto claim = [&](long gpio, const std::string &who, const std::string &section, const std::string &key, size_t line) {
        if (gpio < 0) return;
        if (auto [it, fresh] = claims.emplace(gpio, std::make_pair(who, line)); !fresh) {
            add(line, true, "gpio_twice", section, key, std::to_string(gpio), "GPIO " + std::to_string(gpio) + " is used by " +
                it->second.first + " (line " + N(it->second.second) + ") and " + who + " (line " + N(line) + ").");
            return;
        }
        const bool busOne = std::find(facts.buses.begin(), facts.buses.end(), 1) != facts.buses.end();
        if ((gpio == 2 || gpio == 3) && busOne)
            add(line, true, "gpio_i2c1", section, key, std::to_string(gpio), "GPIO " + std::to_string(gpio) + " is the " +
                (gpio == 2 ? "SDA" : "SCL") + " line of I2C bus 1; " + who + " cannot use it while the bus exists.");
        if (!facts.dpiLines.empty() && gpio <= 27)
            add(line, true, "gpio_dpi", section, key, std::to_string(gpio), "GPIO " + std::to_string(gpio) +
                " is held by the DPI overlay (config.txt line " + N(facts.dpiLines.front()) + "); " + who + " cannot use it.");
    };
    const bool buttonsOn = truthy(valueOf(doc, "gpio_buttons", "enabled").value_or("false"), false);
    const bool encodersOn = truthy(valueOf(doc, "gpio_encoders", "enabled").value_or("false"), false);
    const bool switchesOn = truthy(valueOf(doc, "gpio_switches", "enabled").value_or("false"), false);
    auto outputTarget = [&](const std::string &output) {
        return normalizeToken(valueOf(doc, output, "target").value_or("gpio"));
    };
    auto isGpioTarget = [](const std::string &t) { return t == "gpio" || t == "pin" || t == "raspberry_pi_gpio"; };
    auto isPcfTarget = [](const std::string &t) { return t == "pcf" || t == "pcf8574" || t == "i2c"; };
    // Core starts the trigger first, so it gets a shared line and the tally fails.
    for (const auto &output : {std::string("trigger_output"), std::string("recording_indicator")}) {
        if (!hasSection(doc, output) || !truthy(valueOf(doc, output, "enabled").value_or("false"), false)) continue;
        const std::string who = output == "trigger_output" ? "Trigger output" : "Tally";
        size_t at = sectionHeaders(doc, output).back();
        if (const auto *l = lastKeyLine(doc, output, "line")) at = l->number;
        if (isGpioTarget(outputTarget(output))) {
            const auto line = integer(valueOf(doc, output, "line").value_or(output == "trigger_output" ? "17" : "-1"));
            claim(line.value_or(-1), who, output, "line", at);
            // An open-drain line can only pull low; with active high an LED to GND never lights.
            if (truthy(valueOf(doc, output, "open_drain").value_or("false"), false) &&
                truthy(valueOf(doc, output, "active_high").value_or("false"), false))
                add(at, false, "open_drain_high", output, "open_drain", "", who + " has open drain with active high: the line can "
                    "only pull low, so an LED from the pin to GND never lights. Turn open drain off, or wire the LED from 3.3 V to the pin and use active low.");
        }
    }
    std::set<long> claimedButtons;
    for (const auto &line : doc.lines) {
        if (line.kind != Kind::Key) continue;
        const bool gpioButton = line.section == "gpio_buttons" && line.key.size() > 1 && line.key[0] == 'd' &&
            std::isdigit(static_cast<unsigned char>(line.key[1]));
        if (gpioButton) {
            // d10, and its clicks and hold (d10_double...), are one button on one pin: it is claimed once.
            const bool plain = line.key.find('_') == std::string::npos;
            const auto mapped = act::canonicalAction(line.value);
            const long pin = std::stol(line.key.substr(1));
            if (plain && hardware::isOutputAction(normalizeToken(line.value)))
                add(line.number, false, "tally_shorthand", line.section, line.key, "", "The tally shorthand on line " +
                    N(line.number) + " loses to an enabled [recording_indicator] section and has no polarity key. Use the section.");
            else if (buttonsOn && !mapped.empty() && claimedButtons.insert(pin).second)
                claim(pin, "button d" + std::to_string(pin), line.section, line.key, line.number);
        }
        if (line.section == "gpio_encoders" && encodersOn && line.key.rfind("encoder", 0) == 0) {
            const std::string field = line.key.substr(line.key.find('_') + 1);
            if (field == "a" || field == "b" || field == "press_gpio")
                if (const auto gpio = integer(line.value))
                    claim(*gpio, "encoder " + line.key.substr(7, 1) + " " + field, line.section, line.key, line.number);
        }
        if (line.section == "gpio_switches" && line.key.rfind("switch", 0) == 0 && line.key.size() > 11 &&
            line.key.compare(line.key.size() - 5, 5, "_pins") == 0) {
            const auto pins = pinList(line.value);
            if (pins.empty() || pins.size() > 2)
                add(line.number, true, "switch_pins", line.section, line.key, line.value, "Line " + N(line.number) + ": " + line.key +
                    " takes one pin (a two-way switch) or two (a three-way switch), such as 17 or 22,23.");
            else if (switchesOn)
                for (const long pin : pins)
                    claim(pin, "switch " + line.key.substr(6, line.key.size() - 11), line.section, line.key, line.number);
        }
    }

    // Actions: names Core knows, at most six, standby never on a turn, nothing that starts a take on a turn,
    // no silent duplicates.
    for (const auto &line : doc.lines) {
        if (line.kind != Kind::Key) continue;
        const bool actionKey =
            (line.section == "gpio_buttons" && line.key.size() > 1 && line.key[0] == 'd' && std::isdigit(static_cast<unsigned char>(line.key[1]))) ||
            (line.section == "gpio_buttons" && line.key.rfind("remote", 0) == 0) ||
            (line.section == "pcf8574" && line.key.rfind("pin", 0) == 0) ||
            (line.section == "gpio_switches" && line.key.rfind("switch", 0) == 0 && line.key.find("_pins") == std::string::npos) ||
            ((line.section == "neorotary4" || line.section == "gpio_encoders") && line.key.rfind("encoder", 0) == 0 &&
             (line.key.find("_cw") != std::string::npos || line.key.find("_ccw") != std::string::npos ||
              line.key.find("_press") != std::string::npos || line.key.find("_single") != std::string::npos ||
              line.key.find("_double") != std::string::npos || line.key.find("_triple") != std::string::npos ||
              line.key.find("_hold") != std::string::npos) && line.key.find("_press_gpio") == std::string::npos);
        if (!actionKey) continue;
        const auto parsed = hardware::parseButtonActions(line.value, [](const std::string &t) { return act::canonicalAction(t); });
        if (!parsed.error.empty()) {
            add(line.number, true, "mapping", line.section, line.key, parsed.error, "Line " + N(line.number) + ": " +
                parsed.error + "; Core drops the whole mapping.");
            continue;
        }
        const bool rotation = line.key.find("_cw") != std::string::npos || line.key.find("_ccw") != std::string::npos;
        std::map<std::string, int> seen;
        for (size_t start = 0; start <= line.value.size();) {
            const auto end = line.value.find(',', start);
            const auto token = act::canonicalAction(hardware::actionToken(std::string_view(line.value).substr(
                start, end == std::string::npos ? std::string::npos : end - start)));
            if (!token.empty() && ++seen[token] == 2)
                add(line.number, false, "duplicate_action", line.section, line.key, token, "'" + token + "' appears twice on line " +
                    N(line.number) + ": Core runs it once.");
            if (end == std::string::npos) break;
            start = end + 1;
        }
        for (const auto &action : parsed.actions) {
            const auto c = act::classify(action);
            if (!c.known) {
                const auto hint = act::suggestion(action);
                add(line.number, true, "unknown_action", line.section, line.key, action, "Unknown action '" + action + "' on line " +
                    N(line.number) + ": Core logs it and does nothing." + (hint.empty() ? "" : " Did you mean " + hint + "?"));
            } else if (!c.note.empty() && c.id == "wb_value") {
                add(line.number, false, "wb_clamp", line.section, line.key, action, "'" + action + "' on line " + N(line.number) + ": " + c.note + ".");
            }
            if (rotation && action == "standby")
                add(line.number, false, "standby_turn", line.section, line.key, "", "standby on a turn (line " + N(line.number) +
                    ") never fires: Core skips it for rotations.");
            if (rotation && act::warnOnTurn(action))
                add(line.number, false, "turn_starts", line.section, line.key, action, "'" + action + "' on a turn (line " +
                    N(line.number) + ") runs on every detent: one click starts it, the next stops it. Put it on a press.");
        }
    }

    // Order: the trigger and the tally take bus and address from a [pcf8574] above them.
    if (const auto pcf = sectionLine.find("pcf8574"); pcf != sectionLine.end()) {
        for (const auto &output : {std::string("trigger_output"), std::string("recording_indicator")}) {
            const auto it = sectionLine.find(output);
            if (it == sectionLine.end() || it->second > pcf->second) continue;
            if (isPcfTarget(outputTarget(output)))
                add(it->second, false, "pcf_order", output, "", "", "[" + output + "] targets the PCF8574 but comes before [pcf8574] (line " +
                    N(pcf->second) + "), so it does not inherit its bus and address.");
        }
        // Buttons and an output on one PCF8574 each keep their own copy of the pin byte.
        const SectionInfo *pcfInfo = sectionInfo("pcf8574");
        if (deviceOn(doc, *pcfInfo)) {
            const long inBus = integer(valueOf(doc, "pcf8574", "bus").value_or("1")).value_or(1);
            const long inAddress = integer(valueOf(doc, "pcf8574", "address").value_or("0x20")).value_or(0x20);
            for (const auto &output : {std::string("trigger_output"), std::string("recording_indicator")}) {
                if (!hasSection(doc, output) || !truthy(valueOf(doc, output, "enabled").value_or("false"), false) ||
                    !isPcfTarget(outputTarget(output)))
                    continue;
                const bool inherits = sectionLine.count(output) && sectionLine[output] > pcf->second;
                const long bus = integer(valueOf(doc, output, "bus").value_or("")).value_or(inherits ? inBus : 1);
                const long address = integer(valueOf(doc, output, "address").value_or("")).value_or(inherits ? inAddress : 0x20);
                if (bus == inBus && address == inAddress)
                    add(sectionLine[output], false, "pcf_shared", output, "", "", "[" + output + "] and the PCF8574 buttons share the chip at bus " +
                        std::to_string(bus) + " " + hex(address) + ": each keeps its own copy of the pin byte, so a save or a press can "
                        "glitch the other's pins (unconfirmed on hardware). A second PCF8574 for outputs avoids it.");
            }
        }
    }
    std::stable_sort(issues.begin(), issues.end(), [](const Issue &a, const Issue &b) { return a.line < b.line; });
    return issues;
}

// The new errors a save would add (a hand-edited file with old errors must not lock the editor out).
inline std::vector<Issue> newErrors(const std::vector<Issue> &before, const std::vector<Issue> &after)
{
    std::multiset<std::string> old;
    for (const auto &issue : before) if (issue.error) old.insert(issue.id);
    std::vector<Issue> out;
    for (const auto &issue : after) {
        if (!issue.error) continue;
        const auto it = old.find(issue.id);
        if (it != old.end()) old.erase(it);
        else out.push_back(issue);
    }
    return out;
}

// ---- config.txt, read only -----------------------------------------------------------------------

struct BootFacts {
    bool i2cArm = false;
    std::vector<size_t> dpiLines;          // uncommented DPI dtoverlay lines that apply on a Pi 5
    std::vector<std::string> dpiOverlays;  // their overlay names, unique, in file order (for the sed pattern)
};

// Pi firmware filters: lines apply until the next [filter]; [all] resets. Only [pi5]/[all] apply here.
// dtparam= is a comma list (dtparam=i2c_arm=on,i2c_arm_baudrate=400000), a bare name means on,
// and a # starts a comment.
inline BootFacts parseConfigTxt(const std::string &text)
{
    BootFacts facts;
    bool applies = true;
    size_t number = 0;
    for (const auto &raw : splitLines(text)) {
        ++number;
        std::string line = trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        if (line[0] == '[') {
            const std::string filter = normalizeToken(line);
            applies = filter == "[all]" || filter == "[pi5]";
            continue;
        }
        if (!applies) continue;
        if (line.rfind("dtparam=", 0) == 0) {
            const std::string list = line.substr(8);
            for (size_t start = 0; start <= list.size();) {
                const auto end = list.find(',', start);
                const std::string item = trim(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
                const auto eq = item.find('=');
                const std::string name = normalizeToken(item.substr(0, eq));
                const std::string value = eq == std::string::npos ? "on" : normalizeToken(item.substr(eq + 1));
                if (name == "i2c_arm" || name == "i2c") facts.i2cArm = truthy(value, facts.i2cArm);
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        if (line.rfind("dtoverlay=", 0) == 0) {
            const std::string rest = line.substr(10);
            const std::string name = trim(rest.substr(0, rest.find_first_of(", \t")));
            const std::string token = normalizeToken(name);
            if (token.find("dpi") != std::string::npos || token.find("hyperpixel") != std::string::npos) {
                facts.dpiLines.push_back(number);
                if (std::find(facts.dpiOverlays.begin(), facts.dpiOverlays.end(), name) == facts.dpiOverlays.end())
                    facts.dpiOverlays.push_back(name);
            }
        }
    }
    return facts;
}

// The commands the I2C card shows for Patrik to run. They name the overlays, not line numbers
// (which shift when the file is edited), back up to a timestamped copy, and are safe to run twice.
inline std::vector<std::string> dpiCommands(const BootFacts &facts)
{
    if (facts.dpiOverlays.empty()) return {};
    std::string names;
    for (const auto &name : facts.dpiOverlays) {
        std::string escaped;
        for (const char ch : name) {
            if (std::string_view(".[]()*+?^$|\\/").find(ch) != std::string_view::npos) escaped += '\\';
            escaped += ch;
        }
        names += (names.empty() ? "" : "|") + escaped;
    }
    return {
        "sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.$(date +%Y%m%d-%H%M%S)",
        "sudo sed -i -E 's/^(dtoverlay=(" + names + ")([,[:space:]]|$))/#\\1/' /boot/firmware/config.txt",
        "sudo reboot",
    };
}

// A text-mode save whose base is stale. If base -> mine changes only key values (no comment,
// blank, header or junk line differs, and no inline comment changed), it becomes key edits that merge onto the
// current file like easy mode. nullopt: refuse as before.
inline std::optional<std::vector<Edit>> textToEdits(const std::string &baseText, const std::string &mineText)
{
    const Document base = parse(baseText), mine = parse(mineText);
    auto skeleton = [](const Document &doc) {
        std::vector<std::string> out;
        for (const auto &line : doc.lines)
            if (line.kind != Kind::Key) out.push_back(line.raw);
        return out;
    };
    if (skeleton(base) != skeleton(mine)) return std::nullopt;
    using Key = std::pair<std::string, std::string>;
    auto values = [](const Document &doc, std::map<Key, std::pair<std::string, std::string>> &out) {
        for (const auto &line : doc.lines) {
            if (line.kind != Kind::Key) continue;
            if (line.section.empty()) return false;
            out[{line.section, line.key}] = {line.value, line.comment};
        }
        return true;
    };
    std::map<Key, std::pair<std::string, std::string>> b, m;
    if (!values(base, b) || !values(mine, m)) return std::nullopt;
    std::vector<Edit> edits;
    for (const auto &[key, vc] : m) {
        const auto it = b.find(key);
        if (it != b.end() && it->second.second != vc.second) return std::nullopt;   // an inline comment changed
        if (it == b.end() || it->second.first != vc.first) {
            if (!vc.second.empty()) return std::nullopt;
            edits.push_back({Edit::Op::Set, key.first, key.second, vc.first});
        }
    }
    for (const auto &[key, vc] : b)
        if (!m.count(key)) edits.push_back({Edit::Op::Unset, key.first, key.second, {}});
    return edits;
}

} // namespace apertar::hwconf
