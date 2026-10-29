#pragma once
// The rows the Controls page shows, read from the text of ~/apertar-hardware.conf: one row per GPIO button, encoder
// knob and PCF pin, with the actions it runs. Pure: no I/O. The page writes back through set_controls key edits.
#include "hardware/HardwareConfigText.hpp"

#include <set>
#include <string>
#include <vector>

namespace apertar::hwconf {

struct ControlLine {
    std::string label;                  // "Press", "Turn clockwise"
    std::string key;                    // the file key a save writes, canonical
    std::string raw;                    // the value as written (empty when the key is not in the file)
    std::vector<std::string> actions;   // raw split at commas, each in its canonical spelling
};

struct ControlRow {
    std::string source;                 // the section: gpio_buttons, gpio_switches, neorotary4, pcf8574, gpio_encoders
    std::string title;                  // "GPIO 10", "Knob 1", "PCF pin 0"
    std::string detail;                 // wiring, when the section has it
    int pin = -1;                       // BCM number of a GPIO button (a switch's first pin), else -1
    std::vector<ControlLine> lines;
    std::vector<std::pair<std::string, std::string>> canAdd;   // key, label: clicks and hold not in the file yet
    std::string kind;                   // button, switch2, switch3, knob, pin, encoder
    std::vector<std::string> keys;      // every key of the row in the file: what removing it unsets
    std::string base;                   // a row with a push: its keys are base + a gesture suffix ("d10", "encoder0")
    std::vector<std::pair<std::string, std::string>> gestures;   // suffix, label: Press first ("" or "_press")
};

// The clicks and hold a GPIO button can carry next to Press (counted like Cinemate: ButtonGesture).
inline const std::vector<std::pair<std::string, std::string>> &buttonGestures()
{
    static const std::vector<std::pair<std::string, std::string>> gestures = {
        {"_single", "Single click"}, {"_double", "Double click"}, {"_triple", "Triple click"}, {"_hold", "Hold"}};
    return gestures;
}

// Does the file hold d<pin> or any of its clicks and hold?
inline bool gpioButtonInFile(const Document &doc, int pin)
{
    const std::string key = "d" + std::to_string(pin);
    if (valueOf(doc, "gpio_buttons", key)) return true;
    for (const auto &[suffix, label] : buttonGestures())
        if (valueOf(doc, "gpio_buttons", key + suffix)) return true;
    return false;
}

inline std::string jsonEscape(const std::string &text)
{
    std::string out;
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\u%04x", ch);
                out += hex;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

inline ControlLine controlLine(const Document &doc, const std::string &section, const std::string &label, const std::string &key)
{
    ControlLine line{label, key, {}, {}};
    if (const auto value = valueOf(doc, section, key)) {
        line.raw = *value;
        size_t start = 0;
        while (start <= value->size()) {
            const auto end = value->find(',', start);
            const std::string token = hardware::actions::canonicalAction(
                hardware::actionToken(std::string_view(*value).substr(start, end == std::string::npos ? std::string::npos : end - start)));
            if (!token.empty()) line.actions.push_back(token);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return line;
}

// Keys of the form <prefix><number><suffix>, e.g. d10, encoder2_cw, pin3, in ascending number.
inline std::set<int> indexesOf(const Document &doc, const std::string &section, const std::string &prefix)
{
    std::set<int> found;
    for (const auto &line : doc.lines) {
        if (line.kind != Kind::Key || line.section != section || line.key.rfind(prefix, 0) != 0) continue;
        size_t digits = 0;
        while (prefix.size() + digits < line.key.size() && std::isdigit(static_cast<unsigned char>(line.key[prefix.size() + digits]))) ++digits;
        if (digits > 0 && digits <= 2) found.insert(std::stoi(line.key.substr(prefix.size(), digits)));
    }
    return found;
}

// Every key of a row in the file: base itself and base_<anything> (d1 is not d10's).
inline std::vector<std::string> keysWithBase(const Document &doc, const std::string &section, const std::string &base)
{
    std::vector<std::string> keys;
    for (const auto &line : doc.lines)
        if (line.kind == Kind::Key && line.section == section && (line.key == base || line.key.rfind(base + "_", 0) == 0) &&
            std::find(keys.begin(), keys.end(), line.key) == keys.end())
            keys.push_back(line.key);
    return keys;
}

// A push's lines: Press (empty when not in the file), then the clicks and hold the file has. Every row with a push
// works the same on the page: GPIO buttons and PCF pins (Press is the bare key), knobs and GPIO encoders (_press).
inline void addPushLines(ControlRow &row, const Document &doc, const std::string &base, const std::string &pressSuffix)
{
    row.base = base;
    row.gestures = {{pressSuffix, "Press"}};
    row.lines.push_back(controlLine(doc, row.source, "Press", base + pressSuffix));
    for (const auto &[suffix, label] : buttonGestures()) {
        row.gestures.emplace_back(suffix, label);
        if (valueOf(doc, row.source, base + suffix))
            row.lines.push_back(controlLine(doc, row.source, label, base + suffix));
        else
            row.canAdd.emplace_back(base + suffix, label);
    }
    row.keys = keysWithBase(doc, row.source, base);
}

inline std::vector<ControlRow> controlRows(const Document &doc)
{
    std::vector<ControlRow> rows;
    for (const int pin : indexesOf(doc, "gpio_buttons", "d")) {
        if (!gpioButtonInFile(doc, pin)) continue;
        ControlRow row{"gpio_buttons", "GPIO " + std::to_string(pin), {}, pin, {}, {}, "button", {}, {}, {}};
        addPushLines(row, doc, "d" + std::to_string(pin), "");
        rows.push_back(std::move(row));
    }
    // Two-way switch: one pin, an action for on and for off. Three-way: two pins, an action per position; the middle
    // position is neither pin closed. Core applies the position a switch is in when it starts.
    for (const int index : indexesOf(doc, "gpio_switches", "switch")) {
        const std::string base = "switch" + std::to_string(index);
        const auto pins = pinList(valueOf(doc, "gpio_switches", base + "_pins").value_or(""));
        if (pins.empty() || pins.size() > 2) continue;   // the validator says why
        const std::string first = std::to_string(pins[0]);
        ControlRow row{"gpio_switches", "GPIO " + first, "Two-way switch", static_cast<int>(pins[0]), {}, {}, "switch2", {}, {}, {}};
        if (pins.size() == 1) {
            row.lines.push_back(controlLine(doc, "gpio_switches", "On", base + "_on"));
            row.lines.push_back(controlLine(doc, "gpio_switches", "Off", base + "_off"));
        } else {
            const std::string second = std::to_string(pins[1]);
            row.title = "GPIO " + first + " + " + second;
            row.detail = "Three-way switch";
            row.kind = "switch3";
            row.lines.push_back(controlLine(doc, "gpio_switches", "Position 1 (GPIO " + first + ")", base + "_position0"));
            row.lines.push_back(controlLine(doc, "gpio_switches", "Position 2 (middle)", base + "_position1"));
            row.lines.push_back(controlLine(doc, "gpio_switches", "Position 3 (GPIO " + second + ")", base + "_position2"));
        }
        row.keys = keysWithBase(doc, "gpio_switches", base);
        rows.push_back(std::move(row));
    }
    if (hasSection(doc, "neorotary4")) {
        for (int knob = 0; knob < 4; ++knob) {
            const std::string base = "encoder" + std::to_string(knob);
            ControlRow row{"neorotary4", "Knob " + std::to_string(knob + 1), {}, -1, {}, {}, "knob", {}, {}, {}};
            row.lines.push_back(controlLine(doc, "neorotary4", "Turn clockwise", base + "_cw"));
            row.lines.push_back(controlLine(doc, "neorotary4", "Turn counter-clockwise", base + "_ccw"));
            addPushLines(row, doc, base, "_press");
            rows.push_back(std::move(row));
        }
    }
    for (const int pin : indexesOf(doc, "pcf8574", "pin")) {
        const std::string key = "pin" + std::to_string(pin);
        if (keysWithBase(doc, "pcf8574", key).empty()) continue;
        ControlRow row{"pcf8574", "PCF pin " + std::to_string(pin), {}, -1, {}, {}, "button", {}, {}, {}};
        addPushLines(row, doc, key, "");
        rows.push_back(std::move(row));
    }
    for (const int index : indexesOf(doc, "gpio_encoders", "encoder")) {
        const std::string base = "encoder" + std::to_string(index);
        ControlRow row{"gpio_encoders", "Encoder " + std::to_string(index), {}, -1, {}, {}, "encoder", {}, {}, {}};
        row.detail = "A " + valueOf(doc, "gpio_encoders", base + "_a").value_or("?") + ", B " +
            valueOf(doc, "gpio_encoders", base + "_b").value_or("?") + ", push " + valueOf(doc, "gpio_encoders", base + "_press_gpio").value_or("none");
        row.lines.push_back(controlLine(doc, "gpio_encoders", "Turn clockwise", base + "_cw"));
        row.lines.push_back(controlLine(doc, "gpio_encoders", "Turn counter-clockwise", base + "_ccw"));
        addPushLines(row, doc, base, "_press");
        rows.push_back(std::move(row));
    }
    return rows;
}

// Is the section there and switched on, the way its reader decides?
inline bool sectionOn(const Document &doc, const std::string &name)
{
    const SectionInfo *info = sectionInfo(name);
    return hasSection(doc, name) && truthy(effectiveValue(doc, name, "enabled").value_or(""), info && info->onByDefault);
}

// BCM pins taken by something that is switched on, with who holds them: buttons, encoders, and the trigger and
// tally on a GPIO line. A disabled section holds no pin.
inline std::vector<std::pair<int, std::string>> gpioPinsInUse(const Document &doc)
{
    std::vector<std::pair<int, std::string>> used;
    auto add = [&](long pin, const std::string &by) { if (pin >= 0) used.emplace_back(static_cast<int>(pin), by); };
    if (sectionOn(doc, "gpio_buttons"))
        for (const int pin : indexesOf(doc, "gpio_buttons", "d"))
            if (gpioButtonInFile(doc, pin)) add(pin, "button d" + std::to_string(pin));
    if (sectionOn(doc, "gpio_encoders"))
        for (const int index : indexesOf(doc, "gpio_encoders", "encoder"))
            for (const char *field : {"_a", "_b", "_press_gpio"})
                if (const auto value = valueOf(doc, "gpio_encoders", "encoder" + std::to_string(index) + field))
                    if (const auto pin = integer(*value)) add(*pin, "encoder " + std::to_string(index));
    if (sectionOn(doc, "gpio_switches"))
        for (const int index : indexesOf(doc, "gpio_switches", "switch"))
            for (const long pin : pinList(valueOf(doc, "gpio_switches", "switch" + std::to_string(index) + "_pins").value_or("")))
                add(pin, "switch " + std::to_string(index));
    for (const std::string output : {"trigger_output", "recording_indicator"}) {
        if (!sectionOn(doc, output)) continue;
        const std::string target = normalizeToken(valueOf(doc, output, "target").value_or("gpio"));
        if (target == "gpio" || target == "pin" || target == "raspberry_pi_gpio")
            if (const auto line = integer(valueOf(doc, output, "line").value_or("-1")); line)
                add(*line, output == "trigger_output" ? "trigger output" : "tally");
    }
    return used;
}

// A peripheral's settings as the page edits them: the file's value, or the reader's default when the key is missing.
struct SettingSpec {
    const char *key;
    const char *label;
    const char *type;       // bool, choice, number, text
    const char *fallback;   // what the reader uses with no key
    const char *options;    // choice: comma list
    int min;
    int max;
};

inline std::vector<SettingSpec> peripheralSettings(const std::string &section)
{
    if (section == "gpio_buttons")
        return {
            {"enabled", "On", "bool", "false", "", 0, 0},
            {"active_low", "Pressed", "bool", "true", "low,high", 0, 0},
            {"pull", "Pull", "choice", "up", "up,down,none", 0, 0},
            {"debounce_ms", "Debounce (ms)", "number", "60", "", 0, 1000},
        };   // poll_ms (chips without edge events) and chip stay text-only: the Pi 5's header needs neither
    if (section == "cfexpress_hat")
        return {
            {"enabled", "On", "bool", "true", "", 0, 0},
            {"bus", "I2C bus", "number", "10", "", 0, 255},
            {"address", "Address", "text", "0x34", "", 0, 0},
        };
    if (section == "neorotary4")
        return {
            {"enabled", "On", "bool", "true", "", 0, 0},
            {"bus", "I2C bus", "number", "1", "", 0, 255},
            {"address", "Address", "text", "0x49", "", 0, 0},
            {"invert_direction", "Reverse turning", "bool", "false", "", 0, 0},
            {"debounce_ms", "Debounce (ms)", "number", "60", "", 0, 1000},
            {"poll_ms", "Poll every (ms)", "number", "30", "", 1, 1000},
        };
    if (section == "pcf8574")
        return {
            {"enabled", "On", "bool", "true", "", 0, 0},
            {"bus", "I2C bus", "number", "1", "", 0, 255},
            {"address", "Address", "text", "0x20", "", 0, 0},
            {"active_low", "Pressed", "bool", "true", "low,high", 0, 0},
            {"debounce_ms", "Debounce (ms)", "number", "60", "", 0, 1000},
            {"poll_ms", "Poll every (ms)", "number", "40", "", 1, 1000},
        };
    if (section == "grove_analog")
        return {
            {"enabled", "On", "bool", "true", "", 0, 0},
            {"bus", "I2C bus", "number", "1", "", 0, 255},
            {"address", "Address", "text", "0x04", "", 0, 0},
            {"deadband", "Deadband", "number", "32", "", 0, 4095},
            {"poll_ms", "Poll every (ms)", "number", "80", "", 1, 1000},
        };
    return {};
}

inline std::string peripheralSettingsJson(const Document &doc, const std::string &section)
{
    std::string out = "[";
    bool first = true;
    for (const auto &spec : peripheralSettings(section)) {
        const auto value = effectiveValue(doc, section, spec.key);
        out += std::string(first ? "" : ",") + "{\"key\":\"" + spec.key + "\",\"label\":\"" + jsonEscape(spec.label) +
            "\",\"type\":\"" + spec.type + "\",\"value\":\"" + jsonEscape(value.value_or(spec.fallback)) +
            "\",\"in_file\":" + (value ? "true" : "false") + ",\"default\":\"" + spec.fallback + "\"";
        if (std::string(spec.type) == "choice" || std::string(spec.options) != "") {   // a bool's: the words for true,false
            out += ",\"options\":[";
            std::string options = spec.options;
            for (size_t start = 0, n = 0; start <= options.size(); ++n) {
                const auto end = options.find(',', start);
                out += std::string(n ? "," : "") + "\"" + options.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\"";
                if (end == std::string::npos) break;
                start = end + 1;
            }
            out += "]";
        }
        if (std::string(spec.type) == "number")
            out += ",\"min\":" + std::to_string(spec.min) + ",\"max\":" + std::to_string(spec.max);
        out += "}";
        first = false;
    }
    return out + "]";
}

// {"sections":[...],"rows":[...],"gpio_pins":{"used":[...],"free":[...]}}: the part of get_controls the page draws.
inline std::string modelJson(const Document &doc)
{
    std::string out = "{\"sections\":[";
    bool first = true;
    for (const char *name : {"gpio_buttons", "gpio_switches", "neorotary4", "pcf8574", "gpio_encoders", "grove_analog", "trigger_output",
                             "recording_indicator"}) {
        const bool present = hasSection(doc, name);
        const bool on = sectionOn(doc, name);
        out += std::string(first ? "" : ",") + "{\"name\":\"" + name + "\",\"present\":" + (present ? "true" : "false") +
            ",\"enabled\":" + (on ? "true" : "false") + "}";
        first = false;
    }
    out += "],\"rows\":[";
    first = true;
    for (const auto &row : controlRows(doc)) {
        const bool on = sectionOn(doc, row.source);
        out += std::string(first ? "" : ",") + "{\"source\":\"" + row.source + "\",\"title\":\"" + jsonEscape(row.title) + "\",\"detail\":\"" +
            jsonEscape(row.detail) + "\",\"pin\":" + std::to_string(row.pin) + ",\"enabled\":" + (on ? "true" : "false") + ",\"lines\":[";
        for (size_t i = 0; i < row.lines.size(); ++i) {
            const auto &line = row.lines[i];
            out += std::string(i ? "," : "") + "{\"label\":\"" + jsonEscape(line.label) + "\",\"key\":\"" + line.key + "\",\"raw\":\"" +
                jsonEscape(line.raw) + "\",\"actions\":[";
            for (size_t k = 0; k < line.actions.size(); ++k) out += std::string(k ? "," : "") + "\"" + jsonEscape(line.actions[k]) + "\"";
            out += "]}";
        }
        out += "],\"can_add\":[";
        for (size_t i = 0; i < row.canAdd.size(); ++i)
            out += std::string(i ? "," : "") + "{\"key\":\"" + row.canAdd[i].first + "\",\"label\":\"" + jsonEscape(row.canAdd[i].second) + "\"}";
        out += "],\"kind\":\"" + row.kind + "\",\"keys\":[";
        for (size_t i = 0; i < row.keys.size(); ++i) out += std::string(i ? "," : "") + "\"" + row.keys[i] + "\"";
        out += "],\"base\":\"" + row.base + "\",\"gestures\":[";
        for (size_t i = 0; i < row.gestures.size(); ++i)
            out += std::string(i ? "," : "") + "{\"suffix\":\"" + row.gestures[i].first + "\",\"label\":\"" + row.gestures[i].second + "\"}";
        out += "]}";
        first = false;
    }
    out += "],\"gpio_pins\":{\"used\":[";
    const auto used = gpioPinsInUse(doc);
    std::set<int> taken;
    for (size_t i = 0; i < used.size(); ++i) {
        out += std::string(i ? "," : "") + "{\"pin\":" + std::to_string(used[i].first) + ",\"by\":\"" + jsonEscape(used[i].second) + "\"}";
        taken.insert(used[i].first);
    }
    out += "],\"free\":[";
    first = true;
    for (int pin = 4; pin <= 27; ++pin) {   // 2 and 3 are the I2C bus 1 lines
        if (taken.count(pin)) continue;
        out += std::string(first ? "" : ",") + std::to_string(pin);
        first = false;
    }
    out += "]}}";
    return out;
}

} // namespace apertar::hwconf
