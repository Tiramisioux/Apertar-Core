#pragma once

// Clip folder names from a user template (std only; no camera or filesystem code).
//
// A template is ONE string: {tokens} in braces, every other character literal. The default,
// "{format}_{date}_{clip}", makes exactly the names the camera always made (cDNG_26-10-28_C0001).
// Every token and every literal produces only [A-Za-z0-9._-], so a name is a safe folder name and a
// safe file prefix (DNG frames, <folder>.wav, <folder>.mp4 all derive from the folder name).
//
// Rules (also in apertar-kb): the template must contain {clip} exactly once (parse() appends "_{clip}"
// and reports corrected=true when it is missing); the name is at most 100 characters; runs of
// separators (_ - .) collapse to the first; a token that is empty (e.g. {log} when Log is off) is
// dropped together with the separator that would double up; a name never begins or ends with a separator.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace apertar::clipname {

inline constexpr const char *kDefaultTemplate = "{format}_{date}_{clip}";
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

// The next free clip number: the lowest N in 1..9999 whose expanded name does not exist yet (the rule the
// camera always used; with the default template it is byte-identical). Returns 0 if all are taken.
inline int nextClipNumber(const Parsed &parsed, const Values &values,
                          const std::function<bool(const std::string &)> &exists)
{
    for (int clip = 1; clip <= 9999; ++clip)
        if (!exists(expand(parsed, values, clip)))
            return clip;
    return 0;
}

} // namespace apertar::clipname
