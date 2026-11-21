// Host test for src/camera/FpsCeiling.hpp (the IMX585 lossless cDNG fps limit).
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
