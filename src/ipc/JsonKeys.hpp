#pragma once
// The relay's small key reader (dual-sensor design §4.1, P15, R6). Pure: std only, host-tested (json_keys_test.cpp).
//
// The flat parser in ControlProtocol.cpp stops a value at the first `}` or `,`, so it cannot read a get_state reply
// that carries nested objects and arrays. This reads the top-level members of one JSON object by key: a number, a bool
// or a string, and skips nested values (strings with escapes, objects and arrays, to any depth) without looking inside.
// A key that only occurs nested is not found. A malformed object gives the members read up to the fault.
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apertar::jsonkeys {

struct Member {
    std::string_view key;      // the key as written (no unescape)
    size_t begin = 0;          // offset of the key's opening quote
    size_t valueBegin = 0;     // offset of the value's first character
    size_t end = 0;            // offset just after the value
};

namespace detail {
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// `at` is on an opening quote; returns the offset just after the closing quote, or npos when it never closes.
inline size_t skipString(std::string_view text, size_t at)
{
    for (size_t i = at + 1; i < text.size(); ++i) {
        if (text[i] == '\\') ++i;
        else if (text[i] == '"') return i + 1;
    }
    return std::string_view::npos;
}

// `at` is on `{` or `[`; returns the offset just after its match, or npos.
inline size_t skipNested(std::string_view text, size_t at)
{
    int depth = 0;
    for (size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            i = skipString(text, i);
            if (i == std::string_view::npos) return i;
            --i;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) return i + 1;
        }
    }
    return std::string_view::npos;
}
} // namespace detail

inline std::vector<Member> topLevel(std::string_view obj)
{
    std::vector<Member> members;
    size_t i = 0;
    while (i < obj.size() && detail::isSpace(obj[i])) ++i;
    if (i >= obj.size() || obj[i] != '{') return members;
    ++i;
    for (;;) {
        while (i < obj.size() && (detail::isSpace(obj[i]) || obj[i] == ',')) ++i;
        if (i >= obj.size() || obj[i] != '"') return members;
        Member m;
        m.begin = i;
        const size_t keyEnd = detail::skipString(obj, i);
        if (keyEnd == std::string_view::npos) return members;
        m.key = obj.substr(i + 1, keyEnd - i - 2);
        i = keyEnd;
        while (i < obj.size() && detail::isSpace(obj[i])) ++i;
        if (i >= obj.size() || obj[i] != ':') return members;
        ++i;
        while (i < obj.size() && detail::isSpace(obj[i])) ++i;
        if (i >= obj.size()) return members;
        m.valueBegin = i;
        if (obj[i] == '"') {
            m.end = detail::skipString(obj, i);
        } else if (obj[i] == '{' || obj[i] == '[') {
            m.end = detail::skipNested(obj, i);
        } else {
            size_t j = i;
            while (j < obj.size() && obj[j] != ',' && obj[j] != '}') ++j;
            while (j > i && detail::isSpace(obj[j - 1])) --j;
            m.end = j;
        }
        if (m.end == std::string_view::npos || m.end == i) return members;
        members.push_back(m);
        i = m.end;
    }
}

inline std::optional<Member> find(std::string_view obj, std::string_view key)
{
    for (const Member &m : topLevel(obj))
        if (m.key == key) return m;
    return std::nullopt;
}

// The value's text as written ("\"ready\"" keeps its quotes).
inline std::optional<std::string_view> rawOf(std::string_view obj, std::string_view key)
{
    const auto m = find(obj, key);
    if (!m) return std::nullopt;
    return obj.substr(m->valueBegin, m->end - m->valueBegin);
}

inline std::optional<double> numberOf(std::string_view obj, std::string_view key)
{
    const auto raw = rawOf(obj, key);
    if (!raw || raw->empty() || (*raw)[0] == '"' || (*raw)[0] == '{' || (*raw)[0] == '[') return std::nullopt;
    const std::string text(*raw);
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return value;
}

inline std::optional<bool> boolOf(std::string_view obj, std::string_view key)
{
    const auto raw = rawOf(obj, key);
    if (raw && *raw == "true") return true;
    if (raw && *raw == "false") return false;
    return std::nullopt;
}

inline std::optional<std::string> stringOf(std::string_view obj, std::string_view key)
{
    const auto raw = rawOf(obj, key);
    if (!raw || raw->size() < 2 || (*raw)[0] != '"') return std::nullopt;
    std::string out;
    for (size_t i = 1; i + 1 < raw->size(); ++i) {
        const char c = (*raw)[i];
        if (c != '\\') { out.push_back(c); continue; }
        const char e = (*raw)[++i];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            // BMP only; a bad escape keeps the characters as written.
            unsigned code = 0;
            bool good = i + 4 < raw->size();
            for (int k = 1; good && k <= 4; ++k) {
                const char h = (*raw)[i + k];
                code <<= 4;
                if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                else good = false;
            }
            if (!good) { out += "\\u"; break; }
            i += 4;
            if (code <= 0x7f) out.push_back(static_cast<char>(code));
            else if (code <= 0x7ff) {
                out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else {
                out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
            break;
        }
        default: out.push_back(e); break;   // \" \\ \/
        }
    }
    return out;
}

// The members from `firstKey` through `lastKey`, as written, for a body that carries one writer's run of members at the
// top level (cam1.settings is cam1's settingsBody cut out of its get_state reply). Empty when either key is missing or
// they are out of order.
inline std::string run(std::string_view obj, std::string_view firstKey, std::string_view lastKey)
{
    const auto first = find(obj, firstKey);
    const auto last = find(obj, lastKey);
    if (!first || !last || last->end < first->begin) return {};
    return std::string(obj.substr(first->begin, last->end - first->begin));
}

} // namespace apertar::jsonkeys
