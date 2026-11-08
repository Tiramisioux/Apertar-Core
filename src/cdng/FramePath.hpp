#pragma once

// Where a cDNG frame file goes inside its clip folder (std only; no camera or filesystem code).
//
// [storage] frames_per_folder=N (env APERTAR_CDNG_FRAMES_PER_FOLDER wins). N = 0, the default: every frame in the clip
// root, as always. N > 0: frame number f goes to <clip>/part_<k>/<the unchanged frame name>, k = f / N, at least
// three digits (part_000, part_001, ...). The frame name and its number do not change, so moving every part_*/*.dng
// back into the clip root (tools/flatten_clip.py) gives the flat clip. The WAV, fps-ramp.txt and every sidecar stay
// in the clip root. Why: an exFAT create walks every entry of its folder (5 per frame file) under the filesystem
// lock, so a 30-minute take in one folder pays a create that grows with the take (apertar-kb
// working/drop-frames/nvme-campaign/probe-exfat-dir.md).

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace apertar::framepath {

inline constexpr unsigned kMinFramesPerFolder = 100;
inline constexpr unsigned kMaxFramesPerFolder = 100000;
inline constexpr const char *kPartPrefix = "part_";
// Written in the clip root at take start when N > 0 ("frames_per_folder=N" and "pattern=<clip>_%09d.dng"): the clip
// is split, or partly merged, until the merge (Core's merge worker or tools/flatten_clip.py) removes it last.
inline constexpr const char *kMarkerName = ".parts";

// Why `value` is not a good frames_per_folder, or "" when it is: 0 (off) or a whole number from 100 to 100000.
inline std::string framesPerFolderProblem(const std::string &value)
{
    const bool digits = !value.empty() && value.size() <= 6 && value.find_first_not_of("0123456789") == std::string::npos;
    const unsigned long n = digits ? std::strtoul(value.c_str(), nullptr, 10) : 1;
    return (digits && (n == 0 || (n >= kMinFramesPerFolder && n <= kMaxFramesPerFolder)))
        ? "" : "is not 0 (off) or a whole number from 100 to 100000";
}

// A good value, or nothing (unset, or bad: the caller keeps its default).
inline std::optional<unsigned> parseFramesPerFolder(const char *text)
{
    if (!text || !*text || !framesPerFolderProblem(text).empty())
        return std::nullopt;
    return static_cast<unsigned>(std::strtoul(text, nullptr, 10));
}

// The part a frame number belongs to (0 when splitting is off).
inline uint64_t partIndex(uint64_t frame, unsigned framesPerFolder)
{
    return framesPerFolder == 0 ? 0 : frame / framesPerFolder;
}

// "part_000" ... "part_999", "part_1000" ...
inline std::string partDirName(uint64_t part)
{
    std::string digits = std::to_string(part);
    if (digits.size() < 3)
        digits.insert(0, 3 - digits.size(), '0');
    return kPartPrefix + digits;
}

// True for the names partDirName makes (the readers use this to find a split clip's frames).
inline bool isPartDirName(const std::string &name)
{
    const std::size_t prefix = std::char_traits<char>::length(kPartPrefix);
    return name.size() >= prefix + 3 && name.compare(0, prefix, kPartPrefix) == 0 &&
           name.find_first_not_of("0123456789", prefix) == std::string::npos;
}

// The frame's path relative to the clip root: the name itself, or "part_<k>/<name>".
inline std::string relativeFramePath(const std::string &frameName, uint64_t frame, unsigned framesPerFolder)
{
    return framesPerFolder == 0 ? frameName : partDirName(partIndex(frame, framesPerFolder)) + "/" + frameName;
}

// The frame's full path: <root>/<name> or <root>/part_<k>/<name> (root without a trailing slash).
inline std::string framePath(const std::string &root, const std::string &frameName, uint64_t frame, unsigned framesPerFolder)
{
    return root + "/" + relativeFramePath(frameName, frame, framesPerFolder);
}

} // namespace apertar::framepath
