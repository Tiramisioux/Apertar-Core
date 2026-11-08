// Host test for src/cdng/ClipMerge.hpp (the plan that merges a split clip's part_*/ folders back into the clip root).
//   c++ -std=c++17 -O2 -Wall -Wextra -I src tools/clip_merge_test.cpp -o /tmp/cmt && /tmp/cmt
#include "cdng/ClipMerge.hpp"

#include <cstdio>
#include <string>

using namespace apertar::clipmerge;
using apertar::framepath::partDirName;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}
static void same(const std::string &got, const std::string &want, const char *what)
{
    const bool ok = got == want;
    std::printf("[%s] %s%s\n", ok ? "ok" : "FAIL", what, ok ? "" : (" got '" + got + "' want '" + want + "'").c_str());
    if (!ok)
        ++g_failed;
}

static std::string frameName(unsigned long f)
{
    char b[64];
    std::snprintf(b, sizeof b, "c1_%09lu.dng", f);
    return b;
}

// Frames [from, to) split by n into the listing's parts (part folders as the camera makes them).
static void addFrames(Listing &l, unsigned long from, unsigned long to, unsigned n)
{
    for (unsigned long f = from; f < to; ++f) {
        const std::string dir = partDirName(f / n);
        if (l.parts.empty() || l.parts.back().first != dir)
            l.parts.push_back({dir, {}});
        l.parts.back().second.push_back(frameName(f));
    }
}

int main()
{
    // flat clip, no marker
    {
        Listing l;
        l.rootFiles = {frameName(0), frameName(1), "c1.wav"};
        const Plan p = plan(l);
        check(p.nothingToDo && p.steps.empty() && !p.removeMarker && !p.conflict, "flat clip without marker: nothingToDo");
    }
    // flat clip with a marker, no parts
    {
        Listing l;
        l.rootFiles = {frameName(0), ".parts"};
        l.hasMarker = true;
        const Plan p = plan(l);
        check(!p.nothingToDo && p.steps.empty() && p.removeMarker, "flat clip with marker: no steps, removeMarker");
    }
    // 3 parts, N=500, 1,234 frames; listing shuffled inside a part and parts out of order
    {
        Listing l;
        l.rootFiles = {"c1.wav", ".parts"};
        l.hasMarker = true;
        addFrames(l, 0, 1234, 500);
        std::swap(l.parts[0], l.parts[2]);
        std::reverse(l.parts[1].second.begin(), l.parts[1].second.end());
        const Plan p = plan(l);
        check(p.steps.size() == 1234, "3 parts: 1,234 steps");
        bool order = p.steps.size() == 1234;
        for (unsigned long i = 0; order && i < p.steps.size(); ++i)
            order = p.steps[i].name == frameName(i) && p.steps[i].partDir == partDirName(i / 500);
        check(order, "3 parts: steps in frame order, each from its own part");
        check(p.removeMarker && !p.conflict && p.problem.empty(), "3 parts: removeMarker, no conflict, no problem");
        check(p.removeAfterSteps.size() == 3 && p.removeAfterSteps[0] == "part_000" && p.removeAfterSteps[2] == "part_002", "3 parts: each part removable after its steps");
        check(prefixInvariant(l), "3 parts: prefix invariant holds (empty root)");
    }
    // part_1000 sorts after part_999
    {
        Listing l;
        l.parts.push_back({"part_1000", {frameName(100000)}});
        l.parts.push_back({"part_999", {frameName(99999)}});
        l.parts.push_back({"part_000", {frameName(0)}});
        const Plan p = plan(l);
        check(p.steps.size() == 3 && p.steps[0].partDir == "part_000" && p.steps[1].partDir == "part_999" && p.steps[2].partDir == "part_1000",
              "part_1000 after part_999 (numeric)");
    }
    // conflict
    {
        Listing l;
        l.rootFiles = {frameName(700)};
        addFrames(l, 0, 1000, 500);
        const Plan p = plan(l);
        check(p.conflict, "conflict: detected");
        same(p.conflictName, frameName(700), "conflict: the right name");
        check(p.steps.size() == 700, "conflict: steps only before it (frames 0..699)");
        check(!p.removeMarker, "conflict: no removeMarker");
    }
    // duplicate across parts
    {
        Listing l;
        l.hasMarker = true;
        l.parts.push_back({"part_000", {frameName(5), frameName(6)}});
        l.parts.push_back({"part_001", {frameName(6), frameName(7)}});
        const Plan p = plan(l);
        check(p.conflict && p.conflictName == frameName(6) && p.steps.size() == 2, "duplicate across parts: conflict, 2 steps before it");
    }
    // non-DNG file in a part
    {
        Listing l;
        l.hasMarker = true;
        l.parts.push_back({"part_000", {frameName(0), frameName(1), "notes.txt"}});
        l.parts.push_back({"part_001", {frameName(500)}});
        const Plan p = plan(l);
        check(p.steps.size() == 3, "non-DNG: the DNGs still move");
        check(!p.problem.empty() && p.problem.find("notes.txt") != std::string::npos, "non-DNG: problem names the file");
        check(p.removeAfterSteps.size() == 1 && p.removeAfterSteps[0] == "part_001", "non-DNG: that part is not removable");
        check(!p.removeMarker, "non-DNG: removeMarker false");
    }
    // empty part folder
    {
        Listing l;
        l.hasMarker = true;
        l.parts.push_back({"part_000", {}});
        const Plan p = plan(l);
        check(p.steps.empty() && p.emptyPartsToRemove.size() == 1 && p.removeMarker, "empty part: listed for removal, marker removable");
    }
    // partly merged: part_000 already in the root
    {
        Listing l;
        l.hasMarker = true;
        l.rootFiles = {"c1.wav", ".parts"};
        for (unsigned long f = 0; f < 500; ++f)
            l.rootFiles.push_back(frameName(f));
        addFrames(l, 500, 1234, 500);
        check(prefixInvariant(l), "partly merged: prefix invariant holds");
        const Plan p = plan(l);
        check(p.steps.size() == 734 && p.steps[0].partDir == "part_001" && p.steps[0].name == frameName(500), "partly merged: plan continues with part_001");
        check(!p.conflict && p.removeMarker, "partly merged: no conflict, removeMarker");
    }
    // broken state
    {
        Listing l;
        l.rootFiles = {frameName(0), frameName(600)};
        addFrames(l, 500, 600, 500);
        check(!prefixInvariant(l), "broken: root frame 600 above frames still in a part -> invariant false");
        Listing e;
        check(prefixInvariant(e), "empty listing: invariant holds");
    }

    if (g_failed) {
        std::printf("%d FAILED\n", g_failed);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
