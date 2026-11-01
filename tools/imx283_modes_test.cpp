// Host test for src/camera/Imx283Modes.hpp and activeAreaScalerCrop() in src/camera/ScalerCropGeometry.hpp.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/imx283_modes_test.cpp -o /tmp/i283 && /tmp/i283
#include "camera/Imx283Modes.hpp"
#include "camera/ScalerCropGeometry.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

using namespace apertar;
using namespace apertar::imx283;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

// The brief's table, typed in separately from the header: bits, binning, active, transport, fps.
struct Expected { uint32_t bits, binning, aw, ah, tw, th; double fps; };
static const Expected kExpected[] = {
    {12, 1, 5472, 3648, 5568, 3664, 21.401},
    {12, 2, 2736, 1824, 2784, 1828, 51.796},
    {12, 3, 1824, 1216, 1856, 1220, 60.362},
    {12, 1, 3648, 3648, 3744, 3664, 21.401},
    {12, 1, 4864, 3648, 4960, 3664, 21.401},
    {12, 1, 5016, 3648, 5112, 3664, 21.401},
    {12, 1, 5472, 3080, 5568, 3096, 21.401},
    {12, 1, 5472, 2956, 5568, 2972, 21.401},
    {12, 1, 5472, 2880, 5568, 2896, 21.401},
    {12, 1, 5472, 2736, 5568, 2752, 21.401},
    {12, 1, 5472, 2488, 5568, 2504, 21.401},
    {12, 1, 5472, 2464, 5568, 2480, 21.401},
    {12, 1, 5472, 2328, 5568, 2344, 21.401},
    {12, 1, 5472, 2288, 5568, 2304, 21.401},
    {12, 1, 5472, 2188, 5568, 2204, 21.401},
    {12, 1, 5472, 2144, 5568, 2160, 21.401},
    {12, 2, 1824, 1824, 1872, 1828, 51.796},
    {12, 2, 2432, 1824, 2480, 1828, 51.796},
    {12, 2, 2508, 1824, 2556, 1828, 51.796},
    {12, 2, 2736, 1540, 2784, 1544, 60.271},
    {12, 2, 2736, 1478, 2784, 1482, 60.271},
    {12, 2, 2736, 1440, 2784, 1444, 60.271},
    {12, 2, 2736, 1368, 2784, 1372, 60.271},
    {12, 2, 2736, 1244, 2784, 1248, 60.271},
    {12, 2, 2736, 1232, 2784, 1236, 60.271},
    {12, 2, 2736, 1164, 2784, 1168, 60.271},
    {12, 2, 2736, 1144, 2784, 1148, 60.271},
    {12, 2, 2736, 1094, 2784, 1098, 60.271},
    {12, 2, 2736, 1072, 2784, 1076, 60.271},
    {12, 3, 1216, 1216, 1248, 1220, 60.362},
    {12, 3, 1620, 1216, 1652, 1220, 60.362},
    {12, 3, 1672, 1216, 1704, 1220, 60.362},
    {12, 3, 1820, 1024, 1852, 1028, 60.362},
    {12, 3, 1824, 984, 1856, 988, 60.362},
    {12, 3, 1824, 960, 1856, 964, 60.362},
    {12, 3, 1824, 912, 1856, 916, 60.362},
    {12, 3, 1824, 828, 1856, 832, 60.362},
    {12, 3, 1824, 820, 1856, 824, 60.362},
    {12, 3, 1824, 776, 1856, 780, 60.362},
    {12, 3, 1824, 764, 1856, 768, 60.362},
    {12, 3, 1820, 728, 1852, 732, 60.362},
    {12, 3, 1824, 716, 1856, 720, 60.362},
    {10, 1, 5472, 3648, 5568, 3664, 25.48},
    {10, 1, 5472, 3076, 5568, 3092, 30.173},
    {10, 1, 3840, 2160, 3936, 2176, 60.16},
    {10, 1, 3648, 3648, 3744, 3664, 25.48},
    {10, 1, 4864, 3648, 4960, 3664, 25.48},
    {10, 1, 5016, 3648, 5112, 3664, 25.48},
    {10, 1, 5472, 2956, 5568, 2972, 30.173},
    {10, 1, 5472, 2880, 5568, 2896, 30.173},
    {10, 1, 5472, 2736, 5568, 2752, 30.173},
    {10, 1, 5472, 2488, 5568, 2504, 30.173},
    {10, 1, 5472, 2464, 5568, 2480, 30.173},
    {10, 1, 5472, 2328, 5568, 2344, 30.173},
    {10, 1, 5472, 2288, 5568, 2304, 30.173},
    {10, 1, 5472, 2188, 5568, 2204, 30.173},
    {10, 1, 5472, 2144, 5568, 2160, 30.173},
};

int main()
{
    constexpr size_t n = sizeof(kExpected) / sizeof(kExpected[0]);
    check(kModes.size() == 57 && n == 57, "57 modes");
    size_t twelve = 0, ten = 0;
    for (const Mode &mode : kModes)
        (mode.bits == 12 ? twelve : ten) += mode.bits == 12 || mode.bits == 10 ? 1 : 0;
    check(twelve == 42 && ten == 15, "42 modes at 12-bit, 15 at 10-bit");

    bool rowsMatch = true, transportsMatch = true, fpsMatch = true;
    for (size_t i = 0; i < n; ++i) {
        const Mode &mode = kModes[i];
        const Expected &want = kExpected[i];
        rowsMatch = rowsMatch && mode.bits == want.bits && mode.binning == want.binning &&
                    mode.width == want.aw && mode.height == want.ah;
        const Size transport = transportOf(mode);
        transportsMatch = transportsMatch && transport.width == want.tw && transport.height == want.th;
        fpsMatch = fpsMatch && std::fabs(mode.floorFps - want.fps) < 1e-9;
    }
    check(rowsMatch, "rows (bits, binning, active) equal the table, in order");
    check(transportsMatch, "transport rule: +(96,16) at 1x1, +(48,4) at 2x2, +(32,4) at 3x3, all 57 rows");
    check(fpsMatch, "fps floors equal the table");

    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> active, transport;
    for (const Mode &mode : kModes) {
        active.insert({mode.bits, mode.width, mode.height});
        transport.insert({mode.bits, transportOf(mode).width, transportOf(mode).height});
    }
    check(active.size() == 57 && transport.size() == 57, "active and transport sizes are unique per bit depth");

    bool lookups = true;
    for (const Mode &mode : kModes) {
        const Size t = transportOf(mode);
        lookups = lookups && find(mode.bits, mode.width, mode.height) == &mode &&
                  findTransport(mode.bits, t.width, t.height) == &mode &&
                  activeOfTransport(mode.bits, t.width, t.height).width == mode.width &&
                  activeOfTransport(mode.bits, t.width, t.height).height == mode.height &&
                  isActiveSize(mode.width, mode.height) &&
                  floorFps(mode.bits, mode.width, mode.height) == mode.floorFps;
    }
    check(lookups, "find / findTransport / activeOfTransport / floorFps round-trip every row");
    check(find(10, 2736, 1540) == nullptr && find(12, 3840, 2160) == nullptr, "2736x1540 is 12-bit only, 3840x2160 10-bit only");
    check(findTransport(12, 5472, 3648) == nullptr, "an active size is not a transport size");

    // Aliases and defaults.
    auto is = [](Size s, uint32_t w, uint32_t h) { return s.width == w && s.height == h; };
    check(is(normalise({3840, 2160}, 12), 2736, 1540), "12-bit: 3840x2160 -> 2736x1540");
    check(is(normalise({1920, 1080}, 12), 2736, 1540), "12-bit: 1920x1080 -> 2736x1540");
    check(is(normalise({1920, 1080}, 10), 3840, 2160), "10-bit: 1920x1080 -> 3840x2160");
    check(is(normalise({3840, 2160}, 10), 3840, 2160), "10-bit: 3840x2160 stays");
    check(is(normalise({2736, 1540}, 10), 3840, 2160), "10-bit: 2736x1540 (12-bit only) -> 3840x2160, the nearest");
    check(is(normalise({5472, 3648}, 10), 5472, 3648) && is(normalise({5472, 3648}, 12), 5472, 3648),
          "5472x3648 stays at both depths");

    // Every result is a mode of the depth; the normaliser is idempotent; every table size keeps itself.
    bool inTable = true, idempotent = true, keeps = true;
    const Size probes[] = {{0, 0}, {1, 1}, {1920, 1080}, {1928, 1090}, {3840, 2160}, {3856, 2180}, {2028, 1080},
                           {1332, 990}, {4056, 3040}, {4144, 2176}, {5568, 3664}, {9000, 9000}, {2736, 1540},
                           {3840, 2072}, {1920, 1036}};
    for (const uint32_t bits : {10u, 12u, 0u, 16u}) {
        for (const Size &probe : probes) {
            const Size once = normalise(probe, bits);
            const Size twice = normalise(once, bits);
            inTable = inTable && find(bits == 10 ? 10 : 12, once.width, once.height) != nullptr;
            idempotent = idempotent && once.width == twice.width && once.height == twice.height;
        }
    }
    for (const Mode &mode : kModes) {
        const Size same = normalise({mode.width, mode.height}, mode.bits);
        keeps = keeps && same.width == mode.width && same.height == mode.height;
    }
    check(inTable, "normalise always returns a mode of the depth (other depths count as 12)");
    check(idempotent, "normalise is idempotent");
    check(keeps, "every table size is its own normal form at its depth");

    // ScalerCrop from the runtime maximum. Reference: 5568x3664 delivered, active 5472x3648, bounds as libcamera
    // would report them (origin and size are the driver's, not hard-coded).
    {
        const NativeScalerCrop max{0, 0, 5592, 3694};
        const auto crop = activeAreaScalerCrop(5568, 3664, 5472, 3648, max);
        check(crop && crop->x == 96 * 5592 / 5568 + 1 && crop->y == 0, "1x1 5472x3648: x = ceil(96 * 5592 / 5568) = 97, y = 0");
        // 96*5592/5568 = 96.4 -> 97; 5472*5592/5568 = 5495.6 -> 5496, held to the 5495 left of the maximum;
        // 3648*3694/3664 = 3677.9 -> 3678
        check(crop && crop->width == 5495 && crop->height == 3678,
              "1x1 5472x3648: width ceil(5495.6) held inside the maximum (5495), height ceil(3677.9) = 3678");
        check(crop && crop->x + int(crop->width) <= int(max.width) && crop->height <= max.height, "the crop stays inside the maximum");
    }
    {
        // An offset (even negative) origin is kept: the crop is relative to the runtime max.
        const NativeScalerCrop max{-12, 8, 5600, 3700};
        const auto crop = activeAreaScalerCrop(5568, 3664, 5472, 3648, max);
        check(crop && crop->x == -12 + 97 && crop->y == 8, "origin offset: x = max.x + ceil(...), y = max.y");
    }
    {
        // Exact division: no rounding up.
        const NativeScalerCrop max{0, 0, 2784, 1828};
        const auto crop = activeAreaScalerCrop(2784, 1828, 2736, 1824, max);
        check(crop && crop->x == 48 && crop->y == 0 && crop->width == 2736 && crop->height == 1824,
              "2x2 with a max equal to the delivered size: exact (48, 0, 2736, 1824)");
    }
    {
        const NativeScalerCrop max{0, 0, 1856, 1220};
        const auto crop = activeAreaScalerCrop(1856, 1220, 1824, 1216, max);
        check(crop && crop->x == 32 && crop->width == 1824 && crop->height == 1216, "3x3 exact (32, 0, 1824, 1216)");
    }
    {
        // Rounding up never leaves the maximum: the width is held to what is left of it.
        const NativeScalerCrop max{0, 0, 5593, 3665};
        const auto crop = activeAreaScalerCrop(5568, 3664, 5472, 3648, max);
        check(crop && int(crop->x) + int(crop->width) <= 5593 && crop->height <= 3665, "never past the maximum");
    }
    // Centring the target aspect inside the active crop (what the controller does next).
    {
        const NativeScalerCrop max{0, 0, 2784, 1828};
        const auto crop = activeAreaScalerCrop(2784, 1828, 2736, 1824, max);
        const auto same = crop ? centeredPreviewScalerCrop(2736, 1824, 2736, 1824, *crop) : std::nullopt;
        check(same && same->x == 48 && same->y == 0 && same->width == 2736 && same->height == 1824,
              "target aspect equal to the active aspect: the active crop is kept");
        const auto wide = crop ? centeredPreviewScalerCrop(2736, 1824, 1920, 1080, *crop) : std::nullopt;
        check(wide && wide->width == 2736 && wide->height == 1538 && wide->x == 48 && wide->y == 142,
              "16:9 target in a 3:2 active crop: centred vertically inside it");
    }
    check(!activeAreaScalerCrop(5568, 3664, 5600, 3648, {0, 0, 5592, 3694}), "active wider than delivered is refused");
    check(!activeAreaScalerCrop(0, 3664, 0, 3648, {0, 0, 5592, 3694}), "zero size is refused");
    check(!activeAreaScalerCrop(5568, 3664, 5472, 3648, {0, 0, 0, 0}), "no maximum is refused");

    // The pure part of the existing centred helpers is unchanged by the new function.
    {
        const auto before = centeredRawWindowScalerCrop(4144, 2176, 3872, 2176, {0, 0, 4168, 2192});
        check(before && before->x == 136 && before->y == 0, "centeredRawWindowScalerCrop unchanged (IMX294 case)");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
