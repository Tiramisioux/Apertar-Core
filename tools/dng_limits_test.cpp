// Host test for the DNG size gate in src/playback/DngTags.hpp: in-camera/WebUI playback must open every frame
// size Core writes (up to the IMX283's 5472 x 3648) and still refuse a hostile size before allocating.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/dng_limits_test.cpp -o /tmp/dlt && /tmp/dlt
#include "playback/DngTags.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace apertar::playback;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void put32(std::vector<uint8_t> &b, uint32_t v) { put16(b, uint16_t(v)); put16(b, uint16_t(v >> 16)); }

// A one-tile lossless-compression DNG: header, 16 bytes of "tile", then IFD0 with the tags readDngInfo needs.
// Only the tags are real; readDngInfo never touches the tile bytes, which is the point of the gate.
static bool readSynthetic(uint32_t width, uint32_t height, uint16_t bits, std::string &error)
{
    std::vector<uint8_t> file = { 'I', 'I', 42, 0 };
    put32(file, 24);                              // IFD0 after the 16-byte tile
    for (int i = 0; i < 16; ++i) file.push_back(0);
    struct Tag { uint16_t tag, type; uint32_t value; };
    const Tag tags[] = {
        { 256, 4, width }, { 257, 4, height }, { 258, 3, bits }, { 259, 3, 7 }, { 262, 3, 32803 },
        { 277, 3, 1 }, { 322, 4, width }, { 323, 4, height }, { 324, 4, 8 }, { 325, 4, 16 },
    };
    put16(file, uint16_t(sizeof(tags) / sizeof(tags[0])));
    for (const Tag &t : tags) {
        put16(file, t.tag);
        put16(file, t.type);
        put32(file, 1);
        put32(file, t.value);
    }
    put32(file, 0);

    char path[] = "/tmp/dng_limits_test_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        error = "mkstemp failed";
        return false;
    }
    ::unlink(path);
    const bool wrote = ::write(fd, file.data(), file.size()) == ssize_t(file.size());
    DngInfo info;
    const bool ok = wrote && readDngInfo(fd, info, error);
    ::close(fd);
    return ok;
}

int main()
{
    // Every size Core writes.
    check(dngSizeSupported(5472, 3648), "IMX283 5472x3648 active area");
    check(dngSizeSupported(3648, 3648), "IMX283 3648x3648 (1:1 crop)");
    check(dngSizeSupported(5472, 3076), "IMX283 5472x3076 (16:9 crop)");
    check(dngSizeSupported(4144, 2176) && dngSizeSupported(4176, 2176), "IMX294 4144x2176 and 4176x2176 (wider than 4096)");
    check(dngSizeSupported(3792, 2824), "IMX294 3792x2824");
    check(dngSizeSupported(4056, 3040), "IMX477 4056x3040");
    check(dngSizeSupported(3840, 2200) && dngSizeSupported(3856, 2180), "IMX585 3840x2200 and 3856x2180");
    check(dngSizeSupported(2, 2), "smallest accepted frame");

    // What a hostile or foreign file can ask for.
    check(!dngSizeSupported(5473, 3648) && !dngSizeSupported(5472, 3649), "one pixel past the cap on either side");
    check(!dngSizeSupported(0, 3648) && !dngSizeSupported(5472, 0) && !dngSizeSupported(1, 100), "zero or one pixel wide");
    check(!dngSizeSupported(16384, 16384), "16384x16384 (the old 512 MB lie)");
    check(!dngSizeSupported(0xFFFFFFFFu, 0xFFFFFFFFu), "32-bit dimensions do not overflow the product");
    check(!dngSizeSupported(0xFFFFFFFFu, 2) && !dngSizeSupported(2, 0xFFFFFFFFu), "huge on one side only");

    // The cost a slot can reach is the product cap: 38 MiB of 16-bit samples, under the 40 MiB assert.
    check(kMaxDngPixels == uint64_t(5472) * 3648, "product cap is the IMX283 frame");
    check(kMaxDngSampleBytes == kMaxDngPixels * 2 && kMaxDngSampleBytes <= (40u << 20), "slot samples stay under 40 MiB");

    // End to end through readDngInfo: the tags of a real-sized frame pass, a too-large one is refused.
    std::string error;
    check(readSynthetic(5472, 3648, 12, error), "readDngInfo accepts a 5472x3648 12-bit frame");
    error.clear();
    check(readSynthetic(5472, 3648, 16, error), "readDngInfo accepts a 5472x3648 16-bit frame");
    error.clear();
    check(readSynthetic(4176, 2176, 12, error), "readDngInfo accepts a 4176x2176 frame (was refused at > 4096)");
    error.clear();
    check(!readSynthetic(6000, 4000, 12, error) && error.find("Unsupported DNG layout") == 0,
          "readDngInfo refuses 6000x4000 with the layout error");
    error.clear();
    check(!readSynthetic(65535, 65535, 16, error) && error.find("Unsupported DNG layout") == 0,
          "readDngInfo refuses 65535x65535 before touching the tiles");

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
