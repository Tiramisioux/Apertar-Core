// Host test for src/camera/TuningTdnStrip.hpp (no libcamera needed; needs libyaml for the file side).
//   c++ -std=c++20 -I src tools/tuning_tdn_strip_test.cpp $(pkg-config --cflags --libs yaml-0.1) -o /tmp/tuning_tdn_strip_test
//   /tmp/tuning_tdn_strip_test [golden_dir]
// golden_dir (optional) holds imx585_clearHDR.json and imx585_clearHDR_notdn.json: the strip must reproduce the
// second from the first byte for byte.
#include "camera/TuningTdnStrip.hpp"

#include <iostream>

using namespace apertar::tuning;

static int failures = 0;

static void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "[ok]   " : "[FAIL] ") << what << "\n";
    if (!ok)
        ++failures;
}

static void expectStrip(const std::string &what, const std::string &in, const std::string &want, int removed)
{
    const TdnStripResult r = stripTdn(in);
    const bool ok = r.status == TdnStripStatus::Stripped && r.text == want && r.removed == removed;
    check(ok, what);
    if (!ok)
        std::cout << "   got status " << int(r.status) << " reason '" << r.reason << "' text:\n" << r.text << "\n";
}

static std::string slurp(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    std::stringstream out;
    out << input.rdbuf();
    return out.str();
}

int main(int argc, char **argv)
{
    // modes: tdn last, middle, first
    expectStrip("tdn last in a mode",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}, "tdn": {"b": 2}}}}]})",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}}}}]})", 1);
    expectStrip("tdn middle",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}, "tdn": {"b": 2}, "cdn": {"c": 3}}}}]})",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}, "cdn": {"c": 3}}}}]})", 1);
    expectStrip("tdn first",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"tdn": {"b": 2}, "sdn": {"a": 1}}}}]})",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}}}}]})", 1);
    // flat config
    expectStrip("flat config",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"sdn": {"a": 1}, "tdn": {"b": 2}}}]})",
                R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"sdn": {"a": 1}}}]})", 1);
    // three modes, multi-line layout
    expectStrip("three modes, multi-line",
                "{\n  \"target\": \"pisp\",\n  \"algorithms\": [\n    {\n      \"rpi.denoise\": {\n"
                "        \"normal\": {\n          \"sdn\": { \"x\": 1 },\n          \"tdn\": { \"y\": 2 }\n        },\n"
                "        \"hdr\": {\n          \"tdn\": { \"y\": 3 },\n          \"sdn\": { \"x\": 4 }\n        },\n"
                "        \"night\": {\n          \"sdn\": { \"x\": 5 },\n          \"tdn\": { \"y\": 6 }\n        }\n      }\n    }\n  ]\n}\n",
                "{\n  \"target\": \"pisp\",\n  \"algorithms\": [\n    {\n      \"rpi.denoise\": {\n"
                "        \"normal\": {\n          \"sdn\": { \"x\": 1 }\n        },\n"
                "        \"hdr\": {\n          \"sdn\": { \"x\": 4 }\n        },\n"
                "        \"night\": {\n          \"sdn\": { \"x\": 5 }\n        }\n      }\n    }\n  ]\n}\n", 3);
    // braces, commas and escaped quotes in strings must not confuse the scanner
    expectStrip("braces and escaped quotes in strings",
                R"({"target": "pisp", "note": "a } \" { , \"tdn\": {", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"s": "}{\",\\"}, "tdn": {"s": "{\"}"}}}}]})",
                R"({"target": "pisp", "note": "a } \" { , \"tdn\": {", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"s": "}{\",\\"}}}}]})", 1);
    // a tdn outside rpi.denoise is not touched
    {
        const std::string in = R"({"target": "pisp", "algorithms": [{"rpi.other": {"tdn": {"a": 1}, "x": 2}}]})";
        const TdnStripResult r = stripTdn(in);
        check(r.status == TdnStripStatus::NoTdn && r.text == in, "tdn outside rpi.denoise is left alone (no tdn)");
    }
    {
        const std::string in = R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"sdn": {"a": 1}}}}]})";
        const TdnStripResult r = stripTdn(in);
        check(r.status == TdnStripStatus::NoTdn && r.text == in, "no tdn: unchanged");
    }
    {
        const TdnStripResult r = stripTdn(R"({"target": "pisp", "algorithms": [{"rpi.denoise": {"normal": {"tdn": {"a": 1}}}}]})");
        check(r.status == TdnStripStatus::Failed, "sole member is refused: " + r.reason);
    }
    {
        const TdnStripResult r = stripTdn(R"({"target": "pisp", "algorithms": [)");
        check(r.status == TdnStripStatus::Failed, "broken JSON is refused: " + r.reason);
    }

    // golden: the real IMX585 Clear HDR file
    if (argc > 1) {
        const std::string dir = argv[1];
        const std::string source = slurp(dir + "/imx585_clearHDR.json");
        const std::string want = slurp(dir + "/imx585_clearHDR_notdn.json");
        const TdnStripResult r = stripTdn(source);
        check(!source.empty() && !want.empty(), "golden files read");
        check(r.status == TdnStripStatus::Stripped && r.removed == 3, "golden: three tdn blocks removed");
        check(r.text == want, "golden: result is byte-identical to imx585_clearHDR_notdn.json");

        // the file side, with HOME pointing at a scratch directory
        const std::string home = dir + "/home_test";
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ::setenv("HOME", home.c_str(), 1);
        std::string note;
        const std::string path = previewTuningPath(dir + "/imx585_clearHDR.json", Pipeline::PiSP, note);
        check(path == home + "/.cache/apertar-core/tuning/imx585_clearHDR_notdn.json" && note.empty(), "file side: copy path " + path + " " + note);
        check(slurp(path) == want, "file side: copy is the golden result");
        const auto stamp = std::filesystem::last_write_time(path);
        note.clear();
        const std::string again = previewTuningPath(dir + "/imx585_clearHDR.json", Pipeline::PiSP, note);
        check(again == path && std::filesystem::last_write_time(path) == stamp, "file side: second call reuses the copy");
        const std::string plain = previewTuningPath(path, Pipeline::PiSP, note);
        check(plain == path && !note.empty(), "file side: a file without tdn is returned unchanged: " + note);
        std::filesystem::remove_all(home);
    } else {
        std::cout << "(no golden_dir given: golden and file-side tests skipped)\n";
    }

    std::cout << (failures ? "FAILED\n" : "all ok\n");
    return failures ? 1 : 0;
}
