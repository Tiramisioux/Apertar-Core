// Host test for src/camera/ModeLadder.hpp (the ladder from its saved text, an old mode_ladder.conf and the
// get/set_mode_ladder JSON): the aspect filter.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/mode_ladder_test.cpp -o /tmp/mlt && /tmp/mlt
#include "camera/ModeLadder.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace apertar::ladder;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

// A mode_ladder.conf as Core wrote it before the aspect filter: no aspects= line.
static const std::string kRungs = "3840x2160:16:hdr:25:none:log,3840x2160:12:sdr::lossless_jpeg,1920x1080:12:sdr";
static const std::string kOldFile =
    "# Apertar mode ladder (T34). Core writes this file; edit it in the WebUI (Settings > Record > Mode Ladder).\n"
    "enabled=true\n"
    "ramp_ready=false\n"
    "resolutions=" + kRungs + "\n";

int main()
{
    // ---- the ladder from the text of its settings ----
    {
        const Ladder l = fromText(true, true, kRungs, "16:9,2.39:1");
        check(l.aspects == std::vector<std::string>{ "16:9", "2.39:1" }, "fromText: the aspects, in their order");
        check(l.enabled && l.rampReady && rungsText(l.rungs) == kRungs, "fromText: enabled, ramp_ready and the rungs");
        const Ladder all = fromText(true, false, kRungs, "");
        check(all.aspects.empty() && all.enabled && !all.rampReady && rungsText(all.rungs) == kRungs,
              "fromText: an empty filter is All");
        const std::vector<std::string> other = { "1:1", "1.33:1", "1.85:1", "2.55:1", "4:3", "3:2", "1.37:1" };   // IMX585 decimals and the Record pane's IMX283 names
        check(fromText(true, false, kRungs, aspectsText(other)).aspects == other, "round trip: 1:1, 1.33:1, 1.85:1, 2.55:1, 4:3, 3:2, 1.37:1");
        const Ladder none = fromText(true, true, "", "");
        check(none.rungs.empty() && !none.enabled, "fromText: no resolutions text, the ladder is off");
    }

    // ---- a file without the line ----
    {
        const Ladder l = parseFile(kOldFile);
        check(l.aspects.empty(), "old file (no aspects= line): All");
        check(l.enabled && !l.rampReady && rungsText(l.rungs) == kRungs, "old file: enabled and the rungs as before");
        check(parseFile("").aspects.empty() && !parseFile("").enabled, "no file: All, ladder off");
    }

    // ---- a bad aspects line means All and leaves the rest alone, before or after the other lines ----
    {
        std::string many;   // 33 different ratios, one over the limit
        for (int i = 1; i <= 33; ++i)
            many += (i > 1 ? "," : "") + std::to_string(i) + ":1";
        std::string repeated;   // valid labels, but a line over the size limit
        for (int i = 0; i < 120; ++i)
            repeated += (i ? "," : "") + std::string("16:9");
        const std::vector<std::pair<std::string, std::string>> bad = {
            { "16:9,<script>", "a character outside 0-9 . :" },
            { "16:9;2.39:1", "a semicolon" },
            { "16:9,,2.39:1", "an empty label" },
            { ",16:9", "a leading comma" },
            { "16:9, 2.39:1", "a space" },
            { "2.39:1\r", "a CR (a hand edit with CRLF)" },
            { "16:9,12345678901234567", "a label over 16 characters" },
            { "All", "All, the button's name" },
            { many, "33 ratios (at most 32)" },
            { repeated, "a line over 544 characters" },
        };
        for (const auto &[value, why] : bad) {
            const Ladder first = parseFile("aspects=" + value + "\n" + kOldFile);
            const Ladder last = parseFile(kOldFile + "aspects=" + value + "\n");
            const bool ok = first.aspects.empty() && last.aspects.empty() && first.enabled && last.enabled &&
                            !first.rampReady && rungsText(first.rungs) == kRungs && rungsText(last.rungs) == kRungs;
            check(ok, ("bad aspects (" + why + "): All, rungs and enabled intact").c_str());
        }
        std::string most;   // exactly 32 is fine
        for (int i = 1; i <= 32; ++i)
            most += (i > 1 ? "," : "") + std::to_string(i) + ":1";
        check(parseFile(kOldFile + "aspects=" + most + "\n").aspects.size() == 32, "32 ratios are kept");
        check((parseFile(kOldFile + "aspects=16:9,2.39:1,16:9\n").aspects == std::vector<std::string>{ "16:9", "2.39:1" }),
              "a repeated ratio is dropped, the order kept");
        const Ladder brokenRungs = parseFile("enabled=true\nresolutions=3840x2160:99:sdr\naspects=2.39:1\n");
        check(brokenRungs.rungs.empty() && !brokenRungs.enabled && brokenRungs.aspects == std::vector<std::string>{ "2.39:1" },
              "a broken resolutions line (ladder off) keeps the aspects");
    }

    // ---- fromText: a text this Core cannot read leaves the rest alone ----
    {
        const Ladder broken = fromText(true, true, "3840x2160:99:sdr", "2.39:1");
        check(broken.rungs.empty() && !broken.enabled && broken.rampReady && broken.aspects == std::vector<std::string>{ "2.39:1" },
              "fromText: a broken resolutions text (ladder off) keeps ramp_ready and the aspects");
        const Ladder badFilter = fromText(true, false, kRungs, "16:9,<script>");
        check(badFilter.aspects.empty() && badFilter.enabled && rungsText(badFilter.rungs) == kRungs,
              "fromText: a bad filter is All, the ladder stays on");
    }

    // ---- an old mode_ladder.conf, taken over by parseFile and the text functions ----
    {
        Ladder l = parseFile(kOldFile);
        l.aspects = { "16:9" };
        const Ladder back = fromText(l.enabled, l.rampReady, rungsText(l.rungs), aspectsText(l.aspects));
        check(back.enabled && !back.rampReady && rungsText(back.rungs) == kRungs && back.aspects == l.aspects,
              "take-over: the old file's values come back from their text");
        check(rungsText(parseFile("rungs=" + kRungs + "\n").rungs) == kRungs, "parseFile: \"rungs\", the first name of resolutions");
        check(parseFile("enabled=true\nresolutions=1920x1080:12:sdr\nresolutions=3840x2160:99:sdr\n").rungs.empty(),
              "parseFile: the last resolutions line counts");
    }

    // ---- parseAspects, as set_mode_ladder aspects uses it ----
    {
        std::vector<std::string> out = { "stale" };
        std::string error;
        check(parseAspects("", out, error) && out.empty(), "parseAspects: \"\" is All");
        check(parseAspects("16:9,2.39:1", out, error) && out == std::vector<std::string>{ "16:9", "2.39:1" }, "parseAspects: two ratios");
        error.clear();
        check(!parseAspects("16:9,x", out, error) && out.empty() && contains(error, "\"x\""), "parseAspects: a bad label is refused and named");
        check(aspectsText({ "16:9", "2.39:1" }) == "16:9,2.39:1" && aspectsText({}).empty(), "aspectsText: joined by commas, All empty");
    }

    // ---- toJson ----
    {
        Ladder l = parseFile(kOldFile);
        l.aspects = { "16:9", "2.39:1" };
        const std::vector<RungState> states(l.rungs.size());
        const std::string json = toJson(l, states, -1, {}, {}, {});
        check(contains(json, ",\"aspects\":[\"16:9\",\"2.39:1\"],"), "toJson: \"aspects\":[\"16:9\",\"2.39:1\"]");
        check(contains(json, "\"resolutions_text\":\"" + kRungs + "\""), "toJson: resolutions_text as before");
        l.aspects.clear();
        check(contains(toJson(l, states, -1, {}, {}, {}), ",\"aspects\":[],"), "toJson: All is \"aspects\":[]");
    }

    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
