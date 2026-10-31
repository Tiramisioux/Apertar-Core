// Host test for src/util/StoragePreroll.hpp (the [media] keys, the guard, the trigger decision).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/storage_preroll_test.cpp -o /tmp/spt && /tmp/spt
#include "hardware/HardwareConfigText.hpp"
#include "util/StoragePreroll.hpp"

#include <cstdio>

using namespace apertar::storagepreroll;
namespace hwconf = apertar::hwconf;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

int main()
{
    // keys
    check(prerollConfProblem("preroll", "on").empty() && prerollConfProblem("preroll", "off").empty(), "preroll takes on and off");
    check(!prerollConfProblem("preroll", "yes").empty() && !prerollConfProblem("preroll", "").empty(), "preroll refuses yes and empty");
    check(prerollConfProblem("preroll_seconds", "1").empty() && prerollConfProblem("preroll_seconds", "10").empty(), "seconds 1 and 10 are good");
    check(!prerollConfProblem("preroll_seconds", "0").empty() && !prerollConfProblem("preroll_seconds", "11").empty() &&
          !prerollConfProblem("preroll_seconds", "2.5").empty() && !prerollConfProblem("preroll_seconds", "-1").empty() &&
          !prerollConfProblem("preroll_seconds", "").empty() && !prerollConfProblem("preroll_seconds", "100").empty(), "seconds 0, 11, 2.5, -1, empty, 100 are refused");
    check(prerollConfProblem("mount_path", "x").empty(), "another key is not judged");

    // the Config pane's validator: [media] is a known section, bad values are reported with their line number
    {
        const std::string good = "[media]\npreroll=on\npreroll_seconds=2\n";
        const auto issues = hwconf::validate(good, hwconf::Facts{});
        bool clean = true;
        for (const auto &issue : issues) clean = clean && issue.section != "media" && issue.rule != "unknown_section";
        check(clean, "[media] with good values: no issue, not an unknown section");
        const std::string bad = "[media]\npreroll=yes\npreroll_seconds=11\n";
        int found = 0; bool lines = true;
        for (const auto &issue : hwconf::validate(bad, hwconf::Facts{}))
            if (issue.rule == "preroll_value") { ++found; lines = lines && issue.message.find("on line ") != std::string::npos; }
        check(found == 2 && lines, "two bad [media] values give two issues that name their line");
    }

    // parse
    const std::string file =
        "[storage]\npreroll=off\n[media]\n# comment\npreroll = off\npreroll_seconds=5\n[display]\npreroll=on\n";
    PrerollConf conf = parsePrerollConf(file);
    check(conf.automatic && !*conf.automatic && conf.seconds && *conf.seconds == 5, "parse reads [media] only");
    conf = parsePrerollConf("[media]\npreroll=on\npreroll=maybe\npreroll_seconds=99\npreroll_seconds=3 # x\n");
    check(conf.automatic && *conf.automatic && !conf.seconds, "bad lines and trailing comments are ignored, last good line counts");
    conf = parsePrerollConf("[MEDIA]\nPreroll=OFF\n");
    check(conf.automatic && !*conf.automatic, "section and key case-insensitive");
    check(!parsePrerollConf("").automatic && !parsePrerollConf("[storage]\nbuffer=ram\n").seconds, "nothing set");

    // env beats file
    PrerollConf off; off.automatic = false;
    PrerollConf on; on.automatic = true;
    check(automaticEnabled(nullptr, PrerollConf{}), "default on");
    check(!automaticEnabled(nullptr, off) && automaticEnabled(nullptr, on), "file decides without env");
    check(!automaticEnabled("0", on) && automaticEnabled("1", off), "env 0/1 beats the file");
    check(!automaticEnabled("x", off) && automaticEnabled("", PrerollConf{}), "other env values are unset");

    // seconds
    PrerollConf five; five.seconds = 5;
    check(effectiveSeconds(std::nullopt, PrerollConf{}) == 2, "default 2 s");
    check(effectiveSeconds(std::nullopt, five) == 5 && effectiveSeconds(7u, five) == 7, "file, then the argument wins");
    check(effectiveSeconds(0u, five) == 1 && effectiveSeconds(99u, five) == 10, "clamped 1..10");
    check(parseSecondsArgument("3") == std::optional<unsigned>(3) && !parseSecondsArgument("0") && !parseSecondsArgument("abc"), "seconds argument");

    // guard
    GuardInputs in;
    check(decideGuard(in, Trigger::Manual).run, "idle camera runs");
    struct Case { const char *name; void (*set)(GuardInputs &); const char *reason; };
    const Case cases[] = {
        {"recording", [](GuardInputs &g) { g.recording = true; }, "recording"},
        {"sound", [](GuardInputs &g) { g.soundRecording = true; }, "sound recording"},
        {"timelapse", [](GuardInputs &g) { g.timelapse = true; }, "time-lapse"},
        {"no media", [](GuardInputs &g) { g.noMedia = true; }, "no media"},
        {"media unusable", [](GuardInputs &g) { g.mediaUsable = false; }, "media not usable"},
        {"standby", [](GuardInputs &g) { g.standby = true; }, "standby"},
        {"delete", [](GuardInputs &g) { g.deleteActive = true; }, "deleting media"},
        {"format", [](GuardInputs &g) { g.formatActive = true; }, "formatting"},
        {"speed test", [](GuardInputs &g) { g.speedTest = true; }, "media speed test"},
        {"running", [](GuardInputs &g) { g.prerollActive = true; }, "a pre-roll is already running"},
        {"camera", [](GuardInputs &g) { g.cameraReady = false; }, "camera not ready"},
        {"still", [](GuardInputs &g) { g.stillMode = true; }, "still mode"},
        {"format kind", [](GuardInputs &g) { g.rawFormat = false; }, "not a raw format"},
    };
    for (const Case &c : cases) {
        GuardInputs g;
        c.set(g);
        const GuardDecision d = decideGuard(g, Trigger::Insert);
        check(!d.run && d.reason == c.reason, c.name);
    }
    GuardInputs two; two.recording = true; two.noMedia = true;
    check(decideGuard(two, Trigger::Start).reason == "recording", "the first reason in the list is reported");

    // mount change
    MountChange c;
    c.currentMounted = true; c.currentDevice = "/dev/nvme0n1p1";
    check(triggerForMountChange(c) == std::optional<Trigger>(Trigger::Start), "first cycle with a card = start");
    c.startDone = true;
    check(!triggerForMountChange(c), "first cycle twice is not a second start");
    c.startDone = false; c.currentMounted = false;
    check(!triggerForMountChange(c), "first cycle without a card = nothing");
    c.havePrevious = true; c.startDone = true; c.currentMounted = true;
    check(triggerForMountChange(c) == std::optional<Trigger>(Trigger::Insert), "no card, then a card = insert");
    c.previousMounted = true; c.previousDevice = "/dev/nvme0n1p1";
    check(!triggerForMountChange(c), "same device, other property = nothing");
    c.previousDevice = "/dev/sda1";
    check(triggerForMountChange(c) == std::optional<Trigger>(Trigger::Insert), "another device = insert");
    c.formatRunning = true;
    check(!triggerForMountChange(c), "during a format = nothing (the format requests it)");
    c.formatRunning = false; c.msSinceFormat = 3000;
    check(!triggerForMountChange(c), "just after a format = nothing");
    c.msSinceFormat = kFormatQuietMs + 1;
    check(triggerForMountChange(c) == std::optional<Trigger>(Trigger::Insert), "long after a format = insert");
    c.currentMounted = false;
    check(!triggerForMountChange(c), "card gone = nothing");
    c = MountChange{}; c.havePrevious = true; c.startDone = false; c.currentMounted = true; c.previousMounted = false;
    check(triggerForMountChange(c) == std::optional<Trigger>(Trigger::Insert), "start never done but earlier cycle seen = insert");

    check(std::string(triggerName(Trigger::Insert)) == "mount" && std::string(triggerName(Trigger::Start)) == "start", "trigger names");
    check(lastOk(2.0, 60, 118, 1900000000ull) == "ok: 2.0 s at 60 fps, 118 frames, 1.9 GB written and deleted", "ok text");
    check(lastOk(2.0, 60, 120, 1900000000ull, 59.94) == "ok: 2.0 s at 60 fps (measured 59.9), 120 frames, 1.9 GB written and deleted",
          "ok text with the measured rate");
    check(std::abs(measuredFps(120, 1000000, 3000000 - 16667 + 0) - 119.0 * 1e6 / 1983333.0) < 1e-9, "measured fps formula");
    check(measuredFps(61, 5000000, 7000000) == 30.0, "61 frames over 2 s are 30 fps");
    check(measuredFps(1, 5, 9) == 0.0 && measuredFps(0, 0, 0) == 0.0 && measuredFps(10, 9, 9) == 0.0 && measuredFps(10, 9, 3) == 0.0,
          "measured fps is 0 when it cannot be measured");

    check(!prerollActive() && prerollLast().empty(), "state starts idle and empty");
    prerollState().active.store(true); setPrerollLast("skipped: recording");
    check(prerollActive() && prerollLast() == "skipped: recording", "state is readable");

    std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
