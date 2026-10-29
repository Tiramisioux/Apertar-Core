#pragma once

// Apertar playback, T47 part B1 (reference for the tutorial; not in any repository until you type it).
// What Core knows about one cDNG clip folder, from the files themselves ("the file is the witness"):
// its own FPS, frame numbers and holes, span duration, recorded timecode, format text, shot date.
// The texts follow the touch UI's rules so both screens say one thing (design §4.2, §4.3, F22, F23).
//
// ClipFactsCache: an unchanged folder costs ONE stat (its modification time). On Patrik's exFAT card the
// folder's mtime moves with its newest entry (checked 2026-10-29), and a readdir of 105,000 entries costs
// 3.7 s, so counting entries as well would defeat the cache. A changed folder is probed OUTSIDE the lock
// and swapped in (critic m14).

#include "ClipFrames.hpp"
#include "DngTags.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace apertar::playback {

// ---- the touch UI's text rules (each names the touch UI lines it copies) ------------------------

// Card duration, CLM:147-169: mm:ss, hh:mm:ss from one hour, seconds rounded, "00:00" for nothing.
inline std::string durationCardText(double seconds)
{
    if (!(seconds > 0)) return "00:00";
    const long long total = std::llround(seconds);
    char buf[32];
    if (total >= 3600) std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", total / 60, total % 60);
    return buf;
}

// Sizes, base 1024, one decimal below 100 ("3.9 GB", "128 MB"), PC:1149-1205.
inline std::string sizeText(uint64_t bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    else if (v < 100.0) std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    return buf;
}

// "2026:10:28 14:03:12" -> "28 Oct 2026  14:03" (two spaces). English months: Core never calls setlocale (F23).
inline bool shotDate(const std::string &exif, std::string &text, int64_t &epochMs)
{
    int y, mo, d, h, mi, s;
    if (std::sscanf(exif.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6 || mo < 1 || mo > 12) return false;
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[48];
    std::snprintf(buf, sizeof buf, "%02d %s %04d  %02d:%02d", d, months[mo - 1], y, h, mi);
    text = buf;
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
    epochMs = int64_t(std::mktime(&tm)) * 1000;
    return true;
}

// SMPTE BCD, frame byte first (CinemaDNG TimeCodes 51043): "14:03:12:05". R1: the card shows it.
inline std::string timecodeText(const uint8_t tc[8])
{
    auto bcd = [](uint8_t b) { return (b >> 4) * 10 + (b & 15); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", bcd(tc[3]), bcd(tc[2]), bcd(tc[1]), bcd(tc[0]));
    return buf;
}

inline std::string clipToken(const std::string &name, uint64_t firstBytes, uint64_t count, int64_t coreStartMs)
{
    uint64_t h = 1469598103934665603ULL;               // FNV-1a
    auto mix = [&h](const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    mix(name.data(), name.size()); mix(&firstBytes, sizeof firstBytes); mix(&count, sizeof count); mix(&coreStartMs, sizeof coreStartMs);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", unsigned(h ^ (h >> 32)));
    return buf;
}

// ---- the facts ------------------------------------------------------------------------------

struct ClipFacts {
    bool ok = false;
    std::string error;
    std::string name;
    FrameList frames;
    DngInfo first;                    // the first frame that has bytes
    bool haveFirst = false;
    double fps = 24.0;                // the clip's own FrameRate tag; 24 as the touch UI when it has none
    bool fpsFallback = true;
    bool ramp = false;                // fps-ramp.txt is in the folder (T31)
    bool hasAudio = false;            // <clip>.wav is beside the frames
    bool companded = false;
    uint64_t bytes = 0;               // every file in the folder
    int64_t shotMs = 0;
    std::vector<std::pair<uint64_t, uint64_t>> holes;   // [first number, length]: never written + 0-byte, merged
    std::string durationText, fpsText, sizeText_, shotDateText, timecodeStart, formatText, droppedText;
    std::string token;
};

inline ClipFacts probeClip(const std::filesystem::path &dir, int64_t coreStartMs = 0)
{
    namespace fs = std::filesystem;
    ClipFacts f;
    f.name = dir.filename().string();
    f.frames = listFrames(dir, &f.error);
    if (f.frames.files.empty()) { f.error = f.error.empty() ? "No frames in the folder." : f.error; return f; }

    // One readdir is all this takes (listFrames); on this card a readdir of 105,000 entries is the cost (M4b).
    // The folder's size = the frames + the sound beside them + the ramp file; no second pass over the folder.
    std::error_code ec;
    for (const FrameFile &file : f.frames.files) f.bytes += file.bytes;
    for (const char *extra : {".wav", "_stretched.wav"}) {
        const auto sz = fs::file_size(dir / (f.name + extra), ec);
        if (!ec) { f.bytes += sz; if (extra[0] == '.') f.hasAudio = true; }
        ec.clear();
    }
    f.ramp = fs::exists(dir / "fps-ramp.txt", ec);
    if (f.ramp) { const auto sz = fs::file_size(dir / "fps-ramp.txt", ec); if (!ec) f.bytes += sz; }

    for (const FrameFile &file : f.frames.files) {         // the first frame with a picture
        if (file.bytes == 0) continue;
        const int fd = openDngFrame((dir / file.name).string().c_str());
        std::string err;
        f.haveFirst = readDngInfo(fd, f.first, err);
        closeDngFrame(fd);
        break;
    }
    if (f.haveFirst && f.first.fps > 0) { f.fps = f.first.fps; f.fpsFallback = false; }

    // holes: numbers never written, plus runs of 0-byte files, merged where they touch
    auto holes = f.frames.gaps;
    for (size_t i = 0; i < f.frames.files.size(); ++i) {
        if (f.frames.files[i].bytes != 0) continue;
        const uint64_t n = f.frames.files[i].number;
        if (!holes.empty() && holes.back().first + holes.back().second == n) ++holes.back().second;
        else holes.push_back({n, 1});
    }
    std::sort(holes.begin(), holes.end());
    for (const auto &h : holes) {
        if (!f.holes.empty() && f.holes.back().first + f.holes.back().second >= h.first)
            f.holes.back().second = std::max(f.holes.back().first + f.holes.back().second, h.first + h.second) - f.holes.back().first;
        else f.holes.push_back(h);
    }

    f.durationText = durationCardText(f.frames.durationSeconds(f.fps));       // the SPAN, not the files (D15)
    char fpsBuf[32];
    std::snprintf(fpsBuf, sizeof fpsBuf, "%g FPS", std::round(f.fps * 1000) / 1000);
    f.fpsText = fpsBuf;
    f.sizeText_ = sizeText(f.bytes);
    if (f.haveFirst) {
        shotDate(f.first.dateTimeOriginal, f.shotDateText, f.shotMs);
        if (f.first.hasTimecode) f.timecodeStart = timecodeText(f.first.timecode);
        // FORMAT row (design §4.3): 16-bit = Clear HDR; a LinearizationTable on 12-bit = companded
        const bool table = !f.first.linearization.empty();
        std::string look;
        if (f.first.bits == 16) look = "Clear HDR 16-bit";
        else if (f.name.find("cDNG_3to1_") == 0 && table) { look = "12-bit companded · lossy 3:1"; f.companded = true; }
        else if (f.name.find("cDNG_5to1_") == 0 && table) { look = "12-bit companded · lossy 5:1"; f.companded = true; }
        else if (table) { look = "Clear HDR 12-bit companded"; f.companded = true; }
        else look = "SDR " + std::to_string(f.first.bits) + "-bit";
        const bool lossy = f.name.find("cDNG_3to1_") == 0 || f.name.find("cDNG_5to1_") == 0;
        f.formatText = look + (lossy ? "" : (f.first.compression == 7 ? " · lossless" : " · uncompressed"));
    }
    if (f.ramp) f.droppedText = "unknown (speed ramp)";
    else if (f.frames.missing() == 0 && f.frames.empty == 0) f.droppedText = "none";
    else f.droppedText = std::to_string(f.frames.missing()) + " missing · " + std::to_string(f.frames.empty) + " empty";

    f.token = clipToken(f.name, f.frames.files.front().bytes, f.frames.count(), coreStartMs);
    f.ok = true;
    return f;
}

// ---- the cache --------------------------------------------------------------------------------

class ClipFactsCache {
public:
    explicit ClipFactsCache(int64_t coreStartMs = 0) : coreStartMs_(coreStartMs) {}

    // A folder is unchanged when its mtime is. exFAT: an added or deleted entry moves it (CONFIRMED on
    // three takes, 2026-10-29); a file written in place does not, and frames are never rewritten.
    ClipFacts get(const std::filesystem::path &dir)
    {
        struct stat st {};
        const bool statOk = ::stat(dir.c_str(), &st) == 0;
        const int64_t mtimeNs = statOk ? int64_t(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec : 0;
        const std::string key = dir.string();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(key);
            if (it != entries_.end() && it->second.mtimeNs == mtimeNs && statOk)
                return it->second.facts;
        }
        ClipFacts facts = probeClip(dir, coreStartMs_);           // outside the lock
        probes_.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = {mtimeNs, facts};
        return facts;
    }
    // The last facts for a folder, without probing: used while a hold lasts and nothing may read the card.
    bool peek(const std::filesystem::path &dir, ClipFacts &out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(dir.string());
        if (it == entries_.end()) return false;
        out = it->second.facts;
        return true;
    }
    int probes() const { return probes_.load(); }

private:
    struct Entry { int64_t mtimeNs = 0; ClipFacts facts; };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::atomic<int> probes_{0};
    int64_t coreStartMs_;
};

} // namespace apertar::playback
