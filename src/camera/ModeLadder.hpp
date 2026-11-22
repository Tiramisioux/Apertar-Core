// Dynamic resolution (the mode ladder): pure logic, no libcamera, so it compiles and is tested on any host.
// CameraController owns the sanitiser, so it supplies each mode's state (valid, usable,
// ceiling) as plain numbers; this file parses, validates, orders, chooses and prints.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace apertar::ladder {

struct Rung {
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t bits = 12;       // 10, 12 or 16
    bool clearHdr = false;
    // The raw format the table gives this mode: "none" (uncompressed cDNG), "lossless_jpeg", or empty = the default:
    // Clear HDR uncompressed, SDR lossless. Set by CameraController from the table's cell; never saved or parsed.
    std::string compression;
};

// What this mode records with: the named compression, else the default for its class.
inline std::string effectiveCompression(const Rung &r)
{
    return !r.compression.empty() ? r.compression : (r.clearHdr ? "none" : "lossless_jpeg");
}

struct Ladder {
    bool enabled = false;     // Dynamic resolution is on: Core honours the table
    std::vector<Rung> rungs;  // the enabled modes, empty = the default list; the order of use is tryOrder's
    // The WebUI's aspect filter on Activated sensor modes, the ratios as the card labels them ("16:9",
    // "2.39:1"); empty = All. Kept here so the camera remembers it; it never changes which mode runs.
    std::vector<std::string> aspects;
};

// What CameraController knows about one rung right now.
struct RungState {
    bool valid = false;        // a mode of this sensor (sanitiser keeps it in uncompressed cDNG)
    bool usable = false;       // the sanitiser keeps it in the current format and compression
    double modeCeiling = 0.0;  // the sanitiser's FPS ceiling (table, probe, lossless cap)
    // For the editor (filled when the reply is built, not by the chooser): the sensor window the mode reads,
    // in native pixels (the probe's crop rectangle), its binning, and the FPS ceiling in each compression
    // (none, lossless_jpeg, lossy_3to1, lossy_5to1; 0 = not available in that compression).
    uint32_t windowWidth = 0;
    uint32_t windowHeight = 0;
    uint32_t binning = 0;
    uint32_t rawWidth = 0;      // the frame the sensor delivers for the mode (the probe's size); 0 = the rung's own size
    uint32_t rawHeight = 0;
    double ceilingBy[4] = { 0.0, 0.0, 0.0, 0.0 };
};

inline bool sameMode(const Rung &a, const Rung &b)
{
    return a.width == b.width && a.height == b.height && a.bits == b.bits && a.clearHdr == b.clearHdr;
}

inline std::string classLabel(const Rung &r)  // "HDR16", "SDR12"
{
    return (r.clearHdr ? "HDR" : "SDR") + std::to_string(r.bits);
}

inline std::string label(const Rung &r)       // "4K Clear HDR 16-bit", "3840x2076 SDR 12-bit"
{
    const std::string size = (r.width == 3840 && r.height == 2160)
        ? std::string("4K") : std::to_string(r.width) + "x" + std::to_string(r.height);
    return size + (r.clearHdr ? " Clear HDR " : " SDR ") + std::to_string(r.bits) + "-bit";
}

// A list of modes as the settings and set_mode_ladder carry it: WIDTHxHEIGHT:BITS:hdr|sdr joined by ",". The text of
// an old ladder has up to three more fields (threshold, compression, Log); they are read and dropped: the table
// decides them now.
// Example: 3840x2160:16:hdr,3840x2160:12:sdr
inline bool parseModes(const std::string &text, std::vector<Rung> &out, std::string &error)
{
    out.clear();
    std::stringstream list(text);
    std::string item;
    while (std::getline(list, item, ',')) {
        std::vector<std::string> f;
        std::stringstream fields(item);
        std::string part;
        while (std::getline(fields, part, ':'))
            f.push_back(part);
        Rung r;
        char *end = nullptr;
        const auto x = f.empty() ? std::string::npos : f[0].find('x');
        if (f.size() < 3 || f.size() > 6 || x == std::string::npos) {
            error = "\"" + item + "\": expected WIDTHxHEIGHT:BITS:hdr|sdr.";
            return false;
        }
        r.width = static_cast<uint32_t>(std::strtoul(f[0].c_str(), &end, 10));
        r.height = static_cast<uint32_t>(std::strtoul(f[0].c_str() + x + 1, &end, 10));
        r.bits = static_cast<uint32_t>(std::strtoul(f[1].c_str(), &end, 10));
        if (r.width == 0 || r.height == 0 || (r.bits != 10 && r.bits != 12 && r.bits != 16) ||
            (f[2] != "hdr" && f[2] != "sdr")) {
            error = "\"" + item + "\": size, bits (10, 12, 16) or hdr|sdr not understood.";
            return false;
        }
        r.clearHdr = f[2] == "hdr";
        out.push_back(r);
    }
    if (out.empty()) {
        error = "Add at least one resolution.";
        return false;
    }
    return true;
}

inline std::string number(double v)
{
    std::ostringstream o;
    o << v;
    return o.str();
}

// The enabled modes as saved: WIDTHxHEIGHT:BITS:hdr|sdr joined by ",".
inline std::string modesText(const std::vector<Rung> &rungs)
{
    std::string text;
    for (const auto &r : rungs)
        text += (text.empty() ? "" : ",") + std::to_string(r.width) + "x" + std::to_string(r.height) + ":" +
                std::to_string(r.bits) + ":" + (r.clearHdr ? "hdr" : "sdr");
    return text;
}

// The aspect filter's text: the card's ratio labels joined by "," ("16:9,2.39:1"), "" = All. At most
// kMaxAspects labels of 1 to kMaxAspectLength characters from 0-9 . : (a repeat is dropped); anything else
// is refused, so a hand edit or a damaged line can only mean All.
inline constexpr size_t kMaxAspects = 32;
inline constexpr size_t kMaxAspectLength = 16;

inline bool parseAspects(const std::string &text, std::vector<std::string> &out, std::string &error)
{
    out.clear();
    if (text.size() > kMaxAspects * (kMaxAspectLength + 1)) {
        error = "The aspect filter is too long.";
        return false;
    }
    std::stringstream list(text);
    std::string item;
    while (std::getline(list, item, ',')) {
        if (item.empty() || item.size() > kMaxAspectLength || item.find_first_not_of("0123456789.:") != std::string::npos) {
            error = "\"" + item + "\": an aspect ratio is written as the card shows it, such as 16:9 or 2.39:1.";
            out.clear();
            return false;
        }
        if (std::find(out.begin(), out.end(), item) != out.end())
            continue;
        if (out.size() == kMaxAspects) {
            error = "At most " + std::to_string(kMaxAspects) + " aspect ratios.";
            out.clear();
            return false;
        }
        out.push_back(item);
    }
    return true;
}

inline std::string aspectsText(const std::vector<std::string> &aspects)
{
    std::string text;
    for (size_t i = 0; i < aspects.size(); ++i)
        text += (i ? "," : "") + aspects[i];
    return text;
}

// A sensor mode the editor may add (from the probe), with its state in this format.
struct PoolEntry {
    Rung mode;
    RungState state;
};

// The get_mode_ladder / set_mode_ladder reply. running = 0-based rung index, -1 = off the ladder.
inline std::string toJson(const Ladder &l, const std::vector<RungState> &s, int running,
                          const std::vector<PoolEntry> &pool,
                          const std::vector<std::string> &errors, const std::vector<std::string> &warnings)
{
    const auto quote = [](const std::string &t) {  // labels and messages come from this file: ASCII
        std::string out = "\"";
        for (char c : t) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        return out + "\"";
    };
    const auto list = [&](const std::vector<std::string> &v) {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i)
            out += (i ? "," : "") + quote(v[i]);
        return out + "]";
    };
    const auto mode = [&](const Rung &r, const RungState &st) {
        return "\"width\":" + std::to_string(r.width) + ",\"height\":" + std::to_string(r.height) +
               ",\"bits\":" + std::to_string(r.bits) + ",\"clear_hdr\":" + (r.clearHdr ? "true" : "false") +
               ",\"label\":" + quote(label(r)) + ",\"class\":" + quote(classLabel(r)) +
               ",\"compression\":" + quote(effectiveCompression(r)) +
               ",\"usable\":" + (st.usable ? "true" : "false") + ",\"mode_ceiling\":" + number(st.modeCeiling) +
               ",\"window_w\":" + std::to_string(st.windowWidth) + ",\"window_h\":" + std::to_string(st.windowHeight) +
               ",\"binning\":" + std::to_string(st.binning) +
               ",\"raw_width\":" + std::to_string(st.rawWidth ? st.rawWidth : r.width) +
               ",\"raw_height\":" + std::to_string(st.rawHeight ? st.rawHeight : r.height) + ",\"ceilings\":{\"none\":" + number(st.ceilingBy[0]) +
               ",\"lossless_jpeg\":" + number(st.ceilingBy[1]) + ",\"lossy_3to1\":" + number(st.ceilingBy[2]) +
               ",\"lossy_5to1\":" + number(st.ceilingBy[3]) + "}";
    };
    const auto ceilingOf = [](const RungState &st) { return st.usable ? std::max(st.modeCeiling, 0.0) : 0.0; };
    double top = 0.0;
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i)
        top = std::max(top, ceilingOf(s[i]));
    std::string out = "{\"enabled\":" + std::string(l.enabled ? "true" : "false") +
        ",\"resolutions_text\":" + quote(modesText(l.rungs)) +
        ",\"aspects\":" + list(l.aspects) +
        ",\"ceiling\":" + number(top) + ",\"running_resolution\":" + std::to_string(running + 1) +
        ",\"resolutions\":[";
    double from = 0.0;  // a mode serves (from, ceiling]
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i) {
        const double ceiling = ceilingOf(s[i]);
        out += std::string(i ? "," : "") + "{" + mode(l.rungs[i], s[i]) + ",\"ceiling\":" + number(ceiling) +
               ",\"from\":" + number(from) + "}";
        from = std::max(from, ceiling);
    }
    out += "],\"pool\":[";
    for (size_t i = 0; i < pool.size(); ++i)
        out += std::string(i ? "," : "") + "{" + mode(pool[i].mode, pool[i].state) + "}";
    return out + "],\"errors\":" + list(errors) + ",\"warnings\":" + list(warnings) + "}";
}

// Errors refuse a save: a repeated mode, a mode this sensor does not have. A warning is shown and saved.
inline void validateModes(const Ladder &l, const std::vector<RungState> &s,
                          std::vector<std::string> &errors, std::vector<std::string> &warnings)
{
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i) {
        for (size_t j = 0; j < i; ++j)
            if (sameMode(l.rungs[i], l.rungs[j]))
                errors.push_back(label(l.rungs[i]) + " is listed twice.");
        if (!s[i].valid)
            errors.push_back(label(l.rungs[i]) + " is not a mode of this sensor.");
        else if (!s[i].usable)
            warnings.push_back(label(l.rungs[i]) + " cannot run in this format; it is skipped.");
    }
}

// The ladder from the text of its settings (camera_settings.conf keys): the switch, the enabled modes and the
// aspect filter. A modes text that does not parse is an empty list, which means the default list; a bad filter
// is All.
inline Ladder fromText(bool enabled, const std::string &modes, const std::string &aspects)
{
    Ladder l;
    std::string error;
    l.enabled = enabled;
    if (!modes.empty() && !parseModes(modes, l.rungs, error))
        l.rungs.clear();
    if (!parseAspects(aspects, l.aspects, error))
        l.aspects.clear();
    return l;
}

// mode_ladder.conf, the file the ladder lived in before camera_settings.conf held it: one key per line.
// Read only to take an old file over. A resolutions line with thresholds, compression and Log fields reads as its
// modes: the table decides the rest now.
inline Ladder parseFile(const std::string &text)
{
    bool enabled = false;
    std::string resolutions, aspects;
    std::stringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "enabled") enabled = value == "true";
        else if (key == "resolutions" || key == "rungs") resolutions = value;   // "rungs": the first name
        else if (key == "aspects") aspects = value;
    }
    return fromText(enabled, resolutions, aspects);
}

// Dynamic resolution: one enabled mode with what the camera knows about it right now.
struct Candidate {
    Rung mode;
    bool usable = false;    // the sanitiser keeps it in the current format
    bool crop = false;      // it reads a smaller window of the sensor than the full frame
    double cap = 0.0;       // the highest fps it serves, a step of SustainedFps.hpp; 0 = not used
};

// The order the modes are tried in, best first: Clear HDR before SDR, deeper bit depth, a mode of the full frame
// before a crop-in, more pixels first. Indexes into the candidates.
inline std::vector<size_t> tryOrder(const std::vector<Candidate> &c)
{
    std::vector<size_t> order(c.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&c](size_t a, size_t b) {
        const Rung &x = c[a].mode, &y = c[b].mode;
        if (x.clearHdr != y.clearHdr) return x.clearHdr;
        if (x.bits != y.bits) return x.bits > y.bits;
        if (c[a].crop != c[b].crop) return !c[a].crop;
        return uint64_t(x.width) * x.height > uint64_t(y.width) * y.height;
    });
    return order;
}

// The first usable mode in that order whose cap reaches targetFps (upper bound inclusive). If none does, the
// usable mode with the highest cap (the first of equals). An index into the candidates; -1 when none is usable.
inline int chooseMode(const std::vector<Candidate> &c, double targetFps)
{
    int best = -1;
    double bestCap = 0.0;
    for (const size_t i : tryOrder(c)) {
        if (!c[i].usable || c[i].cap <= 0.0)
            continue;
        if (c[i].cap >= targetFps - 0.0005)
            return static_cast<int>(i);
        if (c[i].cap > bestCap) {
            bestCap = c[i].cap;
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace apertar::ladder
