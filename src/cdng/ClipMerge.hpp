#pragma once

// The merge plan for a split cDNG clip (std only; no filesystem calls). Input is a listing, output is the ordered list
// of renames partDir/name -> name that turns <clip>/part_*/ back into the flat clip (what tools/flatten_clip.py does
// by hand). The camera's merge worker lists the folders, calls plan(), and executes the steps.
//
// Why the order matters: on exFAT every create or rename into a folder walks all entries of the target folder under
// the filesystem lock, so merging 45,000 files is O(N^2) (~0.32 us per existing file per rename). The steps are in
// part order, then frame order, so at every moment the clip is "a valid flat prefix in the root + intact remaining
// parts" (prefixInvariant), and a power cut leaves a state plan() simply continues from.

#include "cdng/FramePath.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace apertar::clipmerge {

struct Listing {
    std::vector<std::string> rootFiles;                                          // regular files in the clip root, names only
    std::vector<std::pair<std::string, std::vector<std::string>>> parts;         // part folder name + its file names
    bool hasMarker = false;                                                      // <clip>/.parts exists
};

struct Step {
    std::string partDir;   // rename partDir/name -> name (in the clip root)
    std::string name;
};

struct Plan {
    std::vector<Step> steps;
    std::vector<std::string> emptyPartsToRemove;   // part dirs that are already empty
    std::vector<std::string> removeAfterSteps;     // part dirs that hold only planned DNGs: rmdir once their last step is done
    bool nothingToDo = false;                      // no part folders and no marker
    bool conflict = false;
    std::string conflictName;                      // the first name that cannot move (steps stop before it)
    bool removeMarker = false;                     // remove .parts last: marker present and no part folder remains
    std::string problem;                           // non-empty: something is left behind, say so
};

namespace detail {

inline bool endsWithNoCase(const std::string &s, const char *suffix)
{
    const std::string x(suffix);
    if (s.size() < x.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        char a = s[s.size() - x.size() + i], b = x[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline bool isDng(const std::string &name) { return endsWithNoCase(name, ".dng"); }

// Same rule as playback/ClipFrames.hpp frameNumberFromName: "<stem>_<digits>.dng" -> digits (right before the last
// '.', preceded by '_', at most 18).
inline bool frameNumber(const std::string &name, uint64_t &number)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    std::size_t i = dot;
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') --i;
    if (i == dot || i == 0 || name[i - 1] != '_' || dot - i > 18) return false;
    number = std::stoull(name.substr(i, dot - i));
    return true;
}

// Numeric order of part folder names (part_999 < part_1000), without overflow for silly widths.
inline bool partLess(const std::string &a, const std::string &b)
{
    auto digits = [](const std::string &s) {
        std::size_t i = std::char_traits<char>::length(framepath::kPartPrefix);
        while (i + 1 < s.size() && s[i] == '0') ++i;
        return s.substr(i);
    };
    const std::string da = digits(a), db = digits(b);
    return da.size() != db.size() ? da.size() < db.size() : da < db;
}

// Frame names in frame order; names without a number after the numbered ones, by name.
inline bool frameLess(const std::string &a, const std::string &b)
{
    uint64_t na = 0, nb = 0;
    const bool ha = frameNumber(a, na), hb = frameNumber(b, nb);
    if (ha != hb) return ha;
    if (ha && na != nb) return na < nb;
    return a < b;
}

// exFAT (and macOS) names are case-insensitive: compare lower-cased.
inline std::string folded(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace detail

inline Plan plan(const Listing &in)
{
    Plan out;
    std::vector<const std::pair<std::string, std::vector<std::string>> *> parts;
    for (const auto &p : in.parts)
        if (framepath::isPartDirName(p.first)) parts.push_back(&p);
    std::sort(parts.begin(), parts.end(), [](auto *a, auto *b) { return detail::partLess(a->first, b->first); });

    out.nothingToDo = parts.empty() && !in.hasMarker;

    std::set<std::string> taken;
    for (const std::string &n : in.rootFiles) taken.insert(detail::folded(n));

    std::size_t remaining = 0;   // part folders that will still exist after the steps
    for (const auto *p : parts) {
        std::vector<std::string> dng, other;
        for (const std::string &n : p->second) (detail::isDng(n) ? dng : other).push_back(n);
        std::sort(dng.begin(), dng.end(), detail::frameLess);

        if (p->second.empty()) {
            out.emptyPartsToRemove.push_back(p->first);
            continue;
        }
        bool stopped = false;
        for (const std::string &n : dng) {
            if (!taken.insert(detail::folded(n)).second) {   // already in the root, or already planned from an earlier part
                out.conflict = true;
                out.conflictName = n;
                stopped = true;
                break;
            }
            out.steps.push_back({p->first, n});
        }
        if (stopped) {
            ++remaining;
            ++remaining;   // this part and every later one stay (the later ones are not planned); marker stays anyway
            break;
        }
        if (!other.empty()) {
            ++remaining;
            if (!out.problem.empty()) out.problem += "; ";
            out.problem += p->first + " keeps " + std::to_string(other.size()) + " non-DNG file(s) (" + other.front() + "), folder not removed";
        } else {
            out.removeAfterSteps.push_back(p->first);
        }
    }
    out.removeMarker = in.hasMarker && !out.conflict && remaining == 0;
    return out;
}

// True when every root frame number is lower than every frame number still in a part (empty sets are fine): the
// "flat prefix + intact parts" state. Executing plan() in order keeps it at every step: each move takes the lowest
// frame left in the lowest part, so the root's highest number stays below all that remain. A false result means
// something else wrote into the clip (or a frame was moved out of order); do not trust a half-merged clip then.
inline bool prefixInvariant(const Listing &in)
{
    bool haveRoot = false;
    uint64_t rootMax = 0;
    for (const std::string &n : in.rootFiles) {
        uint64_t f = 0;
        if (detail::isDng(n) && detail::frameNumber(n, f)) {
            rootMax = haveRoot ? std::max(rootMax, f) : f;
            haveRoot = true;
        }
    }
    if (!haveRoot) return true;
    for (const auto &p : in.parts) {
        if (!framepath::isPartDirName(p.first)) continue;
        for (const std::string &n : p.second) {
            uint64_t f = 0;
            if (detail::isDng(n) && detail::frameNumber(n, f) && f <= rootMax) return false;
        }
    }
    return true;
}

} // namespace apertar::clipmerge
