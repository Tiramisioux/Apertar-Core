#pragma once

// Clip folder names from a user template (std only; no camera or filesystem code).
//
// A template is ONE string: {tokens} in braces, every other character literal. The default,
// "{format}_{date}_{clip}", makes exactly the names the camera always made (cDNG_26-10-28_C0001). The UIs' first
// preset, kStandardTemplate, adds the take's start timecode (HHMMSSFF, the {tc} token): cDNG_26-10-28_C0001_01400812.
// Every token and every literal produces only [A-Za-z0-9._-], so a name is a safe folder name and a
// safe file prefix (DNG frames, <folder>.wav, <folder>.mp4 all derive from the folder name).
//
// Rules (also in apertar-kb): the template must contain {clip} exactly once (parse() appends "_{clip}"
// and reports corrected=true when it is missing); the name is at most 100 characters; runs of
// separators (_ - .) collapse to the first; a token that is empty (e.g. {log} when Log is off) is
// dropped together with the separator that would double up; a name never begins or ends with a separator.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace apertar::clipname {

inline constexpr const char *kDefaultTemplate = "{format}_{date}_{clip}";
inline constexpr const char *kStandardTemplate = "{format}_{date}_{clip}_{tc}";   // the WebUI's and the touch UI's "Standard" preset
inline constexpr std::size_t kMaxTemplateChars = 200;
inline constexpr std::size_t kMaxNameChars = 100;
inline constexpr std::size_t kMaxTextChars = 24;

// Every token name the template accepts (without braces), in palette order. {text:..} is separate.
inline const std::vector<std::string> &tokenNames()
{
    static const std::vector<std::string> names{
        "format", "date", "date_", "yy", "mm", "dd", "time", "time-", "tc", "tc-", "clip",
        "cam", "sensor", "model", "res", "fps", "bits", "comp", "log"};
    return names;
}

inline bool isSeparator(char c) { return c == '_' || c == '-' || c == '.'; }

inline bool isNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || isSeparator(c);
}

// Anything into [A-Za-z0-9._-]: other characters become '-', runs collapse, ends are trimmed, capped.
inline std::string sanitize(const std::string &in, std::size_t maxChars = 16)
{
    std::string out;
    for (char c : in) {
        const char o = isNameChar(c) ? c : '-';
        if (o == '-' && !out.empty() && out.back() == '-')
            continue;
        out.push_back(o);
    }
    while (!out.empty() && isSeparator(out.front())) out.erase(out.begin());
    while (!out.empty() && isSeparator(out.back())) out.pop_back();
    if (out.size() > maxChars) out.resize(maxChars);
    while (!out.empty() && isSeparator(out.back())) out.pop_back();
    return out;
}

// What a take knows when its folder is named. Strings are already sanitised; an empty one drops the token.
struct Values {
    int year = 0;    // two digits, 0-99
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int frame = 0;   // frame index within the second at the current fps (the timecode FF)
    std::string format = "cDNG"; // cDNG, cDNG_3to1, cDNG_5to1, MLV, ProRes, H264
    std::string cam = "cam0";
    std::string sensor;
    std::string model;
    std::string res;  // 4K, 2K, 1080p ...
    std::string fps;  // 25fps
    std::string bits; // 12bit
    std::string comp; // lossless, none, lossy3, lossy5
    std::string log;  // Log or empty
};

struct Part {
    enum class Kind { Literal, Token, Text };
    Kind kind = Kind::Literal;
    std::string text; // Literal: the characters; Token: the name without braces; Text: the free text
};

struct Parsed {
    bool ok = false;
    bool corrected = false;   // {clip} was missing and has been appended
    std::string error;        // set when !ok, names the offending character or token
    std::string normalized;   // the template to store
    std::vector<Part> parts;
};

inline std::string charDescription(char c)
{
    if (c == ' ') return "' ' (space)";
    if (static_cast<unsigned char>(c) < 0x20) return "a control character";
    if (static_cast<unsigned char>(c) >= 0x80) return "a non-ASCII character";
    return std::string("'") + c + "'";
}

inline std::string two(int v)
{
    std::string s = std::to_string(v < 0 ? 0 : v);
    if (s.size() < 2) s.insert(0, 2 - s.size(), '0');
    return s;
}

inline std::string clipLabel(int clip)
{
    std::string s = std::to_string(clip < 0 ? 0 : clip);
    if (s.size() < 4) s.insert(0, 4 - s.size(), '0');
    return "C" + s;
}

inline std::string tokenValue(const std::string &name, const Values &v, int clip)
{
    if (name == "format") return v.format;
    if (name == "date") return two(v.year) + "-" + two(v.month) + "-" + two(v.day);
    if (name == "date_") return two(v.year) + "_" + two(v.month) + "_" + two(v.day);
    if (name == "yy") return two(v.year);
    if (name == "mm") return two(v.month);
    if (name == "dd") return two(v.day);
    if (name == "time") return two(v.hour) + two(v.minute) + two(v.second);
    if (name == "time-") return two(v.hour) + "-" + two(v.minute) + "-" + two(v.second);
    if (name == "tc") return two(v.hour) + two(v.minute) + two(v.second) + two(v.frame);
    if (name == "tc-") return two(v.hour) + "-" + two(v.minute) + "-" + two(v.second) + "-" + two(v.frame);
    if (name == "clip") return clipLabel(clip);
    if (name == "cam") return v.cam;
    if (name == "sensor") return v.sensor;
    if (name == "model") return v.model;
    if (name == "res") return v.res;
    if (name == "fps") return v.fps;
    if (name == "bits") return v.bits;
    if (name == "comp") return v.comp;
    if (name == "log") return v.log;
    return {};
}

// The name a template makes for these values and this clip number. No length check (see parse()).
inline std::string expand(const std::vector<Part> &parts, const Values &values, int clip)
{
    std::string out;
    bool lastSep = true; // true at the start: a leading separator is dropped
    for (const Part &part : parts) {
        if (part.kind == Part::Kind::Literal) {
            for (char c : part.text) {
                if (isSeparator(c)) {
                    if (!lastSep) { out.push_back(c); lastSep = true; }
                } else {
                    out.push_back(c);
                    lastSep = false;
                }
            }
            continue;
        }
        std::string s = part.kind == Part::Kind::Text ? part.text : tokenValue(part.text, values, clip);
        if (lastSep)
            while (!s.empty() && isSeparator(s.front())) s.erase(s.begin());
        if (s.empty()) continue;
        out += s;
        lastSep = isSeparator(s.back());
    }
    while (!out.empty() && isSeparator(out.back())) out.pop_back();
    return out;
}

inline std::string expand(const Parsed &parsed, const Values &values, int clip)
{
    return expand(parsed.parts, values, clip);
}

// The longest name the sanitised values can make; parse() uses it for the 100-character rule.
inline Values worstCaseValues()
{
    Values v;
    v.year = 99; v.month = 12; v.day = 28; v.hour = 23; v.minute = 59; v.second = 59; v.frame = 119;
    v.format = "cDNG_3to1";
    v.cam = "cam0";
    v.sensor = std::string(12, 'S');
    v.model = std::string(16, 'M');
    v.res = "1080p";
    v.fps = "119.88fps";
    v.bits = "12bit";
    v.comp = "lossless";
    v.log = "Log";
    return v;
}

inline bool knownToken(const std::string &name)
{
    const auto &names = tokenNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

inline std::string serialize(const std::vector<Part> &parts)
{
    std::string out;
    for (const Part &p : parts) {
        switch (p.kind) {
        case Part::Kind::Literal: out += p.text; break;
        case Part::Kind::Token: out += "{" + p.text + "}"; break;
        case Part::Kind::Text: out += "{text:" + p.text + "}"; break;
        }
    }
    return out;
}

// Validate and normalise. A template without {clip} gets "_{clip}" appended (corrected=true).
inline Parsed parse(const std::string &tmpl)
{
    Parsed r;
    if (tmpl.empty()) { r.error = "The clip name template is empty."; return r; }
    if (tmpl.size() > kMaxTemplateChars) {
        r.error = "The clip name template is longer than " + std::to_string(kMaxTemplateChars) + " characters.";
        return r;
    }
    std::string literal;
    auto flush = [&]() {
        if (!literal.empty()) { r.parts.push_back({Part::Kind::Literal, literal}); literal.clear(); }
    };
    int clipCount = 0;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (c == '{') {
            const std::size_t close = tmpl.find('}', i + 1);
            if (close == std::string::npos) { r.error = "Unclosed '{' in the clip name template."; return r; }
            const std::string inner = tmpl.substr(i + 1, close - i - 1);
            flush();
            if (inner.rfind("text:", 0) == 0) {
                const std::string text = inner.substr(5);
                if (text.empty()) { r.error = "{text:} needs some text."; return r; }
                if (text.size() > kMaxTextChars) {
                    r.error = "Text '" + text + "' is longer than " + std::to_string(kMaxTextChars) + " characters.";
                    return r;
                }
                for (char t : text)
                    if (!isNameChar(t)) {
                        r.error = "Character " + charDescription(t) + " is not allowed in clip name text (letters, digits, . _ - only).";
                        return r;
                    }
                r.parts.push_back({Part::Kind::Text, text});
            } else if (knownToken(inner)) {
                if (inner == "clip") ++clipCount;
                r.parts.push_back({Part::Kind::Token, inner});
            } else {
                r.error = "Unknown token {" + inner + "}.";
                return r;
            }
            i = close;
        } else if (c == '}') {
            r.error = "Character '}' is not allowed outside a token.";
            return r;
        } else if (isNameChar(c)) {
            literal.push_back(c);
        } else {
            r.error = "Character " + charDescription(c) + " is not allowed in a clip name (letters, digits, . _ - only).";
            return r;
        }
    }
    flush();
    if (clipCount > 1) { r.error = "{clip} may appear only once."; return r; }
    if (clipCount == 0) {
        r.parts.push_back({Part::Kind::Literal, "_"});
        r.parts.push_back({Part::Kind::Token, "clip"});
        r.corrected = true;
    }
    const std::string worst = expand(r.parts, worstCaseValues(), 9999);
    if (worst.size() > kMaxNameChars) {
        r.error = "The template can make names of " + std::to_string(worst.size()) + " characters; the limit is " +
                  std::to_string(kMaxNameChars) + ".";
        return r;
    }
    // The media library tells Timelapse_* folders from clips by this prefix.
    if (worst.rfind("Timelapse", 0) == 0) {
        r.error = "A clip name cannot start with \"Timelapse\" (that prefix is for timelapse folders).";
        return r;
    }
    r.normalized = serialize(r.parts);
    r.ok = true;
    return r;
}

// ---- Two sensors (dual-sensor design §6.2) ----------------------------------------------------------------------
// With cam1 configured every clip name holds {cam}. A template without it gets "_{cam}" appended when the names are
// built; the stored template is never rewritten.
inline constexpr const char *kTwoCameraNameTooLong = "Clip name too long for two cameras. Shorten the clip name.";
inline constexpr const char *kInvalidClipName = "clip_name is not a valid clip name.";

inline std::string ensureCamToken(const std::string &tmpl)
{
    if (tmpl.find("{cam}") != std::string::npos)
        return tmpl;
    return tmpl + "_{cam}";
}

// parse() of the template as the two-camera names use it. When only the length fails the error is the fixed
// two-camera text, so every path (take, set_clip_name_template, apply_preset, preview_clip_name) refuses alike.
// Other errors (a bad character, an unknown token) keep parse()'s text. Parsed::normalized is the template with
// the token, so callers that need the STORED template keep their own string.
inline Parsed parseForTwoCameras(const std::string &tmpl)
{
    Parsed parsed = parse(ensureCamToken(tmpl));
    if (!parsed.ok && parsed.error.rfind("The template can make names of", 0) == 0)
        parsed.error = kTwoCameraNameTooLong;
    return parsed;
}

// A verbatim clip_name (sent by the primary to cam1, or hand-made): every character passes isNameChar, 1 to
// kMaxNameChars characters, no leading or trailing separator. cam1 never runs it through parse().
inline bool validVerbatimName(const std::string &name)
{
    if (name.empty() || name.size() > kMaxNameChars)
        return false;
    for (char c : name)
        if (!isNameChar(c))
            return false;
    return !isSeparator(name.front()) && !isSeparator(name.back());
}

// Tokens whose value changes from one take to the next on the same day.
inline bool isTimeToken(const std::string &name)
{
    return name == "time" || name == "time-" || name == "tc" || name == "tc-";
}

inline bool hasTimeToken(const Parsed &parsed)
{
    for (const Part &p : parsed.parts)
        if (p.kind == Part::Kind::Token && isTimeToken(p.text))
            return true;
    return false;
}

namespace detail {

// '\x01' in `pattern` stands for one time value: one or more of [0-9-].
inline bool matchTimeWildcards(const std::string &pattern, std::size_t p, const std::string &name, std::size_t i)
{
    if (p == pattern.size())
        return i == name.size();
    if (pattern[p] == '\x01') {
        for (std::size_t j = i; j < name.size() && ((name[j] >= '0' && name[j] <= '9') || name[j] == '-'); ++j)
            if (matchTimeWildcards(pattern, p + 1, name, j + 1))
                return true;
        return false;
    }
    return i < name.size() && name[i] == pattern[p] && matchTimeWildcards(pattern, p + 1, name, i + 1);
}

} // namespace detail

// True when `name` is what this template makes for this clip number at any time of the same day: the time tokens
// match any time, and a name without them (the same template minus the time, e.g. cDNG_26-10-28_C0001 for
// "{format}_{date}_{clip}_{tc}") counts too.
inline bool sameClipAnyTime(const Parsed &parsed, const Values &values, int clip, const std::string &name)
{
    std::vector<Part> wild = parsed.parts, timeless;
    for (Part &p : wild)
        if (p.kind == Part::Kind::Token && isTimeToken(p.text)) { p.kind = Part::Kind::Text; p.text = "\x01"; }
    for (const Part &p : parsed.parts)
        if (!(p.kind == Part::Kind::Token && isTimeToken(p.text))) timeless.push_back(p);
    return detail::matchTimeWildcards(expand(wild, values, clip), 0, name, 0) || expand(timeless, values, clip) == name;
}

// The next free clip number: the lowest N in 1..9999 whose expanded name does not exist yet (the rule the camera
// always used). A template with a time token makes a new name at every take, so for it N is also taken when any of
// `existingNames` (the folders on the medium) is clip N at another time of the day (sameClipAnyTime). Returns 0 if all
// are taken.
inline int nextClipNumber(const Parsed &parsed, const Values &values,
                          const std::function<bool(const std::string &)> &exists,
                          const std::vector<std::string> &existingNames = {})
{
    const bool timed = hasTimeToken(parsed);
    for (int clip = 1; clip <= 9999; ++clip) {
        if (exists(expand(parsed, values, clip)))
            continue;
        if (timed && std::any_of(existingNames.begin(), existingNames.end(),
                                 [&](const std::string &n) { return sameClipAnyTime(parsed, values, clip, n); }))
            continue;
        return clip;
    }
    return 0;
}

// ---- Name values shared by the primary's own name and cam1's (dual-sensor design §6.2) ------------------------------------
// The helpers CameraController's clipNameValues() uses, in one place, so the name the primary builds for cam1 from
// cam1.settings reads the same as the name cam0 builds from its own.

// {res}: 8K, 4K, 2K, 1080p, otherwise "<height>p"; empty without a height.
inline std::string resolutionLabel(int width, int height)
{
    if (width >= 7000) return "8K";
    if (width >= 3500) return "4K";
    if (width >= 1900 && width < 2000 && height <= 1100) return "1080p";
    if (width >= 2000 && width < 3500) return "2K";
    return height > 0 ? std::to_string(height) + "p" : std::string();
}

// {fps}: 25fps, 23.976fps.
inline std::string fpsLabel(double fps)
{
    const double rounded = std::round(fps);
    std::ostringstream f;
    if (std::fabs(fps - rounded) < 0.001) f << static_cast<long>(rounded);
    else f << std::fixed << std::setprecision(3) << fps;
    std::string text = f.str();
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text + "fps";
}

// The date, the time and the timecode frame (at `fps`) of a wall-clock instant, in the local zone.
inline void fillTimeValues(Values &v, int64_t micros, double fps)
{
    const std::time_t seconds = static_cast<std::time_t>(micros / 1000000);
    std::tm localTime{};
    localtime_r(&seconds, &localTime);
    v.year = localTime.tm_year % 100;
    v.month = localTime.tm_mon + 1;
    v.day = localTime.tm_mday;
    v.hour = localTime.tm_hour;
    v.minute = localTime.tm_min;
    v.second = localTime.tm_sec;
    const double useFps = fps > 0.0 ? fps : 24.0;
    const int nominalFps = std::max(1, static_cast<int>(std::llround(useFps)));
    v.frame = std::min(nominalFps - 1, static_cast<int>(static_cast<double>(micros % 1000000) * useFps / 1000000.0));
}

// cam1's side of a two-camera name: everything but the date and the time, which are the take's.
struct PartnerNaming {
    bool present = false;
    Values values;          // cam = "cam1"; date, time and frame are filled per take
    double fps = 25.0;
};

struct TwoNames {
    bool ok = false;
    std::string error;
    int clip = 0;
    std::string cam0;
    std::string cam1;
};

// The two names of a take (design 6.2 step 2): one clip number, the lowest free under both names, with `{cam}` in both. The
// date, the time and {tc}'s second are the take's; {tc}'s frame is at each camera's own fps. A template parse() rejects
// falls back to ensureCamToken(kDefaultTemplate); one that only fails the length once `_{cam}` is appended is refused.
inline TwoNames nameTwoCameras(const std::string &templateText, Values v0, const PartnerNaming &partner, int64_t micros, double fps0,
                               const std::function<bool(const std::string &)> &exists,
                               const std::function<std::vector<std::string>()> &listNames)
{
    TwoNames out;
    Parsed parsed = parseForTwoCameras(templateText);
    if (!parsed.ok) {
        if (parsed.error == kTwoCameraNameTooLong) {
            out.error = parsed.error;
            return out;
        }
        parsed = parse(ensureCamToken(kDefaultTemplate));
    }
    v0.cam = "cam0";
    fillTimeValues(v0, micros, fps0);
    Values v1 = partner.present ? partner.values : v0;
    v1.cam = "cam1";
    v1.year = v0.year; v1.month = v0.month; v1.day = v0.day; v1.hour = v0.hour; v1.minute = v0.minute; v1.second = v0.second;
    {
        Values frameOnly;
        fillTimeValues(frameOnly, micros, partner.present ? partner.fps : fps0);
        v1.frame = frameOnly.frame;
    }
    const bool timed = hasTimeToken(parsed);
    std::vector<std::string> existing;
    if (timed)
        existing = listNames();
    for (int clip = 1; clip <= 9999; ++clip) {
        const std::string n0 = expand(parsed, v0, clip);
        const std::string n1 = expand(parsed, v1, clip);
        if (exists(n0) || exists(n1))
            continue;
        if (timed && std::any_of(existing.begin(), existing.end(), [&](const std::string &n) {
                return sameClipAnyTime(parsed, v0, clip, n) || sameClipAnyTime(parsed, v1, clip, n);
            }))
            continue;
        out.ok = true;
        out.clip = clip;
        out.cam0 = n0;
        out.cam1 = n1;
        return out;
    }
    out.error = "Could not allocate a unique clip folder.";
    return out;
}

} // namespace apertar::clipname
