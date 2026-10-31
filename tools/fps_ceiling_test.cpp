// Host test for src/camera/FpsCeiling.hpp (the sensor probe is the maximum fps; the table is the fallback).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/fps_ceiling_test.cpp -o /tmp/fct && /tmp/fct
#include "camera/FpsCeiling.hpp"

#include <cmath>
#include <cstdio>

using namespace apertar::fpsceiling;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}
static bool same(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main()
{
    check(same(resolveSensorMaxFps(60.0, 30.0, false), 60.0), "4K SDR: probed 60, table 30 -> 60");
    check(same(applyBackendCap(resolveSensorMaxFps(30.0, 30.0, false), 25.0), 25.0),
          "4K Clear HDR 16-bit: probed 30, backend cap 25 -> 25");
    check(same(applyBackendCap(resolveSensorMaxFps(30.0, 30.0, false), std::nullopt), 30.0),
          "Clear HDR 12-bit: probed 30, no cap -> 30");
    check(same(resolveSensorMaxFps(std::nullopt, 30.0, false), 30.0), "no probe: table 30 -> 30");
    check(same(resolveSensorMaxFps(60.0, 30.0, true), 30.0), "table-only switch: probed 60, table 30 -> 30");
    check(same(resolveSensorMaxFps(20.0, 30.0, true), 20.0), "table-only switch: probed 20, table 30 -> 20");
    check(same(resolveSensorMaxFps(85.66, 60.0, false), 85.66), "crop: probed 85.66, table 60 -> 85.66");
    check(same(resolveSensorMaxFps(0.0, 30.0, false), 30.0), "probed 0 is absent -> table");
    check(same(resolveSensorMaxFps(-1.0, 30.0, true), 30.0), "probed negative is absent -> table");
    check(same(applyBackendCap(60.0, 25.0), 25.0) && same(applyBackendCap(20.0, 25.0), 20.0),
          "backend cap only lowers");
    check(same(losslessImx585Fps(60.0, std::nullopt), 60.0), "lossless IMX585 4K SDR 12-bit: no override -> mode max 60");
    check(same(losslessImx585Fps(25.0, std::nullopt), 25.0), "lossless IMX585 Clear HDR 16-bit: mode max 25 -> 25");
    check(same(losslessImx585Fps(60.0, 30.0), 30.0), "override 30 restores the old cap");
    check(same(losslessImx585Fps(60.0, 45.0), 45.0), "override 45 below the mode max -> 45");
    check(same(losslessImx585Fps(60.0, 90.0), 60.0), "override 90 never passes the sensor max");
    check(same(losslessImx585Fps(25.0, 30.0), 25.0), "override 30 never passes the 25 backend cap");
    check(!parseLosslessFpsLimit(nullptr) && !parseLosslessFpsLimit("") && !parseLosslessFpsLimit("0") &&
              !parseLosslessFpsLimit("-5") && !parseLosslessFpsLimit("abc") && !parseLosslessFpsLimit("30x"),
          "parse: unset, empty, 0, negative, text, junk -> no override");
    check(parseLosslessFpsLimit("30") && same(*parseLosslessFpsLimit("30"), 30.0) &&
              same(*parseLosslessFpsLimit("47.5"), 47.5),
          "parse: 30 and 47.5");
    return g_failed ? 1 : 0;
}
