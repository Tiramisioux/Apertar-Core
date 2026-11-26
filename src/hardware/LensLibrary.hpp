#pragma once
// The lens library: the lenses the operator saved, which one is in use, and the working lens (what the controls and the
// WebUI's Lens pane edit; it differs from its saved entry until Save over). Pure but for loadFile/saveFile; host-tested by
// tools/lens_test.cpp. The file is ~/.config/apertar-core/lenses.conf, written by Cef168Lens only.
//
// A lens id is one byte and different lenses share one, so an entry is picked by its key, never by the id. The id only
// decides which entry is selected when a lens is mounted: the most recently used one with that id.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace apertar::hardware::cef168 {

// The board's focus range reads 0..0, 0..32767 (the control's default ceiling) or 0..65535 (seen on a Canon EF-S 10-22)
// until the lens is calibrated; after a calibration it is the real range (0..1192 on that lens). Anything else with
// min < max is a calibration.
inline bool focusRangeValid(int low, int high)
{
    return low < high && high != 32767 && high != 65535;
}

struct Entry {
    std::string key;               // slug of the name, unique in the library; "" = the working lens is not saved
    std::string name;
    std::string lastUsed;          // ISO UTC, "" = never
    int lensId = -1;               // the board's byte; -1 = not seen yet
    double apertureMin = 0.0;      // widest f-number the operator entered, 0 = not entered
    double apertureMax = 0.0;
    double lastIris = 0.0;         // 0 = none
    std::optional<int> focusMin, focusMax;
    std::optional<unsigned> distanceMinCm, distanceMaxCm;   // 65535 = infinity
    std::vector<double> pwlMap;    // rpi.af "map": inverse focus distance (m^-1), position, ... ascending; empty = none
    std::optional<unsigned> stepFrames;                      // rpi.af "step_frames" from the lens's moving time
    std::optional<bool> capIris, capFocus, capAutofocus;     // what the lens was shown to do; nothing = untested

    bool calibrated() const { return focusMin && focusMax && focusRangeValid(*focusMin, *focusMax); }
};

struct Library {
    std::vector<Entry> entries;
    std::string selected;          // key of the entry in use, "" = none
    Entry working;
};

// What Save over compares: everything but the iris, which follows the lens by itself, and the last-used stamp.
inline bool differs(const Entry &a, const Entry &b)
{
    return a.name != b.name || a.apertureMin != b.apertureMin || a.apertureMax != b.apertureMax || a.focusMin != b.focusMin ||
        a.focusMax != b.focusMax || a.distanceMinCm != b.distanceMinCm || a.distanceMaxCm != b.distanceMaxCm ||
        a.pwlMap != b.pwlMap || a.stepFrames != b.stepFrames || a.capIris != b.capIris || a.capFocus != b.capFocus || a.capAutofocus != b.capAutofocus;
}

// "Sigma 18-35 f/1.8" -> "sigma-18-35-f1-8": lowercase ASCII, runs of anything else one '-', the '/' of an f-number dropped.
inline std::string slug(const std::string &name)
{
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c == 'f' || c == 'F') && i + 2 < name.size() && name[i + 1] == '/' && std::isdigit(static_cast<unsigned char>(name[i + 2]))) {
            out += 'f';
            ++i;
        } else if (c < 128 && std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "lens" : out;
}

// One line, trimmed, at most 64 bytes.
inline std::string cleanName(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        const bool control = static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
        if (control || c == ' ') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();   // not mid-character
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
    }
    return out;
}

inline Entry *find(Library &lib, const std::string &key)
{
    for (Entry &entry : lib.entries)
        if (entry.key == key) return &entry;
    return nullptr;
}
inline const Entry *find(const Library &lib, const std::string &key)
{
    for (const Entry &entry : lib.entries)
        if (entry.key == key) return &entry;
    return nullptr;
}

// A key no other entry has: the slug, then slug-2, slug-3. `ignore` is the entry being renamed.
inline std::string uniqueKey(const Library &lib, const std::string &name, const std::string &ignore = {})
{
    const std::string base = slug(name);
    std::string key = base;
    for (int n = 2; key != ignore && find(lib, key); ++n) key = base + "-" + std::to_string(n);
    return key;
}

inline std::vector<const Entry *> matching(const Library &lib, int lensId)
{
    std::vector<const Entry *> out;
    for (const Entry &entry : lib.entries)
        if (entry.lensId == lensId) out.push_back(&entry);
    return out;
}

// The entry with this id that was used last (a tie goes to the later one in the file).
inline const Entry *mostRecent(const Library &lib, int lensId)
{
    const Entry *best = nullptr;
    for (const Entry *entry : matching(lib, lensId))
        if (!best || entry->lastUsed >= best->lastUsed) best = entry;
    return best;
}

inline bool unsaved(const Library &lib)
{
    if (!lib.selected.empty()) {
        const Entry *entry = find(lib, lib.selected);
        return !entry || differs(lib.working, *entry);
    }
    return !lib.working.name.empty() || lib.working.apertureMin > 0.0 || lib.working.apertureMax > 0.0 || lib.working.calibrated();
}

inline std::string nowIso()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

// The focus range the lens library holds for the working lens (Core's own calibration run), nothing before one.
inline std::optional<std::pair<int, int>> focusRange(const Entry &entry)
{
    if (!entry.calibrated()) return std::nullopt;
    return std::pair<int, int>{*entry.focusMin, *entry.focusMax};
}

// The range the controls use: the library's, else the board's own when that is valid, else nothing.
inline std::optional<std::pair<int, int>> effectiveFocusRange(const Entry &entry, int boardLow, int boardHigh, bool boardPresent)
{
    if (const auto own = focusRange(entry)) return own;
    if (boardPresent && focusRangeValid(boardLow, boardHigh)) return std::pair<int, int>{boardLow, boardHigh};
    return std::nullopt;
}

// ---- the operations; each returns "" or what is wrong ------------------------------------------------------------------

// Choose an entry (its values become the working lens, discarding unsaved changes), or "" for none: a blank working lens.
inline std::string select(Library &lib, const std::string &key, const std::string &now)
{
    if (key.empty()) {
        Entry blank;
        blank.lensId = lib.working.lensId;
        lib.working = blank;
        lib.selected.clear();
        return {};
    }
    Entry *entry = find(lib, key);
    if (!entry) return "There is no saved lens '" + key + "'.";
    entry->lastUsed = now;
    lib.working = *entry;
    lib.selected = key;
    return {};
}

// A lens with this id was mounted (Core start, plugged in, swapped). Keeps what is selected when it is that lens, else
// picks the most recently used entry with the id, else starts a blank working lens that is not saved. True when it changed.
inline bool adopt(Library &lib, int lensId, const std::string &now)
{
    if (const Entry *current = find(lib, lib.selected); current && current->lensId == lensId) return false;
    if (lib.selected.empty() && lib.working.lensId == lensId) return false;
    if (const Entry *recent = mostRecent(lib, lensId)) {
        select(lib, recent->key, now);
        return true;
    }
    Entry blank;
    blank.lensId = lensId;
    lib.working = blank;
    lib.selected.clear();
    return true;
}

inline std::string saveAs(Library &lib, const std::string &rawName, const std::string &now)
{
    const std::string name = cleanName(rawName);
    if (name.empty()) return "Give the lens a name first.";
    Entry entry = lib.working;
    entry.name = name;
    entry.key = uniqueKey(lib, name);
    entry.lastUsed = now;
    lib.entries.push_back(entry);
    lib.selected = entry.key;
    lib.working = entry;
    return {};
}

// Replaces the selected entry with the working lens; the entry follows the working name (and so its key).
inline std::string saveOver(Library &lib, const std::string &now)
{
    Entry *entry = find(lib, lib.selected);
    if (!entry) return "No saved lens is selected: use save_as.";
    if (lib.working.name.empty()) return "Give the lens a name first.";
    Entry replaced = lib.working;
    replaced.key = uniqueKey(lib, replaced.name, lib.selected);
    replaced.lastUsed = now;
    *entry = replaced;
    lib.selected = replaced.key;
    lib.working = replaced;
    return {};
}

inline std::string remove(Library &lib, const std::string &key)
{
    const auto it = std::find_if(lib.entries.begin(), lib.entries.end(), [&](const Entry &e) { return e.key == key; });
    if (it == lib.entries.end()) return "There is no saved lens '" + key + "'.";
    lib.entries.erase(it);
    if (lib.selected == key) {   // the working lens stays, now unsaved
        lib.selected.clear();
        lib.working.key.clear();
    }
    return {};
}

// The iris belongs to the lens: it goes into the working lens and the selected entry at once.
inline void rememberIris(Library &lib, double fNumber)
{
    lib.working.lastIris = fNumber;
    if (Entry *entry = find(lib, lib.selected)) entry->lastIris = fNumber;
}

// ---- the file ----------------------------------------------------------------------------------------------------------

namespace detail {
inline std::string numberText(double value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}
inline void writeEntry(std::ostringstream &out, const Entry &e)
{
    auto boolean = [](const std::optional<bool> &v) { return *v ? "true" : "false"; };
    out << "name=" << e.name << "\n";
    if (e.lensId >= 0) out << "lens_id=" << e.lensId << "\n";
    if (!e.lastUsed.empty()) out << "last_used=" << e.lastUsed << "\n";
    if (e.apertureMin > 0.0) out << "aperture_min=" << numberText(e.apertureMin) << "\n";
    if (e.apertureMax > 0.0) out << "aperture_max=" << numberText(e.apertureMax) << "\n";
    if (e.lastIris > 0.0) out << "last_iris=" << numberText(e.lastIris) << "\n";
    if (e.focusMin) out << "focus_min=" << *e.focusMin << "\n";
    if (e.focusMax) out << "focus_max=" << *e.focusMax << "\n";
    if (e.distanceMinCm) out << "distance_min_cm=" << *e.distanceMinCm << "\n";
    if (e.distanceMaxCm) out << "distance_max_cm=" << *e.distanceMaxCm << "\n";
    if (!e.pwlMap.empty()) {
        out << "pwl_map=";
        for (size_t i = 0; i < e.pwlMap.size(); ++i) out << (i ? "," : "") << numberText(e.pwlMap[i]);
        out << "\n";
    }
    if (e.stepFrames) out << "step_frames=" << *e.stepFrames << "\n";
    if (e.capIris) out << "cap_iris=" << boolean(e.capIris) << "\n";
    if (e.capFocus) out << "cap_focus=" << boolean(e.capFocus) << "\n";
    if (e.capAutofocus) out << "cap_autofocus=" << boolean(e.capAutofocus) << "\n";
}
inline void readKey(Entry &e, const std::string &key, const std::string &value)
{
    char *end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    const bool numeric = !value.empty() && *end == '\0' && std::isfinite(number);
    if (key == "name") e.name = cleanName(value);
    else if (key == "last_used") e.lastUsed = value;
    else if (key == "lens_id" && numeric) e.lensId = static_cast<int>(number);
    else if (key == "aperture_min" && numeric) e.apertureMin = number;
    else if (key == "aperture_max" && numeric) e.apertureMax = number;
    else if (key == "last_iris" && numeric) e.lastIris = number;
    else if (key == "focus_min" && numeric) e.focusMin = static_cast<int>(number);
    else if (key == "focus_max" && numeric) e.focusMax = static_cast<int>(number);
    else if (key == "distance_min_cm" && numeric && number >= 0) e.distanceMinCm = static_cast<unsigned>(number);
    else if (key == "distance_max_cm" && numeric && number >= 0) e.distanceMaxCm = static_cast<unsigned>(number);
    else if (key == "pwl_map") {
        std::vector<double> map;
        std::istringstream list(value);
        std::string item;
        bool ok = true;
        while (std::getline(list, item, ',')) {
            char *itemEnd = nullptr;
            const double n = std::strtod(item.c_str(), &itemEnd);
            if (item.empty() || *itemEnd != '\0' || !std::isfinite(n)) { ok = false; break; }
            map.push_back(n);
        }
        if (ok && map.size() % 2 == 0) e.pwlMap = std::move(map);
    }
    else if (key == "step_frames" && numeric && number >= 0) e.stepFrames = static_cast<unsigned>(number);
    else if (key == "cap_iris") e.capIris = value == "true";
    else if (key == "cap_focus") e.capFocus = value == "true";
    else if (key == "cap_autofocus") e.capAutofocus = value == "true";
}
} // namespace detail

inline std::string serialize(const Library &lib)
{
    std::ostringstream out;
    out << "# Apertar lens library. Core writes this file (the WebUI's Lens pane); edit it only with Core stopped.\n";
    out << "selected=" << lib.selected << "\n\n[working]\n";
    detail::writeEntry(out, lib.working);
    for (const Entry &entry : lib.entries) {
        out << "\n[lens." << entry.key << "]\n";
        detail::writeEntry(out, entry);
    }
    return out.str();
}

inline Library parse(const std::string &text)
{
    Library lib;
    Entry *target = nullptr;   // the section being read; nothing above the first one
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            const std::string section = line.substr(1, line.size() - 2);
            if (section == "working") target = &lib.working;
            else if (section.rfind("lens.", 0) == 0 && section.size() > 5) {
                lib.entries.push_back({});
                lib.entries.back().key = section.substr(5);
                target = &lib.entries.back();
            } else target = nullptr;
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (!target) {
            if (key == "selected") lib.selected = value;
        } else {
            detail::readKey(*target, key, value);
        }
    }
    if (!lib.selected.empty() && !find(lib, lib.selected)) lib.selected.clear();
    lib.working.key = lib.selected;
    return lib;
}

inline Library loadFile(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

// tmp + rename, like Core's other writers. "" or what went wrong.
inline std::string saveFile(const std::filesystem::path &path, const Library &lib)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.parent_path() / (".lenses.conf." + std::to_string(::getpid()) + ".tmp");
    {
        std::ofstream out(temp, std::ios::trunc);
        out << serialize(lib);
        out.flush();
        if (!out) return "Could not write " + temp.string();
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return "Could not replace " + path.string();
    }
    return {};
}

} // namespace apertar::hardware::cef168
