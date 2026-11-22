// Host test for src/camera/ModeLadder.hpp (the ladder from its saved text, an old mode_ladder.conf and the
// get/set_mode_ladder JSON): the aspect filter.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/mode_ladder_test.cpp -o /tmp/mlt && /tmp/mlt
#include "camera/ModeLadder.hpp"
#include "camera/SustainedFps.hpp"

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
static const std::string kModes = "3840x2160:16:hdr,3840x2160:12:sdr,1920x1080:12:sdr";   // the same list as Dynamic resolution keeps it
static const std::string kOldFile =
    "# Apertar mode ladder (T34). Core writes this file; edit it in the WebUI (Settings > Record > Mode Ladder).\n"
    "enabled=true\n"
    "ramp_ready=false\n"
    "resolutions=" + kRungs + "\n";

int main()
{
    // ---- the ladder from the text of its settings ----
    {
        const Ladder l = fromText(true, kModes, "16:9,2.39:1");
        check(l.aspects == std::vector<std::string>{ "16:9", "2.39:1" }, "fromText: the aspects, in their order");
        check(l.enabled && modesText(l.rungs) == kModes, "fromText: enabled and the modes");
        const Ladder all = fromText(true, kModes, "");
        check(all.aspects.empty() && all.enabled && modesText(all.rungs) == kModes,
              "fromText: an empty filter is All");
        const std::vector<std::string> other = { "1:1", "1.33:1", "1.85:1", "2.55:1", "4:3", "3:2", "1.37:1" };   // IMX585 decimals and the Record pane's IMX283 names
        check(fromText(true, kModes, aspectsText(other)).aspects == other, "round trip: 1:1, 1.33:1, 1.85:1, 2.55:1, 4:3, 3:2, 1.37:1");
        const Ladder none = fromText(true, "", "");
        check(none.rungs.empty() && none.enabled, "fromText: no modes text is the default list, still on");
    }

    // ---- a file without the line ----
    {
        const Ladder l = parseFile(kOldFile);
        check(l.aspects.empty(), "old file (no aspects= line): All");
        check(l.enabled && modesText(l.rungs) == kModes, "old file: enabled and the rungs as before");
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
                            modesText(first.rungs) == kModes && modesText(last.rungs) == kModes;
            check(ok, ("bad aspects (" + why + "): All, rungs and enabled intact").c_str());
        }
        std::string most;   // exactly 32 is fine
        for (int i = 1; i <= 32; ++i)
            most += (i > 1 ? "," : "") + std::to_string(i) + ":1";
        check(parseFile(kOldFile + "aspects=" + most + "\n").aspects.size() == 32, "32 ratios are kept");
        check((parseFile(kOldFile + "aspects=16:9,2.39:1,16:9\n").aspects == std::vector<std::string>{ "16:9", "2.39:1" }),
              "a repeated ratio is dropped, the order kept");
        const Ladder brokenRungs = parseFile("enabled=true\nresolutions=3840x2160:99:sdr\naspects=2.39:1\n");
        check(brokenRungs.rungs.empty() && brokenRungs.enabled && brokenRungs.aspects == std::vector<std::string>{ "2.39:1" },
              "a broken resolutions line (the default list) keeps the aspects");
    }

    // ---- fromText: a text this Core cannot read leaves the rest alone ----
    {
        const Ladder broken = fromText(true, "3840x2160:99:sdr", "2.39:1");
        check(broken.rungs.empty() && broken.enabled && broken.aspects == std::vector<std::string>{ "2.39:1" },
              "fromText: a broken modes text is the default list and keeps the aspects");
        const Ladder badFilter = fromText(true, kModes, "16:9,<script>");
        check(badFilter.aspects.empty() && badFilter.enabled && modesText(badFilter.rungs) == kModes,
              "fromText: a bad filter is All, the ladder stays on");
    }

    // ---- an old mode_ladder.conf, taken over by parseFile and the text functions ----
    {
        Ladder l = parseFile(kOldFile);
        l.aspects = { "16:9" };
        const Ladder back = fromText(l.enabled, modesText(l.rungs), aspectsText(l.aspects));
        check(back.enabled && modesText(back.rungs) == kModes && back.aspects == l.aspects,
              "take-over: the old file's values come back from their text");
        check(modesText(parseFile("rungs=" + kRungs + "\n").rungs) == kModes, "parseFile: \"rungs\", the first name of resolutions");
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

    // ---- parseModes: the old fields are read and dropped ----
    {
        std::vector<Rung> out;
        std::string error;
        check(parseModes(kRungs, out, error) && modesText(out) == kModes && out[0].compression.empty(),
              "parseModes: threshold, compression and Log of an old ladder text are dropped");
        check(parseModes("3840x2160:16:hdr:25:none:log10,1920x1080:10:sdr::lossy_3to1", out, error) && out.size() == 2,
              "parseModes: the old spellings of the extra fields still read");
        error.clear();
        check(!parseModes("3840x2160:99:sdr", out, error) && contains(error, "size, bits"), "parseModes: bits 99 is refused and named");
        check(!parseModes("3840x2160:16", out, error) && contains(error, "expected WIDTHxHEIGHT:BITS:hdr|sdr"), "parseModes: two fields are refused");
        check(!parseModes("", out, error) && error == "Add at least one resolution.", "parseModes: an empty text is refused");
        check(parseModes("3840x2160:16:hdr:1:2:3:4", out, error) == false, "parseModes: seven fields are refused");
    }

    // ---- toJson ----
    {
        Ladder l = parseFile(kOldFile);
        l.aspects = { "16:9", "2.39:1" };
        const std::vector<RungState> states(l.rungs.size());
        const std::string json = toJson(l, states, -1, {}, {}, {});
        check(contains(json, ",\"aspects\":[\"16:9\",\"2.39:1\"],"), "toJson: \"aspects\":[\"16:9\",\"2.39:1\"]");
        check(contains(json, "\"resolutions_text\":\"" + kModes + "\""), "toJson: resolutions_text as before");
        l.aspects.clear();
        check(contains(toJson(l, states, -1, {}, {}, {}), ",\"aspects\":[],"), "toJson: All is \"aspects\":[]");
    }

    // ---- Dynamic resolution: the order and the choice (table C), default list, no crops, TDN off, Log off ----
    {
        using namespace apertar::sustained;
        struct M { Rung mode; double ceiling; };
        const auto rung = [](unsigned w, unsigned h, unsigned bits, bool hdr) { Rung r; r.width = w; r.height = h; r.bits = bits; r.clearHdr = hdr; return r; };
        const M list[] = { { rung(3840, 2160, 16, true), 30 }, { rung(1920, 1080, 16, true), 30 }, { rung(3840, 2160, 12, true), 30 },
                           { rung(3840, 2160, 12, false), 60 }, { rung(1920, 1080, 12, false), 60 }, { rung(3840, 2160, 10, false), 60 },
                           { rung(1920, 1080, 10, false), 60 } };
        for (const Kind kind : { Kind::Nvme, Kind::UsbSsd }) {
            Context context;
            context.storage = kind;
            std::vector<Candidate> candidates;
            for (const M &m : list) {
                Candidate c;
                c.mode = m.mode;
                const Mode mode{ m.mode.width, m.mode.height, m.mode.clearHdr, m.mode.bits, m.mode.width <= 1920 };
                c.usable = true;
                c.cap = capFor(context, mode, m.ceiling).fps;
                candidates.push_back(c);
            }
            const auto pick = [&](double fps) { const int i = chooseMode(candidates, fps); return i < 0 ? std::string("none") : label(candidates[static_cast<size_t>(i)].mode); };
            if (kind == Kind::Nvme) {
                check(pick(20) == "4K Clear HDR 16-bit" && pick(21) == "1920x1080 Clear HDR 16-bit" && pick(30) == "1920x1080 Clear HDR 16-bit",
                      "table C NVMe: up to 20 fps 4K Clear HDR 16-bit, 21 to 30 the 2K one");
                check(pick(33) == "4K SDR 12-bit" && pick(34) == "1920x1080 SDR 12-bit" && pick(60) == "1920x1080 SDR 12-bit",
                      "table C NVMe: 33 fps 4K SDR 12-bit, 34 to 60 2K SDR 12-bit");
                check(pick(80) == "1920x1080 SDR 12-bit", "table C NVMe: above 60 nothing holds it, the highest cap is used");
            } else {
                check(pick(9) == "4K Clear HDR 16-bit" && pick(25) == "1920x1080 Clear HDR 16-bit" && pick(26) == "1920x1080 SDR 12-bit" &&
                          pick(33) == "1920x1080 SDR 12-bit" && pick(60) == "1920x1080 SDR 12-bit",
                      "table C USB SSD: 9 fps 4K Clear HDR 16-bit, 25 the 2K one, 26 to 33 2K SDR 12-bit, above that clamped");
            }
        }
        std::vector<Candidate> order(3);
        order[0].mode = rung(3840, 1744, 12, false); order[0].crop = true;
        order[1].mode = rung(1920, 1080, 12, false);
        order[2].mode = rung(3840, 2160, 12, false);
        const std::vector<size_t> by = tryOrder(order);
        check(by[0] == 2 && by[1] == 1 && by[2] == 0, "tryOrder: a crop-in after the full-frame modes of its class");
        order[1].usable = order[2].usable = false;
        order[0].usable = true;
        order[0].cap = 33;
        check(chooseMode(order, 50) == 0 && chooseMode(order, 25) == 0, "chooseMode: a lone usable mode is the answer, clamped or not");
        order[0].usable = false;
        check(chooseMode(order, 25) == -1, "chooseMode: no usable mode is -1");
        const Ladder owner = parseFile("enabled=true\nresolutions=3840x2160:16:hdr:25:lossless_jpeg,3840x2160:12:sdr:30:lossless_jpeg\naspects=\n");
        check(owner.enabled && modesText(owner.rungs) == "3840x2160:16:hdr,3840x2160:12:sdr" && owner.rungs[0].compression.empty(),
              "take-over: the owner's ladder becomes the enabled list, thresholds and compression dropped");
    }

    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
