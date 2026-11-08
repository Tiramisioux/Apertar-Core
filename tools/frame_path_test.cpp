// Host test for src/cdng/FramePath.hpp (where a frame file goes when [storage] frames_per_folder splits a clip), and
// for playback::listFrames reading a flat, a split and a partly merged clip folder (empty files in a temp folder).
//   c++ -std=c++17 -O2 -Wall -Wextra -I src tools/frame_path_test.cpp -o /tmp/fpt && /tmp/fpt
#include "cdng/FramePath.hpp"
#include "playback/ClipFrames.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace apertar::framepath;

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

int main()
{
    const std::string root = "/media/RAW/cDNG_26-11-08_C0001";
    const std::string name = "cDNG_26-11-08_C0001_000000499.dng";

    // n = 0: flat, exactly today's path
    same(framePath(root, name, 499, 0), root + "/" + name, "n=0: the frame stays in the clip root");
    same(relativeFramePath(name, 123456, 0), name, "n=0: the relative path is the name");
    check(partIndex(123456, 0) == 0, "n=0: part index 0");

    // n = 500: the boundaries
    check(partIndex(0, 500) == 0, "n=500: frame 0 in part 0");
    check(partIndex(499, 500) == 0, "n=500: frame 499 in part 0");
    check(partIndex(500, 500) == 1, "n=500: frame 500 in part 1");
    check(partIndex(999, 500) == 1, "n=500: frame 999 in part 1");
    check(partIndex(1000, 500) == 2, "n=500: frame 1000 in part 2");
    same(framePath(root, name, 499, 500), root + "/part_000/" + name, "n=500: frame 499 -> part_000");
    same(framePath(root, "x_000000500.dng", 500, 500), root + "/part_001/x_000000500.dng", "n=500: frame 500 -> part_001");
    same(framePath(root, "x_000000999.dng", 999, 500), root + "/part_001/x_000000999.dng", "n=500: frame 999 -> part_001");
    same(framePath(root, "x_000001000.dng", 1000, 500), root + "/part_002/x_000001000.dng", "n=500: frame 1000 -> part_002");

    // padding: three digits, more when needed
    same(partDirName(0), "part_000", "padding: part_000");
    same(partDirName(7), "part_007", "padding: part_007");
    same(partDirName(42), "part_042", "padding: part_042");
    same(partDirName(999), "part_999", "padding: part_999");
    same(partDirName(1000), "part_1000", "padding: part_1000 (wider, still sorts after part_999 by number)");
    same(framePath(root, "x_000099999.dng", 99999, 100), root + "/part_999/x_000099999.dng", "n=100: frame 99999 -> part_999");

    // the name is unchanged inside a part
    const std::string rel = relativeFramePath(name, 499, 500);
    same(rel.substr(rel.find('/') + 1), name, "the frame name inside the part is unchanged");

    // part folder names, as the readers recognise them
    check(isPartDirName("part_000") && isPartDirName("part_1000"), "isPartDirName: part_000, part_1000");
    check(!isPartDirName("part_") && !isPartDirName("part_01") && !isPartDirName("part_00a") && !isPartDirName("Part_000") &&
              !isPartDirName("cDNG_26-11-08_C0001.wav"), "isPartDirName: rejects part_, part_01, part_00a, Part_000, a WAV");

    // the key's values
    check(framesPerFolderProblem("0").empty(), "frames_per_folder=0 is good (off)");
    check(framesPerFolderProblem("100").empty() && framesPerFolderProblem("100000").empty(), "100 and 100000 are good");
    check(!framesPerFolderProblem("99").empty() && !framesPerFolderProblem("100001").empty(), "99 and 100001 are bad");
    check(!framesPerFolderProblem("").empty() && !framesPerFolderProblem("-5").empty() && !framesPerFolderProblem("5e3").empty() &&
              !framesPerFolderProblem("1000000").empty() && !framesPerFolderProblem("500 ").empty(), "empty, -5, 5e3, 1000000, '500 ' are bad");
    check(parseFramesPerFolder("500") == 500u && parseFramesPerFolder("0") == 0u, "parse: 500, 0");
    check(!parseFramesPerFolder("50") && !parseFramesPerFolder(nullptr) && !parseFramesPerFolder(""), "parse: 50, null, empty unset");

    // listFrames: the same 1,234 frames (with a hole at 700) flat, split by 500, and partly merged
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "apertar_frame_path_test";
    const auto makeClip = [&](unsigned n, uint64_t mergedBelow) {
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        std::ofstream(tmp / "C1.wav") << "x";
        for (uint64_t f = 0; f < 1234; ++f) {
            if (f == 700) continue;
            const std::string frameName = "C1_" + std::string(9 - std::to_string(f).size(), '0') + std::to_string(f) + ".dng";
            const unsigned layout = f < mergedBelow ? 0 : n;
            const fs::path p = tmp / relativeFramePath(frameName, f, layout);
            fs::create_directories(p.parent_path());
            std::ofstream(p) << (f == 3 ? "" : "d");   // frame 3 is a 0-byte file
        }
    };
    const auto describe = [](const apertar::playback::FrameList &l) {
        return std::to_string(l.count()) + " files " + std::to_string(l.first) + ".." + std::to_string(l.last) + " missing " +
               std::to_string(l.missing()) + " empty " + std::to_string(l.empty) + " parts " + std::to_string(l.parts) + " " + l.lastPart;
    };
    makeClip(0, 0);
    const auto flat = apertar::playback::listFrames(tmp);
    same(describe(flat), "1233 files 0..1233 missing 1 empty 1 parts 0 ", "listFrames: flat clip");
    same(flat.files[600].name, "C1_000000600.dng", "listFrames: flat names are bare");
    makeClip(500, 0);
    const auto split = apertar::playback::listFrames(tmp);
    same(describe(split), "1233 files 0..1233 missing 1 empty 1 parts 3 part_002", "listFrames: split clip, same frames");
    same(split.files[600].name, "part_001/C1_000000600.dng", "listFrames: a split frame is named by its path in the clip");
    check(split.numbered && split.files[600].number == 600 && split.gaps.size() == 1 && split.gaps[0].first == 700,
          "listFrames: split numbers and the hole as flat");
    makeClip(500, 500);
    const auto partly = apertar::playback::listFrames(tmp);
    same(describe(partly), "1233 files 0..1233 missing 1 empty 1 parts 2 part_002", "listFrames: partly merged clip");
    same(partly.files[499].name + " | " + partly.files[500].name, "C1_000000499.dng | part_001/C1_000000500.dng",
         "listFrames: the merged prefix, then the parts");
    fs::remove_all(tmp);

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
