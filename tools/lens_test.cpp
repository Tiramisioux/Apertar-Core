// Host test for src/hardware/Cef168Lens.hpp and LensLibrary.hpp: the data frame and its CRC, the subdev name, the third-stop
// iris table and stepping, focus steps, the lens library (keys, matching, save/replace/delete, the file) and the get_lens JSON. Build: g++ -std=c++20 -I src tools/lens_test.cpp

#include "hardware/Cef168Lens.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace cef = apertar::hardware::cef168;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void put16(uint8_t *at, unsigned value) { at[0] = value & 0xFF; at[1] = (value >> 8) & 0xFF; }

static void frame(uint8_t (&raw)[15], uint8_t id, uint8_t flags, unsigned time, unsigned lo, unsigned hi, unsigned cur,
                  unsigned dmin, unsigned dmax)
{
    raw[0] = id; raw[1] = flags;
    put16(raw + 2, time); put16(raw + 4, lo); put16(raw + 6, hi); put16(raw + 8, cur); put16(raw + 10, dmin); put16(raw + 12, dmax);
    raw[14] = cef::crc8(raw, 14);
}

int main()
{
    // CRC-8, poly 0xA8, init 0xFF, MSB first: the value the board's command frames carry (INP_SET_APERTURE 560).
    const uint8_t zero[1] = {0};
    CHECK(cef::crc8(zero, 0) == 0xFF);
    uint8_t bit = 0x80;
    uint8_t expected = 0xFF ^ bit;
    for (int i = 0; i < 8; ++i) expected = (expected & 0x80) ? uint8_t((expected << 1) ^ 0xA8) : uint8_t(expected << 1);
    CHECK(cef::crc8(&bit, 1) == expected);

    uint8_t raw[15];
    frame(raw, 7, 0x01 | (2 << 1), 321, 100, 1100, 600, 45, cef::kInfiniteCm);
    std::string error;
    const auto data = cef::decode(raw, 15, &error);
    CHECK(data.has_value());
    if (data) {
        CHECK(data->lensId == 7 && data->moving && data->calibrating == 2 && data->movingTime == 321);
        CHECK(data->focusMin == 100 && data->focusMax == 1100 && data->focusCurrent == 600);
        CHECK(data->distanceMinCm == 45 && data->distanceMaxCm == cef::kInfiniteCm);
        CHECK(data->rangeValid() && data->span() == 1000);
    }
    raw[8] ^= 1;   // a flipped bit fails the CRC
    CHECK(!cef::decode(raw, 15, &error) && !error.empty());
    CHECK(!cef::decode(raw, 14, &error));
    frame(raw, 0, 0, 0, 0, 32767, 0, 0, 0);
    CHECK(cef::decode(raw, 15) && !cef::decode(raw, 15)->rangeValid());   // never calibrated
    frame(raw, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(!cef::decode(raw, 15)->rangeValid());

    // A real frame, read from an uncalibrated Canon EF-S 10-22 on AF: the range reads 0..65535.
    const uint8_t real[15] = {0xeb, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x70};
    CHECK(cef::crc8(real, 14) == 0x70);
    const auto live = cef::decode(real, 15, &error);
    CHECK(live && live->lensId == 235 && !live->moving && live->calibrating == 0 && live->movingTime == 1);
    CHECK(live && live->focusMin == 0 && live->focusMax == 65535 && live->focusCurrent == 0 && !live->rangeValid());
    CHECK(live && live->distanceMinCm == 65535 && live->distanceMaxCm == 65535);
    CHECK(cef::focusRangeValid(0, 1203) && !cef::focusRangeValid(0, 0) && !cef::focusRangeValid(0, 32767) &&
          !cef::focusRangeValid(0, 65535) && !cef::focusRangeValid(500, 100));

    // The subdev name.
    CHECK(cef::busOfSubdevName("cef168 0-000d\n") == 0);
    CHECK(cef::busOfSubdevName("cef168 10-000d") == 10);
    CHECK(cef::busOfSubdevName("cef168") == -1);
    CHECK(!cef::busOfSubdevName("imx477 10-001a"));
    CHECK(!cef::busOfSubdevName("cef1680 0-000d"));

    // Iris: whole table without a range, f x 100 on the wire.
    cef::Settings none;
    CHECK(cef::irisSteps(none).size() == 31 && cef::irisSteps(none).front() == 1.0 && cef::irisSteps(none).back() == 32.0);
    CHECK(cef::irisControlValue(5.6) == 560 && cef::irisControlValue(1.0) == 100 && cef::irisControlValue(0.0) == -1);
    CHECK(cef::irisControlValue(200.0) == -1 && cef::irisControlValue(std::nan("")) == -1);

    // A lens that opens to 1.8 and stops down to 22.6: the range ends are on offer.
    cef::Settings lens;
    lens.apertureMin = 1.8;
    lens.apertureMax = 22.6;
    const auto steps = cef::irisSteps(lens);
    CHECK(steps.front() == 1.8 && steps.back() == 22.6);
    CHECK(std::find(steps.begin(), steps.end(), 22.0) != steps.end() && std::find(steps.begin(), steps.end(), 1.6) == steps.end());
    CHECK(cef::clampIris(1.2, lens) == 1.8 && cef::clampIris(40.0, lens) == 22.6 && cef::clampIris(5.6, lens) == 5.6);
    CHECK(cef::clampIris(40.0, none) == 32.0);
    // An end that falls on the table is not listed twice.
    cef::Settings exact;
    exact.apertureMin = 2.8;
    exact.apertureMax = 8.0;
    CHECK(cef::irisSteps(exact) == (std::vector<double>{2.8, 3.2, 3.5, 4.0, 4.5, 5.0, 5.6, 6.3, 7.1, 8.0}));
    // One end entered leaves the other open; ends entered the wrong way round still work.
    cef::Settings oneEnd;
    oneEnd.apertureMax = 4.0;
    CHECK(cef::irisSteps(oneEnd).front() == 1.0 && cef::irisSteps(oneEnd).back() == 4.0);
    cef::Settings swapped;
    swapped.apertureMin = 8.0;
    swapped.apertureMax = 2.8;
    CHECK(cef::irisSteps(swapped) == cef::irisSteps(exact));

    // Stepping: positive closes. Off-table values snap in the direction of travel; the ends hold.
    CHECK(cef::stepIris(5.6, 1, none) == 6.3 && cef::stepIris(5.6, -1, none) == 5.0);
    CHECK(cef::stepIris(5.6, 3, none) == 8.0 && cef::stepIris(5.6, -3, none) == 4.0);
    CHECK(cef::stepIris(2.9, 1, none) == 3.2 && cef::stepIris(2.9, -1, none) == 2.8);
    CHECK(cef::stepIris(32.0, 1, none) == 32.0 && cef::stepIris(1.0, -1, none) == 1.0);
    CHECK(cef::stepIris(30.0, 5, none) == 32.0 && cef::stepIris(1.1, -5, none) == 1.0);
    CHECK(cef::stepIris(std::nullopt, 1, none) == 1.0);
    CHECK(cef::stepIris(std::nullopt, -1, lens) == 1.8);
    CHECK(cef::stepIris(22.0, 1, lens) == 22.6 && cef::stepIris(22.6, 1, lens) == 22.6);
    CHECK(cef::stepIris(1.8, -1, lens) == 1.8 && cef::stepIris(1.8, 1, lens) == 2.0);
    CHECK(cef::stepIris(1.0, 1, lens) == 1.8);     // a value outside the range comes back inside
    CHECK(cef::stepIris(40.0, -1, lens) == 22.6);
    CHECK(cef::stepIris(5.6, 0, lens) == 5.6);
    // The pot: ends and the middle of the listed steps.
    CHECK(cef::irisAtFraction(0.0, exact) == 2.8 && cef::irisAtFraction(1.0, exact) == 8.0);
    CHECK(cef::irisAtFraction(-1.0, exact) == 2.8 && cef::irisAtFraction(2.0, exact) == 8.0);

    // Focus: 1 % of the range, at least 1, kept inside the range; nothing without a calibrated range.
    cef::Data focus;
    focus.focusMin = 100; focus.focusMax = 1100; focus.focusCurrent = 600;
    CHECK(cef::focusStepSize(focus, 1.0) == 10 && cef::focusStepSize(focus, 0.01) == 1 && cef::focusStepSize(focus, 5.0) == 50);
    CHECK(cef::stepFocus(focus, 1, 1.0) == 610 && cef::stepFocus(focus, -3, 1.0) == 570);
    focus.focusCurrent = 1095;
    CHECK(cef::stepFocus(focus, 1, 1.0) == 1100 && cef::stepFocus(focus, 100, 1.0) == 1100);
    focus.focusCurrent = 102;
    CHECK(cef::stepFocus(focus, -1, 1.0) == 100);
    CHECK(cef::focusAtFraction(0.0, focus) == 100 && cef::focusAtFraction(1.0, focus) == 1100 && cef::focusAtFraction(0.5, focus) == 600);
    cef::Data uncalibrated;
    CHECK(cef::focusStepSize(uncalibrated, 1.0) == 0);

    // The JSON: absent lens, and a present one with one control.
    cef::Status absent;
    const std::string off = cef::statusJson(absent, "Manual");
    CHECK(off.find("\"state\":\"absent\"") != std::string::npos && off.find("\"lenses\":[]") != std::string::npos);
    CHECK(off.find("\"present\":false") != std::string::npos && off.find("\"lens_id\":null") != std::string::npos);
    CHECK(off.find("\"aperture_min\":null") != std::string::npos && off.find("\"controls\":[]") != std::string::npos);
    CHECK(off.find("\"focus_step_percent\":1,\"iris_step_thirds\":1,\"af_mode\":\"Manual\"") != std::string::npos);
    cef::Status on;
    on.present = true;
    on.subdev = "/dev/v4l-subdev3";
    on.bus = 0;
    on.data = *data;
    on.data->calibrating = 0;
    on.iris = 5.6;
    cef::Control control;
    control.id = 10094858; control.name = "Focus, Absolute"; control.type = "integer"; control.maximum = 1100;
    control.step = 1; control.value = 600; control.isVolatile = true; control.executeOnWrite = true;
    cef::Control iris;
    iris.id = 10094865; iris.name = "Iris, Absolute"; iris.type = "integer"; iris.writeOnly = true;
    on.controls = {control, iris};
    on.settings.apertureMin = 1.8;
    on.library.working.name = "Sigma";
    const std::string json = cef::statusJson(on, "Continuous");
    CHECK(json.find("\"state\":\"unknown_lens\"") != std::string::npos && json.find("\"lens_name\":\"Sigma\"") != std::string::npos);
    CHECK(json.find("\"unsaved\":true") != std::string::npos && json.find("\"lens_key\":null") != std::string::npos);
    CHECK(json.find("\"calibration\":{\"state\":\"idle\"") != std::string::npos);
    CHECK(cef::statusSummaryJson(on).find("\"focus_position\":600") != std::string::npos);
    CHECK(json.find("\"present\":true,\"state\":\"unknown_lens\",\"subdev\":\"/dev/v4l-subdev3\",\"bus\":0,\"lens_id\":7") != std::string::npos);
    CHECK(json.find("\"distance_max_cm\":null") != std::string::npos && json.find("\"distance_min_cm\":45") != std::string::npos);
    CHECK(json.find("\"iris\":5.6,\"aperture_min\":1.8,\"aperture_max\":null") != std::string::npos);
    CHECK(json.find("\"name\":\"Focus, Absolute\"") != std::string::npos && json.find("\"value\":600") != std::string::npos);
    CHECK(json.find("\"write_only\":true") != std::string::npos && json.find("\"value\":null") != std::string::npos);
    CHECK(json.find("\"error\":null") != std::string::npos);

    // ---- the state word -------------------------------------------------------------------------------------------------
    cef::Status st;
    CHECK(cef::lensState(st) == "absent");
    st.present = true;
    CHECK(cef::lensState(st) == "error");
    st.data = *cef::decode(real, 15);
    st.data->lensId = 0;
    CHECK(cef::lensState(st) == "no_lens");
    st.data->lensId = 235;
    CHECK(cef::lensState(st) == "unknown_lens");
    cef::Library lib;
    {
        cef::Entry e;
        e.key = "ef-s-10-22";
        e.name = "EF-S 10-22";
        e.lensId = 235;
        lib.entries.push_back(e);
        lib.selected = e.key;
    }
    st.library = lib;
    CHECK(cef::lensState(st) == "uncalibrated");   // the board's range 0..65535
    st.data->focusMax = 1203;
    CHECK(cef::lensState(st) == "ready");
    st.data->calibrating = 2;
    CHECK(cef::lensState(st) == "calibrating");
    st.data->calibrating = 0;
    st.calibration.state = "running";
    CHECK(cef::lensState(st) == "calibrating");

    // ---- the library ----------------------------------------------------------------------------------------------------
    CHECK(cef::slug("Sigma 18-35 f/1.8") == "sigma-18-35-f1-8" && cef::slug("24/105 mm") == "24-105-mm");
    CHECK(cef::slug("  Zeiss  Ø ") == "zeiss" && cef::slug("???") == "lens" && cef::slug("") == "lens");
    CHECK(cef::cleanName("  a\n b\t ") == "a b" && cef::cleanName(std::string(100, 'x')).size() == 64);
    CHECK(cef::cleanName(std::string(63, 'x') + "\xc3\xa5").size() == 63);   // not cut in the middle of a character

    cef::Library l;
    l.working.lensId = 7;
    l.working.name = "Sigma 18-35";
    l.working.apertureMin = 1.8;
    l.working.apertureMax = 16;
    CHECK(cef::unsaved(l));
    CHECK(cef::saveAs(l, "", "t1") != "");
    CHECK(cef::saveAs(l, " Sigma  18-35 ", "2026-01-01T00:00:00Z").empty());
    CHECK(l.selected == "sigma-18-35" && l.entries.size() == 1 && l.entries[0].lensId == 7 && l.entries[0].name == "Sigma 18-35");
    CHECK(!cef::unsaved(l));
    // The same name again gets another key, and becomes the one in use.
    l.working.apertureMax = 22;
    CHECK(cef::unsaved(l));
    CHECK(cef::saveAs(l, "Sigma 18-35", "2026-02-01T00:00:00Z").empty());
    CHECK(l.selected == "sigma-18-35-2" && l.entries.size() == 2 && l.entries[0].apertureMax == 16 && l.entries[1].apertureMax == 22);
    // Both have id 7: the most recently used wins, a tie goes to the later one.
    CHECK(cef::matching(l, 7).size() == 2 && cef::matching(l, 8).empty());
    CHECK(cef::mostRecent(l, 7)->key == "sigma-18-35-2" && !cef::mostRecent(l, 8));
    CHECK(cef::select(l, "sigma-18-35", "2026-03-01T00:00:00Z").empty());
    CHECK(l.selected == "sigma-18-35" && l.working.apertureMax == 16 && cef::mostRecent(l, 7)->key == "sigma-18-35");
    CHECK(!cef::select(l, "nope", "t").empty() && l.selected == "sigma-18-35");
    // Selecting discards unsaved changes; the iris follows the lens without making it unsaved.
    l.working.apertureMax = 99;
    CHECK(cef::unsaved(l));
    CHECK(cef::select(l, "sigma-18-35", "2026-03-02T00:00:00Z").empty() && l.working.apertureMax == 16 && !cef::unsaved(l));
    cef::rememberIris(l, 5.6);
    CHECK(!cef::unsaved(l) && l.working.lastIris == 5.6 && cef::find(l, "sigma-18-35")->lastIris == 5.6);
    // Save over renames: the key follows the name, a taken key gets a number, the selection follows.
    l.working.name = "Sigma 18-35 f/1.8";
    CHECK(cef::unsaved(l));
    CHECK(cef::saveOver(l, "2026-03-03T00:00:00Z").empty());
    CHECK(l.selected == "sigma-18-35-f1-8" && l.entries.size() == 2 && !cef::find(l, "sigma-18-35") && !cef::unsaved(l));
    l.working.name = "Sigma 18-35";   // the other entry's slug is sigma-18-35-2: no clash with this one's old key
    CHECK(cef::saveOver(l, "t").empty() && l.selected == "sigma-18-35");
    l.working.name = "Sigma 18-35";
    CHECK(cef::select(l, "sigma-18-35-2", "t").empty() && cef::saveOver(l, "t").empty() && l.selected == "sigma-18-35-2");   // unchanged name keeps its key
    l.working.name = "";
    CHECK(!cef::saveOver(l, "t").empty());
    cef::Library empty;
    CHECK(!cef::saveOver(empty, "t").empty());
    // Detection: keep the selection of the same id, else pick, else blank.
    CHECK(!cef::adopt(l, 7, "t"));
    CHECK(cef::adopt(l, 9, "t") && l.selected.empty() && l.working.lensId == 9 && l.working.name.empty() && !cef::unsaved(l));
    CHECK(!cef::adopt(l, 9, "t"));
    l.working.name = "Mystery";
    CHECK(!cef::adopt(l, 9, "t") && l.working.name == "Mystery");   // an unsaved lens stays while its id stays
    CHECK(cef::adopt(l, 7, "2026-04-01T00:00:00Z") && l.selected == "sigma-18-35-2");
    // Delete: the working lens stays, now unsaved; a delete of another entry leaves the selection.
    CHECK(!cef::remove(l, "nope").empty());
    CHECK(cef::remove(l, "sigma-18-35").empty() && l.selected == "sigma-18-35-2" && l.entries.size() == 1);
    CHECK(cef::remove(l, "sigma-18-35-2").empty() && l.selected.empty() && l.entries.empty() && cef::unsaved(l));

    // Calibration values make a lens "calibrated"; the file round-trips everything.
    cef::Library f;
    cef::Entry a;
    a.key = "a";
    a.name = "Canon EF-S 10-22 = wide";
    a.lensId = 235;
    a.lastUsed = "2026-05-01T10:00:00Z";
    a.apertureMin = 3.5;
    a.apertureMax = 22.6;
    a.lastIris = 5.6;
    a.focusMin = 0; a.focusMax = 1203; a.distanceMinCm = 22; a.distanceMaxCm = cef::kInfiniteCm;
    a.capFocus = true; a.capAutofocus = false;
    CHECK(a.calibrated());
    cef::Entry b;
    b.key = "b";
    b.name = "Plain";
    f.entries = {a, b};
    f.selected = "a";
    f.working = a;
    f.working.name = "Edited";
    const std::string text = cef::serialize(f);
    const cef::Library g = cef::parse(text);
    CHECK(g.entries.size() == 2 && g.selected == "a" && g.working.key == "a" && g.working.name == "Edited");
    CHECK(g.entries[0].name == a.name && g.entries[0].lensId == 235 && g.entries[0].lastUsed == a.lastUsed);
    CHECK(g.entries[0].apertureMin == 3.5 && g.entries[0].apertureMax == 22.6 && g.entries[0].lastIris == 5.6);
    CHECK(g.entries[0].focusMax == 1203 && g.entries[0].distanceMaxCm == cef::kInfiniteCm && g.entries[0].calibrated());
    CHECK(g.entries[0].capFocus == true && g.entries[0].capAutofocus == false && !g.entries[0].capIris);
    CHECK(g.entries[1].lensId == -1 && !g.entries[1].focusMin && g.entries[1].apertureMin == 0.0 && !g.entries[1].calibrated());
    CHECK(cef::serialize(g) == text);
    CHECK(cef::unsaved(g));
    CHECK(cef::parse("selected=gone\n[lens.x]\nname=X\n").selected.empty());   // a selection that is not in the file
    CHECK(cef::parse("garbage\n[lens.]\n[other]\nname=q\n").entries.empty());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("lens_test_" + std::to_string(::getpid()));
    const std::filesystem::path file = dir / "sub" / "lenses.conf";
    CHECK(cef::loadFile(file).entries.empty());   // no file yet
    CHECK(cef::saveFile(file, f).empty());
    CHECK(cef::serialize(cef::loadFile(file)) == text);
    f.entries.pop_back();
    CHECK(cef::saveFile(file, f).empty() && cef::loadFile(file).entries.size() == 1);
    int leftovers = 0;
    for (const auto &entry : std::filesystem::directory_iterator(file.parent_path())) leftovers += entry.path() != file;
    CHECK(leftovers == 0);   // no tmp file left behind
    std::filesystem::remove_all(dir);

    if (failures == 0) std::puts("lens_test: ok");
    return failures == 0 ? 0 : 1;
}
