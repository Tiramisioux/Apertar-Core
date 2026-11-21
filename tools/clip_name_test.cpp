// Host test for src/util/ClipName.hpp (clip folder names from a template).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/clip_name_test.cpp -o /tmp/cnt && /tmp/cnt
#include "util/ClipName.hpp"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

using namespace apertar::clipname;

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

// The name the camera made before templates: prefix + put_time("%y-%m-%d") + "_C" + 4 digits.
static std::string legacyName(const char *formatPrefix, const std::tm &tm, int clip)
{
    std::ostringstream prefix;
    prefix << formatPrefix << std::put_time(&tm, "%y-%m-%d");
    std::ostringstream name;
    name << prefix.str() << "_C" << std::setw(4) << std::setfill('0') << clip;
    return name.str();
}

static Values at(const char *format, int y, int mo, int d, int h = 1, int mi = 40, int s = 8, int f = 12)
{
    Values v;
    v.format = format; v.year = y; v.month = mo; v.day = d; v.hour = h; v.minute = mi; v.second = s; v.frame = f;
    v.sensor = "IMX585"; v.model = "D1"; v.res = "4K"; v.fps = "25fps"; v.bits = "12bit"; v.comp = "none";
    return v;
}

int main()
{
    // Default == today's name.
    const Parsed def = parse(kDefaultTemplate);
    check(def.ok && !def.corrected && def.normalized == kDefaultTemplate, "default template parses unchanged");
    // The Standard preset adds the take's start timecode.
    const Parsed standard = parse(kStandardTemplate);
    check(standard.ok && !standard.corrected && standard.normalized == kStandardTemplate, "Standard preset parses unchanged");
    same(expand(standard, at("cDNG", 26, 10, 28), 1), "cDNG_26-10-28_C0001_01400812", "Standard: clip then HHMMSSFF");
    struct Combo { const char *prefix; const char *format; int y, mo, d, clip; };
    const Combo combos[] = {
        {"cDNG_", "cDNG", 26, 10, 28, 1}, {"MLV_", "MLV", 25, 1, 2, 17}, {"H264_", "H264", 26, 12, 31, 9999},
        {"ProRes_", "ProRes", 27, 3, 4, 42}, {"cDNG_3to1_", "cDNG_3to1", 26, 10, 28, 2},
        {"cDNG_5to1_", "cDNG_5to1", 0, 1, 1, 100},
    };
    for (const Combo &c : combos) {
        std::tm tm{};
        tm.tm_year = 100 + c.y; tm.tm_mon = c.mo - 1; tm.tm_mday = c.d;
        const std::string old = legacyName(c.prefix, tm, c.clip);
        same(expand(def, at(c.format, c.y, c.mo, c.d), c.clip), old, old.c_str());
    }

    // The owner's examples.
    const Parsed dashed = parse("{format}_{date_}_{tc-}_{clip}");
    same(expand(dashed, at("cDNG", 26, 10, 28), 1), "cDNG_26_10_28_01-40-08-12_C0001", "owner example: dashes");
    const Parsed compact = parse("{format}_{date_}_{tc}_{clip}");
    same(expand(compact, at("cDNG", 26, 10, 28), 1), "cDNG_26_10_28_01400812_C0001", "owner example: compact");

    // Other tokens.
    same(expand(parse("{format}_{yy}{mm}{dd}_{time}_{clip}"), at("MLV", 26, 3, 9, 14, 5, 6), 3), "MLV_260309_140506_C0003", "yy mm dd time");
    same(expand(parse("{time-}_{clip}"), at("MLV", 26, 3, 9, 14, 5, 6), 3), "14-05-06_C0003", "time-");
    same(expand(parse("{cam}_{sensor}_{model}_{res}_{fps}_{bits}_{comp}_{clip}"), at("cDNG", 26, 1, 1), 5),
         "cam0_IMX585_D1_4K_25fps_12bit_none_C0005", "camera tokens");

    // Free text.
    const Parsed text = parse("{text:Scene12}_{format}_{clip}");
    check(text.ok, "free text parses");
    same(expand(text, at("cDNG", 26, 10, 28), 1), "Scene12_cDNG_C0001", "free text expands");
    same(parse("Scene12_{clip}").normalized, "Scene12_{clip}", "literal letters are kept");
    check(!parse("{text:}_{clip}").ok, "empty text is rejected");
    check(!parse("{text:" + std::string(25, 'a') + "}_{clip}").ok, "text over 24 chars is rejected");
    check(parse("{text:" + std::string(24, 'a') + "}_{clip}").ok, "text of 24 chars is accepted");
    check(parse("{text:a b}_{clip}").error.find("' ' (space)") != std::string::npos, "text with a space names the space");

    // Missing {clip}.
    const Parsed noclip = parse("{format}_{date}");
    check(noclip.ok && noclip.corrected && noclip.normalized == "{format}_{date}_{clip}", "missing {clip} is appended and flagged");
    check(!parse("{clip}_{clip}").ok, "two {clip} are rejected");

    // Empty tokens drop their separator; separators collapse; no leading/trailing separator.
    Values off = at("cDNG", 26, 10, 28);
    same(expand(parse("{format}_{log}_{clip}"), off, 1), "cDNG_C0001", "empty {log} drops one separator");
    same(expand(parse("{log}_{format}_{clip}"), off, 1), "cDNG_C0001", "empty first token: no leading separator");
    same(expand(parse("{format}_{clip}_{log}"), off, 1), "cDNG_C0001", "empty last token: no trailing separator");
    same(expand(parse("{format}__--..{clip}"), off, 1), "cDNG_C0001", "separator runs collapse to the first");
    same(expand(parse("_{format}_{clip}_"), off, 1), "cDNG_C0001", "literal leading/trailing separators dropped");
    off.log = "Log";
    same(expand(parse("{format}_{log}_{clip}"), off, 1), "cDNG_Log_C0001", "{log} on");

    // Invalid characters.
    check(parse("{format}/{clip}").error.find("'/'") != std::string::npos, "slash is rejected and named");
    check(parse("{format} {clip}").error.find("(space)") != std::string::npos, "space is rejected and named");
    check(parse("{format}:{clip}").error.find("':'") != std::string::npos, "colon is rejected and named");
    check(!parse("{bogus}_{clip}").ok && parse("{bogus}_{clip}").error.find("{bogus}") != std::string::npos, "unknown token is named");
    check(!parse("{format_{clip}").ok, "unclosed brace is rejected");
    check(!parse("").ok, "empty template is rejected");

    // Length limit.
    check(!parse("{text:aaaaaaaaaaaaaaaaaaaaaaaa}_{text:bbbbbbbbbbbbbbbbbbbbbbbb}_{text:cccccccccccccccccccccccc}_{text:dddddddddddddddddddddddd}_{clip}").ok,
          "a template that can exceed 100 characters is rejected");
    check(parse("{text:aaaaaaaaaaaaaaaaaaaaaaaa}_{text:bbbbbbbbbbbbbbbbbbbbbbbb}_{clip}").ok, "a 55-character template is accepted");

    check(!parse("Timelapse_{clip}").ok, "a name starting with Timelapse is reserved");
    check(parse("{format}_Timelapse_{clip}").ok, "Timelapse inside a name is fine");

    // sanitize
    same(sanitize("IMX585 / AAQJ1"), "IMX585-AAQJ1", "sanitize replaces and collapses");
    same(sanitize("Apertar D1!!"), "Apertar-D1", "sanitize trims");

    // nextClipNumber: the lowest free number, as the camera always did.
    std::set<std::string> taken{"cDNG_26-10-28_C0001", "cDNG_26-10-28_C0002", "cDNG_26-10-28_C0004"};
    auto exists = [&](const std::string &n) { return taken.count(n) > 0; };
    check(nextClipNumber(def, at("cDNG", 26, 10, 28), exists) == 3, "next number fills the first gap (as before)");
    check(nextClipNumber(def, at("cDNG", 26, 10, 29), exists) == 1, "a new date restarts at C0001");
    check(nextClipNumber(dashed, at("cDNG", 26, 10, 28), exists) == 1, "a time-in-name template starts at C0001");

    // A time token: the number counts the day's folders at any time (and the same names without the time).
    std::set<std::string> timedTaken{"cDNG_26-10-28_C0001_01000000", "cDNG_26-10-28_C0002_02-00-00-00x", "cDNG_26-10-28_C0002_09300011"};
    const std::vector<std::string> timedList(timedTaken.begin(), timedTaken.end());
    auto timedExists = [&](const std::string &n) { return timedTaken.count(n) > 0; };
    check(nextClipNumber(standard, at("cDNG", 26, 10, 28), timedExists, timedList) == 3, "Standard: C0001 and C0002 taken at other times");
    check(nextClipNumber(standard, at("cDNG", 26, 10, 29), timedExists, timedList) == 1, "Standard: a new date restarts at C0001");
    const std::vector<std::string> oldNames(taken.begin(), taken.end());
    check(nextClipNumber(standard, at("cDNG", 26, 10, 28), exists, oldNames) == 3, "Standard: original names of the day count (C0001 C0002 C0004)");
    check(nextClipNumber(standard, at("cDNG", 26, 10, 28), [](const std::string &) { return false; }) == 1, "Standard without a listing: C0001");
    const std::vector<std::string> compactNames{"cDNG_26_10_28_01400812_C0001", "cDNG_26_10_28_11223344_C0002"};
    check(nextClipNumber(compact, at("cDNG", 26, 10, 28, 15, 0, 0, 0), [](const std::string &) { return false; }, compactNames) == 3,
          "time before {clip}: other times still count");
    check(!sameClipAnyTime(standard, at("cDNG", 26, 10, 28), 1, "cDNG_26-10-28_C0001_0140081x"), "a letter is not a time");
    check(!sameClipAnyTime(standard, at("cDNG", 26, 10, 28), 1, "cDNG_26-10-28_C00012_01400812"), "C00012 is not C0001");

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
