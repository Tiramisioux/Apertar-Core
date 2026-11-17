// Host test for src/camera/Imx585Modes.hpp: the mode counts per class, the transport/active rules,
// the width policy and the lookups. Build: g++ -std=c++20 -I src tools/imx585_modes_test.cpp

#include "camera/Imx585Modes.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

namespace im = apertar::imx585;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    CHECK(im::all(im::Class::S12).size() == 64);
    CHECK(im::all(im::Class::S10).size() == 64);
    CHECK(im::all(im::Class::H12).size() == 29);   // the driver's binned 1920x1080 row is left out
    CHECK(im::all(im::Class::H16).size() == 34);

    CHECK(im::classFor(12, false) == im::Class::S12);
    CHECK(im::classFor(10, false) == im::Class::S10);
    CHECK(im::classFor(12, true) == im::Class::H12);
    CHECK(im::classFor(16, true) == im::Class::H16);

    // No duplicate (size, binned) row in a class.
    for (const im::Class cls : { im::Class::S12, im::Class::S10, im::Class::H12, im::Class::H16 }) {
        std::set<std::tuple<uint32_t, uint32_t, bool>> seen;
        for (const im::Mode &mode : im::all(cls))
            CHECK(seen.insert({ mode.width, mode.height, mode.binned }).second);
    }

    // 16-bit transport: +40 rows at 1x1, +20 binned.
    CHECK(im::transportFor(im::Class::H16, 3840, 2072, 0) == std::optional<im::Size>(im::Size{ 3840, 2112 }));
    CHECK(im::transportFor(im::Class::H16, 3840, 2160, 0) == std::optional<im::Size>(im::Size{ 3840, 2200 }));
    CHECK(im::transportFor(im::Class::H16, 1920, 1080, 0) == std::optional<im::Size>(im::Size{ 1920, 1100 }));  // the binned row
    CHECK(im::transportFor(im::Class::S12, 3840, 2072, 0) == std::optional<im::Size>(im::Size{ 3840, 2072 }));
    CHECK(!im::transportFor(im::Class::H16, 1234, 567, 0));

    CHECK(im::activeFor({ 3840, 2072 }, 16) == std::optional<im::Size>(im::Size{ 3840, 2032 }));
    CHECK(im::activeFor({ 3840, 2072 }, 12) == std::optional<im::Size>(im::Size{ 3840, 2072 }));
    CHECK(im::activeFor({ 3840, 2200 }, 16) == std::optional<im::Size>(im::Size{ 3840, 2160 }));
    CHECK(im::activeFor({ 1920, 1100 }, 16) == std::optional<im::Size>(im::Size{ 1920, 1080 }));
    CHECK(!im::activeFor({ 1920, 1120 }, 16));      // the 1x1 1920x1080 window is not offered

    // Every H16 row round-trips through transport and back, except the shadowed 1x1 row.
    for (const im::Mode &mode : im::offered(im::Class::H16, 0)) {
        const im::Size transport = im::transportFor(im::Class::H16, mode);
        const auto active = im::activeFor(transport, 16);
        CHECK(active && *active == (im::Size{ mode.width, mode.height }));
    }
    CHECK(im::offered(im::Class::H16, 0).size() == 33);

    // (1920, 1080) means the binned row in the classes that list both.
    const im::Mode *hdFull = im::find(im::Class::H16, 1920, 1080);
    CHECK(hdFull && hdFull->binned);

    // The width policy: 1280 by default, 0 shows all.
    CHECK(im::kDefaultMinModeWidth == 1280);
    CHECK(im::find(im::Class::S12, 960, 540, 1280) == nullptr);
    CHECK(im::find(im::Class::S12, 960, 540, 0) != nullptr);
    CHECK(im::find(im::Class::S12, 1488, 1080, 1280) != nullptr);
    CHECK(im::find(im::Class::S12, 1080, 1080, 1280) == nullptr);
    CHECK(im::offered(im::Class::S12, 1280).size() == 64 - 11 - 10 - 1);   // 960 x11, 1024 x10, 1080x1080b
    for (const im::Mode &mode : im::offered(im::Class::S10))
        CHECK(mode.width >= im::kDefaultMinModeWidth);

    // Class membership: 1.85:1 binned only in SDR; 4K in all four.
    CHECK(im::find(im::Class::S12, 1920, 1036));
    CHECK(!im::find(im::Class::H12, 1920, 1036));
    CHECK(!im::find(im::Class::H16, 1920, 1036));
    for (const im::Class cls : { im::Class::S12, im::Class::S10, im::Class::H12, im::Class::H16 })
        CHECK(im::find(cls, 3840, 2160));
    CHECK(im::anyClassHas(1920, 1016));

    // 1.85:1 of both drivers: the cinemate-port driver's 3840x2072 / 1920x1036b and the original
    // driver's 3840x2076 / 1920x1038b, in SDR 12-bit and 10-bit; 3840 also in 12-bit Clear HDR.
    for (const im::Class cls : { im::Class::S12, im::Class::S10 }) {
        for (const uint32_t height : { 2072u, 2076u }) {
            const im::Mode *uhd = im::find(cls, 3840, height);
            CHECK(uhd && !uhd->binned);
        }
        for (const uint32_t height : { 1036u, 1038u }) {
            const im::Mode *hd = im::find(cls, 1920, height);
            CHECK(hd && hd->binned);
        }
    }
    const im::Mode *hdr185 = im::find(im::Class::H12, 3840, 2076);
    CHECK(hdr185 && !hdr185->binned);
    CHECK(!im::find(im::Class::H12, 1920, 1038, 0));
    CHECK(!im::find(im::Class::H16, 3840, 2076, 0));   // RAW16 1.85:1 stays 3840x2072 (transport 3840x2112)
    CHECK(im::transportFor(im::Class::S12, 3840, 2076, 0) == std::optional<im::Size>(im::Size{ 3840, 2076 }));
    CHECK(im::transportFor(im::Class::S10, 1920, 1038, 0) == std::optional<im::Size>(im::Size{ 1920, 1038 }));

    // driverCropSize: a 1.85:1 size follows the height the probe found in the class.
    const std::vector<im::Size> portS12 = { { 1920, 1080 }, { 3840, 2160 }, { 3840, 2072 }, { 1920, 1036 } };
    const std::vector<im::Size> originalS12 = { { 1920, 1080 }, { 3840, 2160 }, { 3840, 2076 }, { 1920, 1038 } };
    const std::vector<im::Size> portH16 = { { 3840, 2200 }, { 3840, 2112 }, { 1920, 1100 } };
    const std::vector<im::Size> none;
    for (const uint32_t height : { 2072u, 2076u }) {
        CHECK(im::driverCropSize(im::Class::S12, { 3840, height }, portS12) == (im::Size{ 3840, 2072 }));
        CHECK(im::driverCropSize(im::Class::S12, { 3840, height }, originalS12) == (im::Size{ 3840, 2076 }));
        CHECK(im::driverCropSize(im::Class::S12, { 3840, height }, none) == (im::Size{ 3840, height }));
        CHECK(im::driverCropSize(im::Class::H16, { 3840, height }, portH16) == (im::Size{ 3840, 2072 }));
        CHECK(im::driverCropSize(im::Class::H16, { 3840, height }, none) == (im::Size{ 3840, 2072 }));
    }
    for (const uint32_t height : { 1036u, 1038u }) {
        CHECK(im::driverCropSize(im::Class::S12, { 1920, height }, portS12) == (im::Size{ 1920, 1036 }));
        CHECK(im::driverCropSize(im::Class::S12, { 1920, height }, originalS12) == (im::Size{ 1920, 1038 }));
        CHECK(im::driverCropSize(im::Class::S10, { 1920, height }, none) == (im::Size{ 1920, height }));
        CHECK(im::driverCropSize(im::Class::H12, { 1920, height }, none) == (im::Size{ 1920, height }));  // no binned 1.85:1 there
    }
    CHECK(im::driverCropSize(im::Class::H12, { 3840, 2076 }, { { 3840, 2160 }, { 3840, 2072 } }) == (im::Size{ 3840, 2072 }));
    CHECK(im::driverCropSize(im::Class::S12, { 3840, 2160 }, originalS12) == (im::Size{ 3840, 2160 }));
    CHECK(im::driverCropSize(im::Class::S12, { 3840, 2024 }, portS12) == (im::Size{ 3840, 2024 }));

    // nearest: an exact row is its own nearest; a missing size takes the closest row.
    for (const im::Mode &mode : im::offered(im::Class::H12)) {
        const im::Mode *near = im::nearest(im::Class::H12, mode.width, mode.height);
        CHECK(near && near->width == mode.width && near->height == mode.height);
    }
    const im::Mode *near = im::nearest(im::Class::H12, 1920, 1036);
    CHECK(near && near->width == 1920 && near->height == 1040);
    near = im::nearest(im::Class::H16, 4056, 3040);
    CHECK(near && near->width == 3840 && near->height == 2160);
    CHECK(im::nearest(im::Class::S12, 3840, 2160, 100000) == nullptr);

    // Why setClearHdr enters at 3840x2160: the nearest 12-bit Clear HDR row of Full HD is the 1x1
    // 1920x1040 crop, not 4K; the 4K row exists in that class and is 1x1.
    near = im::nearest(im::Class::H12, 1920, 1080);
    CHECK(near && near->width == 1920 && near->height == 1040 && !near->binned);
    // Binned 1920x1080 is not offered at 12-bit Clear HDR (its curve is unverified); 16-bit and SDR keep it.
    CHECK(!im::find(im::Class::H12, 1920, 1080, 0));
    for (const im::Class cls : { im::Class::S12, im::Class::S10, im::Class::H16 }) {
        const im::Mode *hd = im::find(cls, 1920, 1080);
        CHECK(hd && hd->binned);
    }
    const im::Mode *hdr4k = im::find(im::Class::H12, 3840, 2160);
    CHECK(hdr4k && !hdr4k->binned);

    // APERTAR_IMX585_MIN_MODE_WIDTH: a number overrides, a negative or unparsable value keeps the default.
    CHECK(im::minModeWidth() == 1280);
    setenv("APERTAR_IMX585_MIN_MODE_WIDTH", "0", 1);
    CHECK(im::minModeWidth() == 0);
    setenv("APERTAR_IMX585_MIN_MODE_WIDTH", "1900", 1);
    CHECK(im::minModeWidth() == 1900);
    for (const char *bad : { "-1", "-1280", "abc", "12x", "99999999999" }) {
        setenv("APERTAR_IMX585_MIN_MODE_WIDTH", bad, 1);
        CHECK(im::minModeWidth() == 1280);
    }
    unsetenv("APERTAR_IMX585_MIN_MODE_WIDTH");

    if (failures == 0)
        std::puts("imx585_modes_test: ok");
    return failures == 0 ? 0 : 1;
}
