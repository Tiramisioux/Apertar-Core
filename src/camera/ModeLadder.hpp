// Mode ladder (T34): pure logic, no libcamera, so it compiles and is tested on any host.
// CameraController owns the sanitiser, so it supplies each rung's state (valid, usable,
// ceiling) as plain numbers; this file parses, validates, chooses and prints.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <apertar/core/Types.hpp>

namespace apertar::ladder {

struct Rung {
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t bits = 12;       // 10, 12 or 16
    bool clearHdr = false;
    double threshold = 0.0;   // highest FPS this rung serves; 0 = the mode's own ceiling
    // The raw format of this resolution: "none" (uncompressed cDNG), "lossless_jpeg", "lossy_3to1" or
    // "lossy_5to1" (lossy DNG, no Clear HDR). Empty = the default: Clear HDR uncompressed, SDR lossless.
    std::string compression;
    CdngLog cdngLog = CdngLog::Off;  // CineMate Log for this resolution: off, or on (one step below the source depth)
};

inline bool knownCompression(const std::string &c)
{
    return c.empty() || c == "none" || c == "lossless_jpeg" || c == "lossy_3to1" || c == "lossy_5to1";
}

// The depth this resolution stores through CineMate Log, 0 = linear.
inline unsigned logBits(const Rung &r)
{
    return cdngLogStoredBits(r.cdngLog, r.bits);
}

// What this resolution records with: the named compression, else the default for its class.
inline std::string effectiveCompression(const Rung &r)
{
    return !r.compression.empty() ? r.compression : (r.clearHdr ? "none" : "lossless_jpeg");
}

struct Ladder {
    bool enabled = false;     // the toggle: Core honours the ladder
    bool rampReady = false;   // with Speed Ramp on: takes start in the top rung
    std::vector<Rung> rungs;  // first = preferred mode, serves the lowest FPS; thresholds rise
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

// One number per rung, in and out of takes.
inline double rungCeiling(const Rung &r, const RungState &s)
{
    if (!s.usable || s.modeCeiling <= 0.0)
        return 0.0;
    return r.threshold > 0.0 ? std::min(r.threshold, s.modeCeiling) : s.modeCeiling;
}

inline double ladderCeiling(const Ladder &l, const std::vector<RungState> &s)
{
    double top = 0.0;
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i)
        top = std::max(top, rungCeiling(l.rungs[i], s[i]));
    return top;
}

// The first usable rung that reaches targetFps (upper bound inclusive). If none does,
// the usable rung with the highest ceiling. -1 when no rung is usable.
inline int chooseRung(const Ladder &l, const std::vector<RungState> &s, double targetFps)
{
    int best = -1;
    double bestCeiling = 0.0;
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i) {
        const double ceiling = rungCeiling(l.rungs[i], s[i]);
        if (ceiling <= 0.0)
            continue;
        if (ceiling >= targetFps - 0.0005)
            return static_cast<int>(i);
        if (ceiling > bestCeiling) {
            bestCeiling = ceiling;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Resolution text: WIDTHxHEIGHT:BITS:hdr|sdr[:FPS[:COMPRESSION[:log|log10|log8]]], joined by ",".
// log = CineMate Log on (one step down); log10 and log8 are old spellings that still parse as log; absent = off.
// Example: 3840x2160:16:hdr:25:none:log,3840x2160:12:sdr::lossless_jpeg
inline bool parseRungs(const std::string &text, std::vector<Rung> &out, std::string &error)
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
        if (!item.empty() && item.back() == ':')
            f.push_back("");
        Rung r;
        char *end = nullptr;
        const auto x = f.empty() ? std::string::npos : f[0].find('x');
        if (f.size() < 3 || f.size() > 6 || x == std::string::npos) {
            error = "\"" + item + "\": expected WIDTHxHEIGHT:BITS:hdr|sdr[:FPS[:COMPRESSION[:log|log10|log8]]].";
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
        if (f.size() >= 4 && !f[3].empty()) {
            r.threshold = std::strtod(f[3].c_str(), &end);
            if (!end || *end != '\0' || !(r.threshold > 0.0 && r.threshold <= 280.0)) {
                error = "\"" + item + "\": the threshold must be an FPS above 0 and at most 280, or empty.";
                return false;
            }
        }
        if (f.size() >= 5) {
            r.compression = f[4];
            if (!knownCompression(r.compression)) {
                error = "\"" + item + "\": compression must be none, lossless_jpeg, lossy_3to1 or lossy_5to1.";
                return false;
            }
        }
        if (f.size() == 6) {
            if (f[5] == "log" || f[5] == "log10" || f[5] == "log8") r.cdngLog = CdngLog::Auto;
            else {
                error = "\"" + item + "\": the last field must be log.";
                return false;
            }
        }
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

inline std::string rungsText(const std::vector<Rung> &rungs)
{
    std::string text;
    for (const auto &r : rungs) {
        if (!text.empty())
            text += ",";
        text += std::to_string(r.width) + "x" + std::to_string(r.height) + ":" + std::to_string(r.bits) + ":" +
                (r.clearHdr ? "hdr" : "sdr") +
                (r.threshold > 0.0 || !r.compression.empty() || r.cdngLog != CdngLog::Off ? ":" + (r.threshold > 0.0 ? number(r.threshold) : "") : "") +
                (!r.compression.empty() || r.cdngLog != CdngLog::Off ? ":" + r.compression : "") +
                (r.cdngLog == CdngLog::Auto ? ":log" : "");
    }
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

// Errors refuse a save. Warnings are shown and saved (they depend on the format).
inline void validate(const Ladder &l, const std::vector<RungState> &s,
                     std::vector<std::string> &errors, std::vector<std::string> &warnings)
{
    const auto name = [](size_t i) { return "Resolution " + std::to_string(i + 1); };
    if (l.rungs.size() > 8)
        errors.push_back("At most 8 resolutions.");
    double lastThreshold = 0.0, reached = 0.0;
    size_t lastThresholdRung = 0;
    for (size_t i = 0; i < l.rungs.size(); ++i) {
        const Rung &r = l.rungs[i];
        for (size_t j = 0; j < i; ++j)
            if (sameMode(r, l.rungs[j]))
                errors.push_back(name(i) + " repeats resolution " + std::to_string(j + 1) + ".");
        if (r.threshold > 0.0) {
            if (lastThreshold > 0.0 && r.threshold <= lastThreshold)
                errors.push_back("Thresholds must rise down the list: " + name(i) + " (" + number(r.threshold) +
                                 " fps) is not above resolution " + std::to_string(lastThresholdRung + 1) + " (" +
                                 number(lastThreshold) + " fps).");
            lastThreshold = r.threshold;
            lastThresholdRung = i;
        }
        if (r.clearHdr && (r.compression == "lossy_3to1" || r.compression == "lossy_5to1"))
            errors.push_back(name(i) + ": lossy DNG does not support Clear HDR.");
        if (i >= s.size())
            continue;
        if (!s[i].valid) {
            errors.push_back(name(i) + ": " + label(r) + " is not a mode of this sensor.");
            continue;
        }
        if (!s[i].usable) {
            warnings.push_back(name(i) + " (" + label(r) + ") cannot run in this format; it is skipped.");
            continue;
        }
        if (r.threshold > s[i].modeCeiling + 0.0005)
            warnings.push_back(name(i) + " stops at " + number(s[i].modeCeiling) + " fps here, not " +
                               number(r.threshold) + ".");
        const double ceiling = rungCeiling(r, s[i]);
        if (ceiling <= reached + 0.0005)
            warnings.push_back(name(i) + " is never used: an earlier resolution already reaches " + number(reached) + " fps.");
        reached = std::max(reached, ceiling);
        if (i > 0) {
            const Rung &p = l.rungs[i - 1];
            if (p.clearHdr != r.clearHdr || p.bits != r.bits)
                warnings.push_back("Resolution " + std::to_string(i) + " -> " + std::to_string(i + 1) + " changes " +
                                   label(p) + " to " + label(r) + ": the look changes between takes.");
            if (p.width != r.width || p.height != r.height)
                warnings.push_back("Resolution " + std::to_string(i) + " -> " + std::to_string(i + 1) +
                                   " changes the frame size.");
        }
    }
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
               ",\"compression_set\":" + (r.compression.empty() ? "false" : "true") +
               ",\"log_bits\":" + std::to_string(logBits(r)) +
               ",\"usable\":" + (st.usable ? "true" : "false") + ",\"mode_ceiling\":" + number(st.modeCeiling) +
               ",\"window_w\":" + std::to_string(st.windowWidth) + ",\"window_h\":" + std::to_string(st.windowHeight) +
               ",\"binning\":" + std::to_string(st.binning) +
               ",\"raw_width\":" + std::to_string(st.rawWidth ? st.rawWidth : r.width) +
               ",\"raw_height\":" + std::to_string(st.rawHeight ? st.rawHeight : r.height) + ",\"ceilings\":{\"none\":" + number(st.ceilingBy[0]) +
               ",\"lossless_jpeg\":" + number(st.ceilingBy[1]) + ",\"lossy_3to1\":" + number(st.ceilingBy[2]) +
               ",\"lossy_5to1\":" + number(st.ceilingBy[3]) + "}";
    };
    std::string out = "{\"enabled\":" + std::string(l.enabled ? "true" : "false") +
        ",\"ramp_ready\":" + (l.rampReady ? "true" : "false") + ",\"resolutions_text\":" + quote(rungsText(l.rungs)) +
        ",\"aspects\":" + list(l.aspects) +
        ",\"ceiling\":" + number(ladderCeiling(l, s)) + ",\"running_resolution\":" + std::to_string(running + 1) +
        ",\"resolutions\":[";
    double from = 0.0;  // a rung serves (from, ceiling]
    for (size_t i = 0; i < l.rungs.size() && i < s.size(); ++i) {
        const double ceiling = rungCeiling(l.rungs[i], s[i]);
        out += std::string(i ? "," : "") + "{" + mode(l.rungs[i], s[i]) + ",\"threshold\":" +
               number(l.rungs[i].threshold) + ",\"ceiling\":" + number(ceiling) + ",\"from\":" + number(from) + "}";
        from = std::max(from, ceiling);
    }
    out += "],\"pool\":[";
    for (size_t i = 0; i < pool.size(); ++i)
        out += std::string(i ? "," : "") + "{" + mode(pool[i].mode, pool[i].state) + "}";
    return out + "],\"errors\":" + list(errors) + ",\"warnings\":" + list(warnings) + "}";
}

// mode_ladder.conf: one key per line, so a person can read it. A ladder save rewrites it whole, with this
// header. A filter save writes a missing or empty file whole; in any other it replaces the aspects= line and
// puts it last (withAspectsLine), so the header stays as it is.
inline std::string fileText(const Ladder &l)
{
    return "# Apertar mode ladder (T34). Core writes this file; edit it in the WebUI (Settings > Modes: Activated sensor modes and Mode Ladder).\n"
           "enabled=" + std::string(l.enabled ? "true" : "false") + "\n"
           "ramp_ready=" + std::string(l.rampReady ? "true" : "false") + "\n"
           "resolutions=" + rungsText(l.rungs) + "\n"
           "aspects=" + aspectsText(l.aspects) + "\n";
}

// The file with its aspects= line replaced by one at the end (as fileText puts it; several become one). Every
// other line stays byte for byte, so a filter save never rewrites what this Core could not read: a resolutions
// line from another build, a CRLF hand edit, an unknown key. A last line without its newline gets one.
inline std::string withAspectsLine(const std::string &text, const std::vector<std::string> &aspects)
{
    std::string out;
    std::stringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos && line.compare(0, eq, "aspects") == 0)
            continue;
        out += line + "\n";
    }
    return out + "aspects=" + aspectsText(aspects) + "\n";
}

inline Ladder parseFile(const std::string &text)
{
    Ladder l;
    std::stringstream in(text);
    std::string line, error;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "enabled") l.enabled = value == "true";
        else if (key == "ramp_ready") l.rampReady = value == "true";
        else if ((key == "resolutions" || key == "rungs") && !parseRungs(value, l.rungs, error)) l.rungs.clear();   // "rungs": the first name
        else if (key == "aspects" && !parseAspects(value, l.aspects, error)) l.aspects.clear();   // a bad filter is All; the rungs stay
    }
    if (l.rungs.empty())
        l.enabled = false;  // a broken file never switches the camera
    return l;
}

} // namespace apertar::ladder
