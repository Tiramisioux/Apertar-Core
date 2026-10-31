// Host test for src/cdng/LogCurve.hpp and the cdngLogBits rule in Types.hpp.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/log_curve_test.cpp -o /tmp/lct && /tmp/lct
#include "cdng/ClearHdrCcmp.hpp"
#include "cdng/LogCurve.hpp"
#include <apertar/core/Types.hpp>
#include "camera/ModeLadder.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace apertar::cdnglog;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

struct Config {
    int src, tgt, black, white;
};
// Each pair twice: the reference black level and a second one, to show black is not hard-wired.
static const Config kConfigs[] = {
    {16, 12, 3200, 65535}, {16, 12, 2800, 65520},
    {12, 10, 200, 4095},   {12, 10, 256, 4000},
    {10, 8, 50, 1023},     {10, 8, 64, 1000},
};

struct Golden {
    uint32_t level, code, back;
};
struct GoldenSet {
    int src, tgt, black, white;
    std::vector<Golden> rows;
};
// GOLDEN: regenerate with `python3 tools/gen_log_curve.py` and paste the output here.
static const GoldenSet kGolden[] = {
    {16, 12, 3200, 65535, {
        {0, 0, 2721},
        {2957, 16, 2964},
        {3199, 31, 3192},
        {3200, 32, 3200},
        {3201, 98, 3201},
        {3202, 155, 3202},
        {3210, 454, 3210},
        {3823, 2068, 3824},
        {9433, 3080, 9438},
        {34367, 3789, 34348},
        {65534, 4095, 65535},
        {65535, 4095, 65535},
    }},
    {12, 10, 200, 4095, {
        {0, 0, 3},
        {100, 16, 103},
        {199, 31, 197},
        {200, 32, 200},
        {201, 76, 201},
        {202, 109, 202},
        {210, 246, 210},
        {238, 405, 238},
        {589, 712, 590},
        {2147, 929, 2145},
        {4094, 1023, 4095},
        {4095, 1023, 4095},
    }},
    {10, 8, 50, 1023, {
        {0, 0, 29},
        {39, 4, 40},
        {49, 7, 49},
        {50, 8, 50},
        {51, 15, 51},
        {52, 22, 52},
        {59, 52, 59},
        {60, 55, 60},
        {147, 146, 148},
        {536, 222, 540},
        {1022, 255, 1023},
        {1023, 255, 1023},
    }}
};

int main()
{
    std::printf("1. validity and constants\n");
    {
        check(pairConstants(16, 12) && pairConstants(12, 10) && pairConstants(10, 8), "three pairs known");
        check(!pairConstants(16, 10) && !pairConstants(12, 12) && !pairConstants(8, 8), "other pairs unknown");
        check(!makeParams(12, 12, 200, 4095), "no curve when target >= source");
        check(!makeParams(12, 10, 200, 5000), "white above source range rejected");
        check(!makeParams(12, 10, 4095, 4095), "white <= black rejected");
        check(!makeParams(10, 8, 10, 1023), "footroom deeper than black rejected");
        Params bad;
        check(!bad.valid() && buildTables(bad).forward.empty(), "default params invalid, empty tables");
        const auto p = makeParams(12, 10, 200, 4095);
        check(p && p->valid() && !p->describe().empty(), "describe");
    }

    std::printf("2. properties per pair\n");
    for (const Config &c : kConfigs) {
        char tag[96];
        const auto p = makeParams(c.src, c.tgt, c.black, c.white);
        std::snprintf(tag, sizeof tag, "%d->%d black=%d white=%d", c.src, c.tgt, c.black, c.white);
        check(p.has_value(), tag);
        if (!p)
            continue;
        const Tables t = buildTables(*p);
        check(t.forward.size() == (size_t{1} << c.src), "forward size");
        check(t.inverse.size() == (size_t{1} << c.tgt), "inverse size");
        check(std::is_sorted(t.forward.begin(), t.forward.end()), "forward monotonic");
        check(std::is_sorted(t.inverse.begin(), t.inverse.end()), "inverse monotonic");
        check(t.inverse.back() == c.white, "inverse.back() == white");
        check(t.inverse[static_cast<size_t>(p->footroomCodes)] == c.black, "first mu-law code decodes to black");
        check(t.inverse[t.forward[static_cast<size_t>(c.black)]] == c.black, "black point round-trips");

        // Round trip above black. One code covers many levels near the top, so the bound is the local bin:
        // |error| <= half the span between the neighbouring codes' levels, plus 1 for rounding. Where codes
        // are denser than levels this reduces to the 1 LSB the task asked for.
        int worst = 0, worstLevel = 0;
        double worstRatio = 0;  // error / (local half bin), for the report
        bool within = true, oneLsbBelowTop = true;
        const size_t cmax = t.inverse.size() - 1;
        for (int level = c.black; level <= c.white; ++level) {
            const uint16_t code = t.forward[static_cast<size_t>(level)];
            const int back = t.inverse[code];
            const int err = std::abs(back - level);
            const size_t lo = code == 0 ? 0 : code - 1u, hi = std::min<size_t>(code + 1u, cmax);
            const double halfBin = (t.inverse[hi] - t.inverse[lo]) / (2.0 * static_cast<double>(hi - lo));
            if (err > worst) {
                worst = err;
                worstLevel = level;
            }
            if (halfBin > 0)
                worstRatio = std::max(worstRatio, err / halfBin);
            if (err > halfBin + 1.0)
                within = false;
            if (halfBin <= 1.0 && err > 1)
                oneLsbBelowTop = false;
        }
        check(within, "round trip within local half bin + 1 for every level above black");
        check(oneLsbBelowTop, "round trip within 1 LSB wherever the bin is 2 LSB or less");
        std::printf("   %s: worst round-trip error %d LSB at level %d (%.2f of the local half bin)\n", tag,
                    worst, worstLevel, worstRatio);
    }

    std::printf("3. golden values from tools/gen_log_curve.py\n");
    for (const GoldenSet &g : kGolden) {
        const auto p = makeParams(g.src, g.tgt, g.black, g.white);
        check(p.has_value(), "golden params");
        if (!p)
            continue;
        const Tables t = buildTables(*p);
        for (const Golden &row : g.rows) {
            check(encodeCode(*p, row.level) == row.code, "golden encode");
            check(t.forward[row.level] == row.code, "golden forward table");
            check(decodeLevel(*p, row.code) == row.back, "golden decode");
            check(t.inverse[row.code] == row.back, "golden inverse table");
        }
    }

    std::printf("4. composed table for 12-bit Clear HDR\n");
    {
        const auto &expansion = apertar::ccmp::expansionTable();
        for (const Config &c : {Config{16, 12, 3200, 65520}, Config{16, 12, 3200, 65535}}) {
            const auto p = makeParams(c.src, c.tgt, c.black, c.white);
            check(p.has_value(), "composed params");
            if (!p)
                continue;
            const Tables plain = buildTables(*p);
            const Tables composed = buildComposed(*p, expansion);
            check(composed.forward.size() == 4096, "composed forward has 4096 entries");
            check(composed.inverse == plain.inverse, "composed inverse equals the plain 16-bit inverse");
            check(std::is_sorted(composed.forward.begin(), composed.forward.end()), "composed forward monotonic");
            bool same = true;
            for (size_t code = 0; code < 4096; ++code) {
                const uint16_t viaPlain = plain.forward[expansion[code]];
                same = same && composed.forward[code] == viaPlain &&
                       composed.inverse[composed.forward[code]] == plain.inverse[viaPlain];
            }
            check(same, "composed[c] matches plain forward applied to expansion[c]");
        }
        check(buildComposed(*makeParams(12, 10, 200, 4095), expansion).forward.empty(), "composed needs a 16-bit curve");
        check(buildComposed(*makeParams(16, 12, 3200, 65535), std::span<const uint16_t>(expansion.data(), 100)).forward.empty(),
              "composed needs 4096 entries");
    }

    std::printf("5. applying the tables to samples\n");
    {
        const auto p = makeParams(12, 10, 200, 4095);
        const Tables t = buildTables(*p);
        std::vector<uint16_t> ramp(4096);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = static_cast<uint16_t>(i);
        std::vector<uint16_t> samples = ramp;
        applyTable(samples.data(), samples.size(), t.forward.data());
        check(samples == t.forward, "forward applied to a ramp gives forward[L]");
        bool back = true;
        for (size_t i = 0; i < samples.size(); ++i)
            back = back && t.inverse[samples[i]] == decodeLevel(*p, encodeCode(*p, static_cast<uint32_t>(i)));
        check(back, "inverse of an applied sample is the decoded level");
        applyTable(samples.data(), 0, t.forward.data());
        check(samples == t.forward, "count 0 changes nothing");

        const std::vector<uint16_t> wide = containerTable(t.forward, 0);
        check(wide.size() == 65536, "container table has 65536 entries");
        bool same = true, clamped = true;
        for (size_t i = 0; i < 65536; ++i) {
            same = same && (i >= 4096 || wide[i] == t.forward[i]);
            clamped = clamped && (i < 4096 || wide[i] == t.forward.back());
        }
        check(same, "container table equals forward inside the source range");
        check(clamped, "values above the source range clamp to the top code");
        const auto p8 = makeParams(10, 8, 50, 1023);
        const Tables t8 = buildTables(*p8);
        const std::vector<uint16_t> high = containerTable(t8.forward, 8);
        check(high[500] == (t8.forward[500] << 8) && (high[500] & 0xff) == 0, "8-bit codes sit in the high byte");
        check(containerTable({}, 0).empty() && containerTable(t.forward, 9).empty(), "bad container input gives nothing");

        // Clear HDR 12-bit: linear 16-bit sample -> CCMP code -> composed Log code in one pass.
        const auto p16 = makeParams(16, 12, 3200, 65520);
        const Tables plain = buildTables(*p16);
        const Tables composed = buildComposed(*p16, apertar::ccmp::expansionTable());
        const std::vector<uint16_t> viaCcmp = containerTable(composed.forward, 0, apertar::ccmp::compressionLut());
        check(viaCcmp.size() == 65536, "ccmp container table has 65536 entries");
        bool exact = true;
        for (uint16_t code = 0; code < 4096; ++code) {
            std::vector<uint16_t> one{apertar::ccmp::expansionTable()[code]};  // what libcamera delivers
            applyTable(one.data(), 1, viaCcmp.data());
            exact = exact && one[0] == composed.forward[code] && one[0] == plain.forward[apertar::ccmp::expansionTable()[code]];
        }
        check(exact, "libcamera's linear sample for each CCMP code lands on composed.forward[code]");
        check(containerTable(composed.forward, 0, std::span<const uint16_t>(apertar::ccmp::expansionTable().data(), 4096)).empty(),
              "via-index table must have 65536 entries");
    }

    std::printf("6. cdngLogBits rule\n");
    {
        using namespace apertar;
        const auto bits = [](RecordingFormat format, bool hdr, unsigned depth, CdngLog log) {
            CaptureSettings s;
            s.recordingFormat = format;
            s.clearHdr = hdr;
            s.rawBitDepth = depth;
            s.cdngLog = log;
            return cdngLogBits(s);
        };
        const RecordingFormat C = RecordingFormat::Cdng, L = RecordingFormat::LossyDng;
        check(bits(C, true, 12, CdngLog::Auto) == 12, "CCMP: on stores 12");
        check(bits(C, true, 12, CdngLog::Off) == 0, "CCMP: off is 0");
        check(bits(C, true, 16, CdngLog::Auto) == 12, "Clear HDR 16-bit: normal 16->12");
        check(bits(C, false, 16, CdngLog::Auto) == 12 && bits(C, false, 12, CdngLog::Auto) == 10, "16->12, 12->10");
        check(bits(C, false, 10, CdngLog::Auto) == 8 && bits(C, false, 8, CdngLog::Auto) == 0, "10->8, 8-bit stays linear");
        check(bits(C, false, 16, CdngLog::Off) == 0 && bits(C, false, 10, CdngLog::Off) == 0, "off is 0 at any depth");
        check(bits(L, true, 12, CdngLog::Auto) == 10, "Lossy DNG has no CCMP codes: normal 12->10");
        check(bits(RecordingFormat::Mlv, true, 12, CdngLog::Auto) == 0, "MLV unavailable");
    }

    std::printf("7. one Off/On switch: strings, stored bits, ladder text\n");
    {
        using namespace apertar;
        const auto parse = [](const char *text, CdngLog start) {
            CdngLog out = start;
            const bool ok = cdngLogFromString(text, out);
            return std::pair<bool, CdngLog>(ok, out);
        };
        check(parse("off", CdngLog::Auto) == std::pair<bool, CdngLog>(true, CdngLog::Off), "off -> Off");
        for (const char *legacy : {"auto", "on", "12", "10", "8"})
            check(parse(legacy, CdngLog::Off) == std::pair<bool, CdngLog>(true, CdngLog::Auto), legacy);
        check(parse("banana", CdngLog::Auto) == std::pair<bool, CdngLog>(false, CdngLog::Auto), "banana is rejected, out untouched");
        check(parse("banana", CdngLog::Off) == std::pair<bool, CdngLog>(false, CdngLog::Off), "banana leaves Off alone too");
        check(parse("", CdngLog::Off).first == false && parse("log", CdngLog::Off).first == false, "empty and 'log' are not values");
        check(std::string(toString(CdngLog::Off)) == "off" && std::string(toString(CdngLog::Auto)) == "auto", "toString is off/auto");
        check(cdngLogStoredBits(CdngLog::Auto, 16) == 12 && cdngLogStoredBits(CdngLog::Auto, 12) == 10 &&
              cdngLogStoredBits(CdngLog::Auto, 10) == 8 && cdngLogStoredBits(CdngLog::Auto, 8) == 0 &&
              cdngLogStoredBits(CdngLog::Auto, 14) == 0 && cdngLogStoredBits(CdngLog::Off, 16) == 0,
              "stored bits: auto 16/12/10/8 -> 12/10/8/0, off 0");

        std::vector<apertar::ladder::Rung> rungs;
        std::string error;
        const bool ok = apertar::ladder::parseRungs(
            "3840x2160:16:hdr:25:none:log,3840x2160:12:sdr::none:log10,1920x1080:10:sdr::none:log8,1920x1080:12:sdr", rungs, error);
        check(ok && rungs.size() == 4, "ladder parses log, log10, log8 and no field");
        check(ok && rungs[0].cdngLog == CdngLog::Auto && rungs[1].cdngLog == CdngLog::Auto &&
              rungs[2].cdngLog == CdngLog::Auto && rungs[3].cdngLog == CdngLog::Off, "log/log10/log8 are Auto, absent is Off");
        const std::string text = ok ? apertar::ladder::rungsText(rungs) : "";
        check(text.find("log10") == std::string::npos && text.find("log8") == std::string::npos &&
              text.find(":log") != std::string::npos, "rungsText prints only log");
        std::vector<apertar::ladder::Rung> again;
        check(apertar::ladder::parseRungs(text, again, error) && apertar::ladder::rungsText(again) == text, "ladder text round-trips");
        check(ok && apertar::ladder::logBits(rungs[0]) == 12 && apertar::ladder::logBits(rungs[1]) == 10 &&
              apertar::ladder::logBits(rungs[2]) == 8 && apertar::ladder::logBits(rungs[3]) == 0, "rung log_bits resolve one step down");
        std::vector<apertar::ladder::Rung> bad;
        check(!apertar::ladder::parseRungs("3840x2160:16:hdr:25:none:logx", bad, error) &&
              error.find("the last field must be log") != std::string::npos, "ladder rejects other words");
    }

    std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
