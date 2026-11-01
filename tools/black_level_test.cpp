// Host test for src/cdng/BlackLevel.hpp: the black and white level cDNG and MLV both declare for a raw
// container (MLV used to scale the black only for 8 and 10 bit, so 16-bit Clear HDR declared 200, not ~3200).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/black_level_test.cpp -o /tmp/blt && /tmp/blt
#include "cdng/BlackLevel.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace apertar;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

int main()
{
    // The 12-bit reference pedestal (200) at every depth the encoders write.
    check(encodedBlackLevel(200.0f, RawBitDepth::Bits12) == 200, "12-bit: 200 stays 200");
    check(encodedBlackLevel(200.0f, RawBitDepth::Bits10) == 50, "10-bit: 200 -> 50");
    check(encodedBlackLevel(200.0f, RawBitDepth::Bits8) == 12, "8-bit: 200 -> 12");
    const int32_t black16 = encodedBlackLevel(200.0f, RawBitDepth::Bits16);
    check(black16 > 3000, "16-bit: 200 is not left at 200 (was the MLV bug)");
    check(std::abs(black16 - 3200) <= 1, "16-bit: 200 -> the 16x left-shifted pedestal (3200, +-1)");

    // Clear HDR 16-bit as libcamera reports it: 3200 on the 16-bit scale, divided by 16 into the 12-bit
    // reference scale by metadataFromRequest, then back up by the helper.
    check(std::abs(encodedBlackLevel(3200.0f / 16.0f, RawBitDepth::Bits16) - 3200) <= 1,
          "16-bit Clear HDR: 3200 / 16 round-trips to ~3200");

    // Values already on the container scale (CCMP codes12 table, CineMate Log) are not scaled again.
    check(encodedBlackLevel(3200.0f, RawBitDepth::Bits16, true) == 3200, "container scale 16-bit: 3200 stays 3200");
    check(encodedBlackLevel(200.0f, RawBitDepth::Bits12, true) == 200, "container scale 12-bit: 200 stays 200");
    check(encodedBlackLevel(50.0f, RawBitDepth::Bits10, true) == 50, "container scale 10-bit: 50 stays 50");

    // Never at or above the white level, never negative, never NaN-driven garbage.
    check(encodedBlackLevel(1.0e9f, RawBitDepth::Bits16, true) == 65534, "huge black clamps to white - 1 (16-bit)");
    check(encodedBlackLevel(1.0e9f, RawBitDepth::Bits8, true) == 254, "huge black clamps to white - 1 (8-bit)");
    check(encodedBlackLevel(-5.0f, RawBitDepth::Bits12) == 0, "negative black -> 0");
    check(encodedBlackLevel(std::numeric_limits<float>::quiet_NaN(), RawBitDepth::Bits16) == 0, "NaN black -> 0");

    check(encodedWhiteLevel(RawBitDepth::Bits8) == 255, "white 8-bit");
    check(encodedWhiteLevel(RawBitDepth::Bits10) == 1023, "white 10-bit");
    check(encodedWhiteLevel(RawBitDepth::Bits12) == 4095, "white 12-bit");
    check(encodedWhiteLevel(RawBitDepth::Bits16) == 65535, "white 16-bit");

    for (RawBitDepth depth : { RawBitDepth::Bits8, RawBitDepth::Bits10, RawBitDepth::Bits12, RawBitDepth::Bits16 }) {
        const int32_t black = encodedBlackLevel(kDefaultSourceBlackLevel, depth);
        check(black > 0 && static_cast<uint32_t>(black) < encodedWhiteLevel(depth),
              "default pedestal sits between 0 and white at every depth");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
