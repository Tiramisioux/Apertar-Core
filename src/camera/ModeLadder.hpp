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

namespace apertar::ladder {

struct Rung {
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t bits = 12;       // 10, 12 or 16
    bool clearHdr = false;
    double threshold = 0.0;   // highest FPS this rung serves; 0 = the mode's own ceiling
};

struct Ladder {
    bool enabled = false;     // the toggle: Core honours the ladder
    bool rampReady = false;   // with Speed Ramp on: takes start in the top rung
    std::vector<Rung> rungs;  // first = preferred mode, serves the lowest FPS; thresholds rise
};

// What CameraController knows about one rung right now.
struct RungState {
    bool valid = false;        // a mode of this sensor (sanitiser keeps it in uncompressed cDNG)
    bool usable = false;       // the sanitiser keeps it in the current format and compression
    double modeCeiling = 0.0;  // the sanitiser's FPS ceiling (table, probe, lossless cap)
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

// Rung text: WIDTHxHEIGHT:BITS:hdr|sdr[:FPS], rungs joined by ",".
// Example: 3840x2160:16:hdr:25,3840x2160:12:sdr
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
        if (f.size() < 3 || f.size() > 4 || x == std::string::npos) {
            error = "\"" + item + "\": expected WIDTHxHEIGHT:BITS:hdr|sdr or WIDTHxHEIGHT:BITS:hdr|sdr:FPS.";
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
        if (f.size() == 4 && !f[3].empty()) {
            r.threshold = std::strtod(f[3].c_str(), &end);
            if (!end || *end != '\0' || !(r.threshold > 0.0 && r.threshold <= 280.0)) {
                error = "\"" + item + "\": the threshold must be an FPS above 0 and at most 280, or empty.";
                return false;
            }
        }
        out.push_back(r);
    }
    if (out.empty()) {
        error = "Add at least one rung.";
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
                (r.clearHdr ? "hdr" : "sdr") + (r.threshold > 0.0 ? ":" + number(r.threshold) : "");
    }
    return text;
}

// Errors refuse a save. Warnings are shown and saved (they depend on the format).
inline void validate(const Ladder &l, const std::vector<RungState> &s,
                     std::vector<std::string> &errors, std::vector<std::string> &warnings)
{
    const auto name = [](size_t i) { return "Rung " + std::to_string(i + 1); };
    if (l.rungs.size() > 8)
        errors.push_back("At most 8 rungs.");
    double lastThreshold = 0.0, reached = 0.0;
    size_t lastThresholdRung = 0;
    for (size_t i = 0; i < l.rungs.size(); ++i) {
        const Rung &r = l.rungs[i];
        for (size_t j = 0; j < i; ++j)
            if (sameMode(r, l.rungs[j]))
                errors.push_back(name(i) + " repeats rung " + std::to_string(j + 1) + ".");
        if (r.threshold > 0.0) {
            if (lastThreshold > 0.0 && r.threshold <= lastThreshold)
                errors.push_back("Thresholds must rise down the list: " + name(i) + " (" + number(r.threshold) +
                                 " fps) is not above rung " + std::to_string(lastThresholdRung + 1) + " (" +
                                 number(lastThreshold) + " fps).");
            lastThreshold = r.threshold;
            lastThresholdRung = i;
        }
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
            warnings.push_back(name(i) + " is never used: an earlier rung already reaches " + number(reached) + " fps.");
        reached = std::max(reached, ceiling);
        if (i > 0) {
            const Rung &p = l.rungs[i - 1];
            if (p.clearHdr != r.clearHdr || p.bits != r.bits)
                warnings.push_back("Rung " + std::to_string(i) + " -> " + std::to_string(i + 1) + " changes " +
                                   label(p) + " to " + label(r) + ": the look changes between takes.");
            if (p.width != r.width || p.height != r.height)
                warnings.push_back("Rung " + std::to_string(i) + " -> " + std::to_string(i + 1) +
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
               ",\"usable\":" + (st.usable ? "true" : "false") + ",\"mode_ceiling\":" + number(st.modeCeiling);
    };
    std::string out = "{\"enabled\":" + std::string(l.enabled ? "true" : "false") +
        ",\"ramp_ready\":" + (l.rampReady ? "true" : "false") + ",\"rungs_text\":" + quote(rungsText(l.rungs)) +
        ",\"ceiling\":" + number(ladderCeiling(l, s)) + ",\"running_rung\":" + std::to_string(running + 1) +
        ",\"rungs\":[";
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

// mode_ladder.conf: one key per line, so a person can read it. Core rewrites it whole.
inline std::string fileText(const Ladder &l)
{
    return "# Apertar mode ladder (T34). Core writes this file; edit it in the WebUI (Settings > Record > Mode Ladder).\n"
           "enabled=" + std::string(l.enabled ? "true" : "false") + "\n"
           "ramp_ready=" + std::string(l.rampReady ? "true" : "false") + "\n"
           "rungs=" + rungsText(l.rungs) + "\n";
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
        else if (key == "rungs" && !parseRungs(value, l.rungs, error)) l.rungs.clear();
    }
    if (l.rungs.empty())
        l.enabled = false;  // a broken file never switches the camera
    return l;
}

} // namespace apertar::ladder
