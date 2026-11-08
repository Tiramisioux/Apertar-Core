// Host test for the [storage] write-buffer keys: the reader (bufferbudget::parseBufferConf) and the Config pane's validator.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src tools/storage_buffer_conf_test.cpp -o /tmp/sbct && /tmp/sbct
#include "hardware/HardwareConfigText.hpp"
#include "util/BufferBudget.hpp"

#include <cstdio>

using namespace apertar;
static int g_failed = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}
static std::vector<hwconf::Issue> bufferIssues(const std::string &text)
{
    std::vector<hwconf::Issue> out;
    for (const auto &issue : hwconf::validate(text, hwconf::Facts{}))
        if (issue.section == "storage") out.push_back(issue);
    return out;
}
static const std::string head = "[storage]\nmount_path=/media/RAW\npcie_controller=1000110000.pcie\n";

int main()
{
    {
        const auto conf = bufferbudget::parseBufferConf(head);
        check(!conf.ram && !conf.fraction && !conf.floorMb && !conf.maxFraction, "keys absent: nothing set (frames, defaults)");
        check(bufferIssues(head).empty(), "keys absent: no issue; mount_path and pcie_controller still parse");
        const auto doc = hwconf::parse(head);
        check(hwconf::valueOf(doc, "storage", "mount_path") == "/media/RAW", "mount_path still /media/RAW");
        check(hwconf::valueOf(doc, "storage", "pcie_controller") == "1000110000.pcie", "pcie_controller still read");
    }
    {
        const std::string text = head + "buffer=ram\nbuffer_ram_fraction=0.7\nbuffer_ram_floor_mb=256\nbuffer_ram_max_fraction=0.4\n";
        const auto conf = bufferbudget::parseBufferConf(text);
        check(conf.ram && *conf.ram, "buffer=ram");
        check(conf.fraction && *conf.fraction == 0.7, "fraction 0.7");
        check(conf.floorMb && *conf.floorMb == 256, "floor 256");
        check(conf.maxFraction && *conf.maxFraction == 0.4, "max fraction 0.4");
        check(bufferIssues(text).empty(), "valid ram block: no issue");
    }
    {
        const auto conf = bufferbudget::parseBufferConf(head + "buffer=frames\n");
        check(conf.ram && !*conf.ram, "buffer=frames");
    }
    {
        const std::string text = head + "buffer=ramm\n";
        const auto conf = bufferbudget::parseBufferConf(text);
        const auto issues = bufferIssues(text);
        check(!conf.ram, "buffer=ramm: unset (frames)");
        check(issues.size() == 1 && issues[0].line == 4 && issues[0].error, "buffer=ramm: one error on line 4");
        if (!issues.empty()) std::printf("      %s\n", issues[0].message.c_str());
    }
    {
        const std::string text = head + "buffer_ram_fraction=1.5\nbuffer_ram_max_fraction=0.05\n";
        const auto conf = bufferbudget::parseBufferConf(text);
        const auto issues = bufferIssues(text);
        check(!conf.fraction && !conf.maxFraction, "out of range: unset (default)");
        check(issues.size() == 2 && issues[0].line == 4 && issues[1].line == 5, "out of range: errors on lines 4 and 5");
        for (const auto &i : issues) std::printf("      %s\n", i.message.c_str());
    }
    {
        const std::string text = head + "buffer_ram_floor_mb=abc\n";
        const auto issues = bufferIssues(text);
        check(!bufferbudget::parseBufferConf(text).floorMb && issues.size() == 1, "floor=abc: unset, one error");
        if (!issues.empty()) std::printf("      %s\n", issues[0].message.c_str());
        check(!bufferIssues(head + "buffer_ram_floor_mb=0\n").size(), "floor=0 is valid");
        check(bufferIssues(head + "buffer_ram_floor_mb=-5\n").size() == 1, "floor=-5 is an error");
        check(bufferIssues(head + "buffer_ram_floor_mb=1048577\n").size() == 1, "floor above 1048576 is an error");
    }
    {
        const std::string text = head + "buffer=ram # fast\n";
        check(!bufferbudget::parseBufferConf(text).ram, "trailing comment: ignored (as Core's Python reader keeps it in the value)");
    }
    {
        // frames_per_folder (split a take into part_NNN folders) and its precedence
        const auto conf = bufferbudget::parseBufferConf(head + "frames_per_folder=5000\n");
        check(conf.framesPerFolder && *conf.framesPerFolder == 5000, "frames_per_folder=5000");
        check(bufferbudget::requestedFramesPerFolder(nullptr, conf) == 5000, "frames_per_folder: the file when the env is unset");
        check(bufferbudget::requestedFramesPerFolder("500", conf) == 500, "frames_per_folder: APERTAR_CDNG_FRAMES_PER_FOLDER wins");
        check(bufferbudget::requestedFramesPerFolder("50", conf) == 0, "frames_per_folder: a bad env value is 0 (one folder)");
        check(bufferbudget::requestedFramesPerFolder(nullptr, bufferbudget::parseBufferConf(head)) == 0, "frames_per_folder: default 0");
        const std::string bad = head + "frames_per_folder=99\n";
        const auto issues = bufferIssues(bad);
        check(!bufferbudget::parseBufferConf(bad).framesPerFolder && issues.size() == 1 && issues[0].line == 4 && issues[0].error,
              "frames_per_folder=99: unset, one error on line 4");
        if (!issues.empty()) std::printf("      %s\n", issues[0].message.c_str());
        check(bufferIssues(head + "frames_per_folder=0\n").empty(), "frames_per_folder=0 is valid");
    }
    {
        const std::string text = "[camera]\nbuffer=ram\n";
        check(!bufferbudget::parseBufferConf(text).ram, "buffer= in another section is ignored");
    }
    {
        // set_buffer_mode's edit on the text (the store applies exactly this): key replaced, key added, section added.
        const hwconf::Edit ram{hwconf::Edit::Op::Set, "storage", "buffer", "ram"};
        const std::string withKey = "# top\n[storage]\nmount_path=/media/RAW\n# Write buffer: note\nbuffer=frames\n\n[camera]\nx=1\n";
        const std::string a = hwconf::applyEdits(withKey, {ram});
        check(a == "# top\n[storage]\nmount_path=/media/RAW\n# Write buffer: note\nbuffer=ram\n\n[camera]\nx=1\n", "edit replaces buffer=, keeps comments and other lines");
        const std::string b = hwconf::applyEdits(head + "\n[camera]\nx=1\n", {ram});
        check(b == head + "buffer=ram\n\n[camera]\nx=1\n", "edit adds the missing key at the end of [storage]");
        const std::string c = hwconf::applyEdits("[camera]\nx=1\n", {ram});
        check(bufferbudget::parseBufferConf(c).ram.value_or(false) && c.find("[camera]\nx=1") != std::string::npos, "edit creates a missing [storage]");
        check(bufferIssues(c).empty(), "...and the result validates");
    }
    {
        bufferbudget::BufferConf none, ramConf;
        ramConf.ram = true;
        check(bufferbudget::requestedBufferMode(nullptr, none) == "frames", "request: neither -> frames");
        check(bufferbudget::requestedBufferMode("", ramConf) == "ram", "request: empty env, conf ram -> ram");
        check(bufferbudget::requestedBufferMode("frames", ramConf) == "frames", "request: env frames beats conf ram");
        check(bufferbudget::requestedBufferMode("ram", none) == "ram", "request: env ram beats absent conf");
        check(bufferbudget::requestedBufferMode("ramm", ramConf) == "frames", "request: bad env -> frames");
        bufferbudget::publishBufferSetting("frames", "x");
        bufferbudget::publishBufferRequest("ram");
        const auto snap = bufferbudget::bufferSettingSnapshot();
        check(snap.setting == "ram" && snap.note == "x", "publishBufferRequest keeps the note");
    }
    {
        // buffer_stop_percent: absent, 95, 0, bad values, and the mode-dependent default (90 in ram, off in frames).
        check(!bufferbudget::parseBufferConf(head).stopPercent, "stop percent absent: unset");
        const auto c95 = bufferbudget::parseBufferConf(head + "buffer_stop_percent=95\n");
        check(c95.stopPercent && *c95.stopPercent == 95 && bufferIssues(head + "buffer_stop_percent=95\n").empty(), "stop percent 95: set, no issue");
        const auto c0 = bufferbudget::parseBufferConf(head + "buffer_stop_percent=0\n");
        check(c0.stopPercent && *c0.stopPercent == 0 && bufferIssues(head + "buffer_stop_percent=0\n").empty(), "stop percent 0 (off): set, no issue");
        check(bufferbudget::parseBufferConf(head + "buffer_stop_percent=50\n").stopPercent.value_or(0) == 50, "stop percent 50 is valid");
        check(bufferbudget::parseBufferConf(head + "buffer_stop_percent=99\n").stopPercent.value_or(0) == 99, "stop percent 99 is valid");
        for (const char *bad : {"49", "100", "abc", "", "-5", "9.5", "095x"}) {
            const std::string text = head + "buffer_stop_percent=" + bad + "\n";
            const auto issues = bufferIssues(text);
            check(!bufferbudget::parseBufferConf(text).stopPercent && issues.size() == 1 && issues[0].line == 4 && issues[0].error,
                  std::string("stop percent '") + bad + "': unset, one error on line 4");
            if (!issues.empty() && std::string(bad) == "49") std::printf("      %s\n", issues[0].message.c_str());
        }
        using bufferbudget::effectiveStopPercent;
        using bufferbudget::parseStopPercent;
        const std::optional<unsigned> none;
        check(effectiveStopPercent(true, none, none) == 90, "default in ram mode is 90");
        check(effectiveStopPercent(false, none, none) == 0, "default in frames mode is off (0)");
        check(effectiveStopPercent(false, 80u, none) == 80, "key set in frames mode applies");
        check(effectiveStopPercent(true, 0u, none) == 0, "key 0 turns it off in ram mode");
        check(effectiveStopPercent(true, 95u, none) == 95, "key 95 in ram mode");
        check(effectiveStopPercent(true, 95u, 70u) == 70 && effectiveStopPercent(false, none, 60u) == 60, "env wins over key and over the mode default");
        check(effectiveStopPercent(true, 95u, 0u) == 0, "env 0 wins over key 95");
        check(parseStopPercent("90") == 90u && parseStopPercent("0") == 0u, "env 90 and 0 parse");
        check(!parseStopPercent("49") && !parseStopPercent("100") && !parseStopPercent("abc") && !parseStopPercent("") && !parseStopPercent(nullptr),
              "env 49/100/abc/empty/absent: unset (falls to the key, then the default)");
        check(effectiveStopPercent(true, 95u, parseStopPercent("abc")) == 95, "bad env falls back to the key");
        check(!bufferbudget::safetyStopReached(80, 100, 90) && bufferbudget::safetyStopReached(90, 100, 90) && !bufferbudget::safetyStopReached(100, 100, 0)
                  && !bufferbudget::safetyStopReached(5, 0, 90) && bufferbudget::safetyStopReached(3'000'000'000ull, 3'200'000'000ull, 90),
              "safetyStopReached: threshold, off, empty capacity, bytes");
    }
    std::printf("%s\n", g_failed ? "FAILED" : "all ok");
    return g_failed ? 1 : 0;
}
