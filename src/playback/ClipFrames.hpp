#pragma once

// Apertar playback, design sketch v2 (not in any repository).
// The frame list of one clip folder, and the map from time to frame (critics M1, M7; operator P1).
//
// Core numbers a cDNG frame by its sensor time: llround((ts - first) * fps / 1e6)
// (CameraController.cpp:3780-3787), so a frame the writer never wrote leaves a hole in the file
// numbers. The take's WAV spans the same time: first frame to last frame + one period
// (AudioRecordingSession.cpp:382-384, :606-626). So sound time t belongs to the frame NUMBER
// first + floor(t * fps), not to the position floor(t * fps) in the list of files. The touch UI
// and Cinemate use the position (playbackcontroller.cpp:849-851; cinemate playback.py:265), which
// runs the picture ahead of the sound on any take with drops.
//
// What stays as on the touch screen (parity, operator P15): the list is every *.dng in the folder,
// 0-byte files included (playbackcontroller.cpp:696-706, cliplistmodel.cpp:502, :530), so FRAMES and
// "FRAME i / N" count the same files on both screens. A 0-byte frame is shown like a hole: the
// last frame with picture stays up.
// What differs (D15, until U9 brings the touch UI along): the clock maps through the numbers, and
// DURATION is the span (last - first + 1) / fps, which is also the WAV's length.
//
// Timelapse folders are numbered by a counter (CameraController.cpp:1038-1047) and T31 ramp takes
// by count, so for them number == position and nothing changes. A timelapse shot as DNG + JPEG
// pairs gives one frame per stem, the DNG first (critic m9; the touch UI lists *.dng only).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace apertar::playback {

struct FrameFile {
    std::string name;        // file name inside the clip folder
    uint64_t number = 0;     // from "<clip>_<digits>.dng"; the position when a name has no number
    uint64_t bytes = 0;
};

struct FrameList {
    std::vector<FrameFile> files;            // sorted by number, then name
    bool numbered = false;                   // every name carried a number
    uint64_t first = 0, last = 0;            // numbers of the first and last file
    uint64_t empty = 0;                      // 0-byte files (a pulled card leaves ~21-23)
    std::vector<std::pair<uint64_t, uint64_t>> gaps;   // [first missing number, how many]

    uint64_t count() const { return files.size(); }                    // FRAMES, the "N" of FRAME i / N
    uint64_t span() const { return files.empty() ? 0 : last - first + 1; }
    uint64_t missing() const { return span() - count(); }              // numbers never written
    double durationSeconds(double fps) const { return fps > 0 ? double(span()) / fps : 0.0; }

    // Position (0-based) of the file whose number is the largest at or below `number`. This is the
    // "i" of FRAME i / N: over a hole it holds the frame before the hole.
    size_t positionAtNumber(uint64_t number) const
    {
        if (files.empty()) return 0;
        auto it = std::upper_bound(files.begin(), files.end(), number,
                                   [](uint64_t n, const FrameFile &f) { return n < f.number; });
        return it == files.begin() ? 0 : size_t(it - files.begin()) - 1;
    }
    // The file to decode for that position: itself, or the last one before it with bytes.
    // Returns count() when no frame up to there has a picture.
    size_t pictureAtPosition(size_t position) const
    {
        for (size_t p = std::min(position + 1, files.size()); p-- > 0;)
            if (files[p].bytes > 0) return p;
        return files.size();
    }
    // Timeline number for clock time t (seconds since the first frame).
    uint64_t numberAtTime(double t, double fps) const
    {
        if (files.empty() || fps <= 0 || t <= 0) return first;
        const uint64_t offset = uint64_t(t * fps);           // floor; the WAV starts at frame `first`
        return std::min(last, first + offset);
    }
};

namespace detail {

inline bool endsWithNoCase(const std::string &s, const char *suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    return true;
}

// "<clip>_000000123.dng" -> 123. False when the stem does not end in _<digits>.
inline bool frameNumberFromName(const std::string &name, uint64_t &number)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    size_t i = dot;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) --i;
    if (i == dot || i == 0 || name[i - 1] != '_' || dot - i > 18) return false;
    number = std::stoull(name.substr(i, dot - i));
    return true;
}

} // namespace detail

// One readdir of the folder; sizes from the directory entries (one stat each on exFAT, PROBABLE).
inline FrameList listFrames(const std::filesystem::path &dir, std::string *error = nullptr)
{
    FrameList out;
    std::error_code ec;
    struct Candidate { std::string stem, name; uint64_t bytes; bool dng; };
    std::vector<Candidate> found;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) { ec.clear(); continue; }
        const std::string name = entry.path().filename().string();
        const bool dng = detail::endsWithNoCase(name, ".dng");
        const bool jpg = detail::endsWithNoCase(name, ".jpg") || detail::endsWithNoCase(name, ".jpeg");
        if (!dng && !jpg) continue;
        const uint64_t bytes = entry.file_size(ec);
        if (ec) { ec.clear(); continue; }
        found.push_back({entry.path().stem().string(), name, bytes, dng});
    }
    if (ec && error) *error = "Could not read the clip folder: " + ec.message();
    // One frame per stem, the DNG first. A folder with DNGs ignores lone JPEGs (as the touch UI).
    const bool anyDng = std::any_of(found.begin(), found.end(), [](const Candidate &c) { return c.dng; });
    std::sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
        return a.stem != b.stem ? a.stem < b.stem : a.dng > b.dng;
    });
    std::string lastStem;
    bool haveLast = false;
    for (const Candidate &c : found) {
        if (haveLast && c.stem == lastStem) continue;
        if (anyDng && !c.dng) continue;
        lastStem = c.stem; haveLast = true;
        out.files.push_back({c.name, 0, c.bytes});
    }
    out.numbered = !out.files.empty();
    for (FrameFile &f : out.files)
        if (!detail::frameNumberFromName(f.name, f.number)) out.numbered = false;
    if (out.numbered) {
        std::stable_sort(out.files.begin(), out.files.end(), [](const FrameFile &a, const FrameFile &b) {
            return a.number < b.number;
        });
    } else {
        for (size_t i = 0; i < out.files.size(); ++i) out.files[i].number = i;   // name order
    }
    if (!out.files.empty()) {
        out.first = out.files.front().number;
        out.last = out.files.back().number;
    }
    for (size_t i = 0; i < out.files.size(); ++i) {
        if (out.files[i].bytes == 0) ++out.empty;
        if (i > 0 && out.files[i].number > out.files[i - 1].number + 1)
            out.gaps.push_back({out.files[i - 1].number + 1, out.files[i].number - out.files[i - 1].number - 1});
    }
    return out;
}

} // namespace apertar::playback
