#pragma once

// Preview temporal denoise (TDN) off: a copy of a libcamera PiSP tuning file with every "tdn" member of the
// "rpi.denoise" algorithm removed. The recorded DNGs are the raw sensor frames and do not change; only the ISP
// output (HDMI, touch UI, WebUI, peaking, zebra) loses the blend with the previous frame, which makes the ISP
// back end much faster at 4K Clear HDR 16-bit.
//
// stripTdn() is a pure text-level remover: it keeps every byte of the source except the removed members and
// exactly one adjacent comma. previewTuningPath() is the file side: it keeps a validated copy in
// $HOME/.cache/apertar-core/tuning/<stem>_notdn.json and falls back to the source whenever anything is unsure.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "camera/DefaultTuningSelection.hpp"

namespace apertar::tuning {

enum class TdnStripStatus {
    Stripped,   // text holds the result, removed >= 1
    NoTdn,      // the source has no tdn member in rpi.denoise: nothing to do
    Failed      // not usable (not JSON, odd shape, a sole member...): reason says why
};

struct TdnStripResult {
    TdnStripStatus status = TdnStripStatus::Failed;
    std::string text;
    int removed = 0;
    std::string reason;
};

namespace tdn_detail {

struct Node;

struct Member {
    std::string key;     // raw text between the quotes
    size_t keyStart = 0; // offset of the opening quote
    std::unique_ptr<Node> value;
};

struct Node {
    char type = 0; // '{', '[', '"' or 'v' (number, true, false, null)
    size_t start = 0;
    size_t end = 0; // one past the last byte of the value
    std::vector<Member> members;
    std::vector<Node> elems;
};

class Parser {
public:
    explicit Parser(const std::string &text) : t_(text) {}

    bool parse(Node &root)
    {
        i_ = 0;
        if (!value(root, 0))
            return false;
        skipWs();
        return i_ == t_.size();
    }

    const std::string &error() const { return error_; }

private:
    bool fail(const char *why)
    {
        if (error_.empty())
            error_ = why;
        return false;
    }

    void skipWs()
    {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\r' || t_[i_] == '\n'))
            ++i_;
    }

    // At an opening quote; leaves i_ one past the closing quote. Braces, commas and escaped quotes inside are skipped.
    bool string(std::string *raw)
    {
        const size_t open = i_;
        ++i_;
        while (i_ < t_.size() && t_[i_] != '"')
            i_ += t_[i_] == '\\' ? 2 : 1;
        if (i_ >= t_.size())
            return fail("unterminated string");
        if (raw)
            raw->assign(t_, open + 1, i_ - open - 1);
        ++i_;
        return true;
    }

    bool value(Node &node, int depth)
    {
        if (depth > 64)
            return fail("nested too deep");
        skipWs();
        if (i_ >= t_.size())
            return fail("unexpected end");
        node.start = i_;
        const char c = t_[i_];
        if (c == '{') {
            node.type = '{';
            ++i_;
            skipWs();
            if (i_ < t_.size() && t_[i_] == '}') {
                ++i_;
                node.end = i_;
                return true;
            }
            while (true) {
                skipWs();
                if (i_ >= t_.size() || t_[i_] != '"')
                    return fail("expected a key");
                Member member;
                member.keyStart = i_;
                if (!string(&member.key))
                    return false;
                skipWs();
                if (i_ >= t_.size() || t_[i_] != ':')
                    return fail("expected ':'");
                ++i_;
                member.value = std::make_unique<Node>();
                if (!value(*member.value, depth + 1))
                    return false;
                node.members.push_back(std::move(member));
                skipWs();
                if (i_ < t_.size() && t_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == '}') {
                    ++i_;
                    node.end = i_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            node.type = '[';
            ++i_;
            skipWs();
            if (i_ < t_.size() && t_[i_] == ']') {
                ++i_;
                node.end = i_;
                return true;
            }
            while (true) {
                node.elems.emplace_back();
                if (!value(node.elems.back(), depth + 1))
                    return false;
                skipWs();
                if (i_ < t_.size() && t_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == ']') {
                    ++i_;
                    node.end = i_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            node.type = '"';
            if (!string(nullptr))
                return false;
            node.end = i_;
            return true;
        }
        node.type = 'v';
        while (i_ < t_.size() && std::string(",]} \t\r\n").find(t_[i_]) == std::string::npos)
            ++i_;
        if (i_ == node.start)
            return fail("empty value");
        node.end = i_;
        return true;
    }

    const std::string &t_;
    size_t i_ = 0;
    std::string error_;
};

inline const Node *find(const Node &object, const char *key)
{
    if (object.type != '{')
        return nullptr;
    for (const Member &member : object.members) {
        if (member.key == key)
            return member.value.get();
    }
    return nullptr;
}

// The configs libcamera reads from every rpi.denoise: the mode objects (normal, hdr, night...) when a "normal"
// exists, else the flat object itself.
inline std::vector<const Node *> denoiseConfigs(const Node &root)
{
    std::vector<const Node *> configs;
    const Node *algorithms = find(root, "algorithms");
    if (!algorithms || algorithms->type != '[')
        return configs;
    for (const Node &algorithm : algorithms->elems) {
        const Node *denoise = find(algorithm, "rpi.denoise");
        if (!denoise || denoise->type != '{')
            continue;
        if (find(*denoise, "normal")) {
            for (const Member &mode : denoise->members) {
                if (mode.value->type == '{')
                    configs.push_back(mode.value.get());
            }
        } else {
            configs.push_back(denoise);
        }
    }
    return configs;
}

inline size_t countTdnMembers(const Node &root)
{
    size_t count = 0;
    for (const Node *config : denoiseConfigs(root)) {
        for (const Member &member : config->members)
            count += member.key == "tdn" ? 1 : 0;
    }
    return count;
}

inline std::string rawText(const std::string &text, const Node *node)
{
    return node ? text.substr(node->start, node->end - node->start) : std::string();
}

} // namespace tdn_detail

// Removes every "tdn" member of every rpi.denoise config. The member goes with its value and exactly one adjacent
// comma (the one before it, or, for the first member, the one after it); all other bytes are kept.
inline TdnStripResult stripTdn(const std::string &text)
{
    using namespace tdn_detail;
    TdnStripResult result;
    Node root;
    Parser parser(text);
    if (!parser.parse(root) || root.type != '{') {
        result.reason = "not a JSON object" + (parser.error().empty() ? std::string() : " (" + parser.error() + ")");
        return result;
    }

    std::vector<std::pair<size_t, size_t>> cuts;
    for (const Node *config : denoiseConfigs(root)) {
        int inThisConfig = 0;
        for (size_t k = 0; k < config->members.size(); ++k) {
            const Member &member = config->members[k];
            if (member.key != "tdn")
                continue;
            if (++inThisConfig > 1) {
                result.reason = "a denoise config has two tdn members";
                return result;
            }
            const size_t keyStart = member.keyStart;
            const size_t valueEnd = member.value->end;
            size_t b = keyStart;
            while (b > 0 && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n'))
                --b;
            if (b > 0 && text[b - 1] == ',') {
                cuts.emplace_back(b - 1, valueEnd);   // ', "tdn": {...}'
            } else {
                size_t a = valueEnd;
                while (a < text.size() && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n'))
                    ++a;
                if (a >= text.size() || text[a] != ',') {
                    result.reason = "tdn is the only member of its denoise config";
                    return result;
                }
                ++a;
                while (a < text.size() && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n'))
                    ++a;
                cuts.emplace_back(keyStart, a);       // '"tdn": {...},' and the space before the next member
            }
        }
    }

    if (cuts.empty()) {
        result.status = TdnStripStatus::NoTdn;
        result.text = text;
        return result;
    }

    std::sort(cuts.begin(), cuts.end());
    for (size_t k = 1; k < cuts.size(); ++k) {
        if (cuts[k].first < cuts[k - 1].second) {
            result.reason = "overlapping cuts";
            return result;
        }
    }
    std::string out = text;
    for (size_t k = cuts.size(); k-- > 0;)
        out.erase(cuts[k].first, cuts[k].second - cuts[k].first);

    // The result must still be one JSON object with the same target and no tdn left in any denoise config.
    Node check;
    Parser checker(out);
    if (!checker.parse(check) || check.type != '{') {
        result.reason = "the stripped text does not parse";
        return result;
    }
    if (countTdnMembers(check) != 0) {
        result.reason = "a tdn member is left after stripping";
        return result;
    }
    if (rawText(text, find(root, "target")) != rawText(out, find(check, "target"))) {
        result.reason = "the target changed";
        return result;
    }

    result.status = TdnStripStatus::Stripped;
    result.text = std::move(out);
    result.removed = static_cast<int>(cuts.size());
    return result;
}

namespace tdn_detail {

inline bool readFile(const std::filesystem::path &path, std::string &out)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 4 * 1024 * 1024)
        return false;
    std::ifstream input(path, std::ios::binary);
    out.assign(static_cast<size_t>(size), '\0');
    return static_cast<bool>(input.read(out.data(), static_cast<std::streamsize>(out.size())));
}

inline bool writeFileAtomic(const std::filesystem::path &path, const std::string &content, bool validate,
                            Pipeline pipeline, std::string &why)
{
    const std::filesystem::path temp = path.parent_path() / ("." + path.filename().string() + ".tmp." + std::to_string(::getpid()));
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            why = "could not write " + temp.string();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    if (validate && !compatibleFile(temp, pipeline, &why)) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        why = "could not rename to " + path.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

inline std::string identityOf(const std::filesystem::path &source)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    const auto mtime = std::filesystem::last_write_time(source, ec).time_since_epoch().count();
    return source.string() + "\n" + std::to_string(static_cast<unsigned long long>(size)) + "\n" +
        std::to_string(static_cast<long long>(mtime)) + "\n";
}

} // namespace tdn_detail

// The tuning file Core hands to libcamera when the preview's temporal denoise is off. `source` is the file Core
// resolved (automatic default, Clear HDR variant, or the user's pick). Returns the copy without tdn, or `source`
// itself when there is nothing to strip or anything fails; `note` then says why (empty on success).
// The copy is regenerated when the source's size or modification time changed (a sidecar file keeps them).
inline std::string previewTuningPath(const std::string &source, Pipeline pipeline, std::string &note)
{
    using namespace tdn_detail;
    note.clear();
    if (source.empty()) {
        note = "libcamera default tuning: no file to strip";
        return source;
    }
    const std::filesystem::path sourcePath(source);
    const char *home = std::getenv("HOME");
    if (!home || !*home) {
        note = "no HOME: using the source tuning with temporal denoise";
        return source;
    }
    const std::filesystem::path directory = std::filesystem::path(home) / ".cache" / "apertar-core" / "tuning";
    const std::filesystem::path target = directory / (sourcePath.stem().string() + "_notdn.json");
    const std::filesystem::path sidecar = directory / (sourcePath.stem().string() + "_notdn.json.src");
    const std::string identity = identityOf(sourcePath);

    std::string current;
    if (readFile(sidecar, current) && current == identity) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(target, ec) && compatibleFile(target, pipeline))
            return target.string();
    }

    std::string text;
    if (!readFile(sourcePath, text)) {
        note = "could not read " + source + ": using it unchanged";
        return source;
    }
    const TdnStripResult stripped = stripTdn(text);
    if (stripped.status == TdnStripStatus::NoTdn) {
        note = "the tuning file has no tdn member: using it unchanged";
        return source;
    }
    if (stripped.status == TdnStripStatus::Failed) {
        note = "could not strip tdn (" + stripped.reason + "): using the tuning file unchanged";
        return source;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        note = "could not create " + directory.string() + ": using the tuning file unchanged";
        return source;
    }
    std::string why;
    if (!writeFileAtomic(target, stripped.text, true, pipeline, why)) {
        note = "could not store the stripped tuning (" + why + "): using the tuning file unchanged";
        return source;
    }
    if (!writeFileAtomic(sidecar, identity, false, pipeline, why))
        note = "stripped copy written, but its source stamp was not (" + why + ")";
    return target.string();
}

} // namespace apertar::tuning
