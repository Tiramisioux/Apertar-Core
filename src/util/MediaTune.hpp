#pragma once

// The media tuning file: the write-path knobs (page-cache drop, per-frame writeback, sync interval, raw
// buffers, writers, queue depth, kernel tuning) as a small ini file instead of environment variables, so a
// test night needs no systemd drop-in. Pure std, no Linux-only includes, so tools/media_tune_test.cpp runs on
// any host. The glue to the recording medium (class and filesystem names) lives in MediaProfile.hpp.
//
// Two files: the base file /home/pi/apertar-media-tune.conf (kept, owned by pi) and the override file
// /run/apertar-media/media-tune.override.conf (tmpfs: gone at reboot, for test nights; the directory comes from
// config/apertar-media.tmpfiles.conf. Not /dev/shm: logind's RemoveIPC deletes a user's files there when the
// user's last session ends, 2026-11-03). Per key the first hit
// wins in this order: override [class.fs], [fs], [class], [default]; then base [class.fs], [fs], [class],
// [default]; else the key is unset and Core's own default stays. The environment variable of a knob wins over
// both files (A/B scripts rely on that); that rule lives at each read site. Files are read when a value is
// needed; the page-cache drop and the per-frame writeback are fixed for the life of the process.

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace apertar {

constexpr const char *kMediaTuneBasePath = "/home/pi/apertar-media-tune.conf";
constexpr const char *kMediaTuneOverridePath = "/run/apertar-media/media-tune.override.conf";

struct MediaTune {
    std::optional<std::string> dontneed;       // sync | writer | off
    std::optional<bool> writeback;
    std::optional<unsigned> syncMs;            // 0..60000
    std::optional<unsigned> rawBuffers;        // 3..16, absolute count
    std::optional<unsigned> extraRawBuffers;   // 0..8, on top of the base count
    std::optional<unsigned> workers;           // 1..8
    std::optional<unsigned> queue;             // 4..64
    std::optional<bool> kernelTune;
    std::map<std::string, std::string> source;   // key -> "override [pi-nvme.exfat]", "file [default]"
    std::vector<std::string> warnings;
    bool baseFound = false;
    bool overrideFound = false;

    std::string summary() const;
};

namespace mediatune {

inline std::string trim(const std::string &s)
{
    const char *ws = " \t\r\f\v";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string::npos)
        return std::string();
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

inline std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool isKnownKey(const std::string &key)
{
    return key == "dontneed" || key == "writeback" || key == "sync_ms" || key == "raw_buffers" ||
        key == "extra_raw_buffers" || key == "workers" || key == "queue" || key == "kernel_tune";
}

// The names mediaFilesystemName() / mediaClassName() print (MediaProfile.hpp), without "none" and "unknown".
inline bool isKnownFilesystem(const std::string &s)
{
    return s == "exfat" || s == "ext4" || s == "vfat" || s == "ntfs" || s == "ntfs-fuse" || s == "other";
}
inline bool isKnownClass(const std::string &s)
{
    return s == "pi-nvme" || s == "usb-uas" || s == "usb-storage" || s == "sd-card" || s == "other";
}
inline bool isKnownSection(const std::string &name)
{
    if (name == "default" || isKnownFilesystem(name) || isKnownClass(name))
        return true;
    const auto dot = name.find('.');
    return dot != std::string::npos && isKnownClass(name.substr(0, dot)) && isKnownFilesystem(name.substr(dot + 1));
}

// A whole number in [lo, hi], digits only.
inline bool parseUnsigned(const std::string &value, unsigned lo, unsigned hi, unsigned *out)
{
    if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
    if (parsed < lo || parsed > hi)
        return false;
    if (out)
        *out = static_cast<unsigned>(parsed);
    return true;
}

inline bool parseBool(const std::string &value, bool *out)
{
    if (value == "on" || value == "1" || value == "true") {
        if (out)
            *out = true;
        return true;
    }
    if (value == "off" || value == "0" || value == "false") {
        if (out)
            *out = false;
        return true;
    }
    return false;
}

// "" when the value is fine, else what is wrong with it (for the warning). `value` is already lower-cased.
inline std::string valueProblem(const std::string &key, const std::string &value)
{
    if (key == "dontneed")
        return (value == "sync" || value == "writer" || value == "off") ? "" : "is not sync, writer or off";
    if (key == "writeback" || key == "kernel_tune")
        return parseBool(value, nullptr) ? "" : "is not on, off, 1, 0, true or false";
    if (key == "sync_ms")
        return parseUnsigned(value, 0, 60000, nullptr) ? "" : "is not a whole number from 0 to 60000";
    if (key == "raw_buffers")
        return parseUnsigned(value, 3, 16, nullptr) ? "" : "is not a whole number from 3 to 16";
    if (key == "extra_raw_buffers")
        return parseUnsigned(value, 0, 8, nullptr) ? "" : "is not a whole number from 0 to 8";
    if (key == "workers")
        return parseUnsigned(value, 1, 8, nullptr) ? "" : "is not a whole number from 1 to 8";
    if (key == "queue")
        return parseUnsigned(value, 4, 64, nullptr) ? "" : "is not a whole number from 4 to 64";
    return "";
}

// section name -> key -> value (lower-cased). The last good line of a key in a section counts; a bad line is
// dropped with a warning and does not undo an earlier good one. Keys of an unknown section are skipped after
// one warning for the section. `label` is "file" or "override".
using ParsedConf = std::map<std::string, std::map<std::string, std::string>>;

inline ParsedConf parseMediaTuneText(const std::string &text, const std::string &label, const std::string &path,
                                     std::vector<std::string> *warnings)
{
    ParsedConf conf;
    std::string section = "default";
    bool sectionKnown = true;
    const auto warn = [&](const std::string &what) {
        if (warnings)
            warnings->push_back(what + " (" + label + " " + path + ")");
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            sectionKnown = isKnownSection(section);
            if (!sectionKnown)
                warn("unknown section [" + section + "] ignored");
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            warn("line without '=' ignored in section [" + section + "]: " + line);
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = lower(trim(line.substr(eq + 1)));
        if (!sectionKnown)
            continue;
        if (!isKnownKey(key)) {
            warn("unknown key " + key + "=" + value + " ignored in section [" + section + "]");
            continue;
        }
        const std::string problem = valueProblem(key, value);
        if (!problem.empty()) {
            warn("key " + key + "=" + value + " " + problem + ", ignored in section [" + section + "]");
            continue;
        }
        conf[section][key] = value;
    }
    return conf;
}

// The first hit of `key` for this medium in one parsed file, or nullptr. Order: class.fs, fs, class, default.
inline const std::string *lookup(const ParsedConf &conf, const std::string &key, const std::string &className,
                                 const std::string &fsName, std::string *sectionOut)
{
    std::vector<std::string> sections;
    if (!className.empty() && !fsName.empty())
        sections.push_back(className + "." + fsName);
    if (!fsName.empty())
        sections.push_back(fsName);
    if (!className.empty())
        sections.push_back(className);
    sections.push_back("default");
    for (const std::string &section : sections) {
        const auto s = conf.find(section);
        if (s == conf.end())
            continue;
        const auto k = s->second.find(key);
        if (k == s->second.end())
            continue;
        if (sectionOut)
            *sectionOut = section;
        return &k->second;
    }
    return nullptr;
}

} // namespace mediatune

// Pure: the two files' text (empty when absent) and the medium's names as mediaClassName() /
// mediaFilesystemName() print them. Override file first, then base file, per key.
inline MediaTune resolveMediaTune(const std::string &baseText, const std::string &overrideText, bool baseFound,
                                  bool overrideFound, const std::string &className, const std::string &fsName)
{
    using namespace mediatune;
    MediaTune tune;
    tune.baseFound = baseFound;
    tune.overrideFound = overrideFound;
    const ParsedConf over = parseMediaTuneText(overrideText, "override", kMediaTuneOverridePath, &tune.warnings);
    const ParsedConf base = parseMediaTuneText(baseText, "file", kMediaTuneBasePath, &tune.warnings);
    const std::string cls = lower(className);
    const std::string fs = lower(fsName);

    static const char *const keys[] = { "dontneed", "writeback", "sync_ms", "raw_buffers",
                                        "extra_raw_buffers", "workers", "queue", "kernel_tune" };
    for (const char *keyName : keys) {
        const std::string key = keyName;
        std::string section;
        const std::string *value = lookup(over, key, cls, fs, &section);
        std::string origin = "override";
        if (!value) {
            value = lookup(base, key, cls, fs, &section);
            origin = "file";
        }
        if (!value)
            continue;
        tune.source[key] = origin + " [" + section + "]";
        bool flag = false;
        unsigned number = 0;
        if (key == "dontneed")
            tune.dontneed = *value;
        else if (key == "writeback" && parseBool(*value, &flag))
            tune.writeback = flag;
        else if (key == "kernel_tune" && parseBool(*value, &flag))
            tune.kernelTune = flag;
        else if (key == "sync_ms" && parseUnsigned(*value, 0, 60000, &number))
            tune.syncMs = number;
        else if (key == "raw_buffers" && parseUnsigned(*value, 3, 16, &number))
            tune.rawBuffers = number;
        else if (key == "extra_raw_buffers" && parseUnsigned(*value, 0, 8, &number))
            tune.extraRawBuffers = number;
        else if (key == "workers" && parseUnsigned(*value, 1, 8, &number))
            tune.workers = number;
        else if (key == "queue" && parseUnsigned(*value, 4, 64, &number))
            tune.queue = number;
    }
    return tune;
}

namespace mediatune {

// A missing or unreadable file is "not found" and empty. Capped at 64 KiB: a config file, not a log.
inline bool readTextFile(const char *path, std::string *text)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open())
        return false;
    text->assign(std::string());
    char buffer[4096];
    while (text->size() < 65536 && in.read(buffer, sizeof buffer).gcount() > 0)
        text->append(buffer, static_cast<size_t>(in.gcount()));
    return true;
}

} // namespace mediatune

inline MediaTune loadMediaTune(const std::string &className, const std::string &fsName)
{
    std::string baseText, overrideText;
    const bool baseFound = mediatune::readTextFile(kMediaTuneBasePath, &baseText);
    const bool overrideFound = mediatune::readTextFile(kMediaTuneOverridePath, &overrideText);
    return resolveMediaTune(baseText, overrideText, baseFound, overrideFound, className, fsName);
}

inline std::string MediaTune::summary() const
{
    const auto num = [](const std::optional<unsigned> &v) { return v ? std::to_string(*v) : std::string("-"); };
    const auto flag = [](const std::optional<bool> &v) { return v ? std::string(*v ? "on" : "off") : std::string("-"); };
    std::string out = std::string("file=") + (baseFound ? kMediaTuneBasePath : "absent") +
        " override=" + (overrideFound ? kMediaTuneOverridePath : "absent") +
        " dontneed=" + (dontneed ? *dontneed : std::string("-")) +
        " writeback=" + flag(writeback) +
        " sync_ms=" + num(syncMs) +
        " raw_buffers=" + num(rawBuffers) +
        " extra_raw_buffers=" + num(extraRawBuffers) +
        " workers=" + num(workers) +
        " queue=" + num(queue) +
        " kernel_tune=" + flag(kernelTune) +
        " source: ";
    if (source.empty())
        return out + "-";
    bool first = true;
    for (const auto &entry : source) {
        out += (first ? "" : ",") + entry.first + "=" + entry.second;
        first = false;
    }
    return out;
}

} // namespace apertar
