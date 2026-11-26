// Host test: the lens tags CdngEncoder writes (Exif FNumber 0x829D, FocalLength 0x920A, SubjectDistance 0x9206, LensModel 0xA434; IFD0
// DNG LensInfo 0xC630), only for known values, kept right when a value changes between frames of one take, and the cost of that.
//   g++ -std=c++20 -O2 -Wall -I src -I include tools/cdng_lens_test.cpp src/cdng/CdngEncoder.cpp src/cdng/LosslessJpegEncoder.cpp src/color/ColorProfileConfig.cpp -o /tmp/cdng_lens_test -lyaml -lpthread && /tmp/cdng_lens_test /dev/shm
#include "cdng/CdngEncoder.hpp"
#include <apertar/core/Types.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace apertar;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

namespace {

struct Tag {
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t valueOrOffset = 0;
    std::vector<uint8_t> bytes;
};

uint32_t le32(const std::vector<uint8_t> &f, size_t at) { return f[at] | (f[at + 1] << 8) | (f[at + 2] << 16) | (uint32_t(f[at + 3]) << 24); }
uint16_t le16(const std::vector<uint8_t> &f, size_t at) { return uint16_t(f[at] | (f[at + 1] << 8)); }

std::vector<uint8_t> slurp(const std::string &path)
{
    std::vector<uint8_t> bytes;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        uint8_t chunk[65536];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
        std::fclose(f);
    }
    return bytes;
}

size_t typeBytes(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: return 4;
    default: return 8;
    }
}

std::optional<Tag> findTag(const std::vector<uint8_t> &f, size_t ifd, uint16_t tag)
{
    const uint16_t count = le16(f, ifd);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t e = ifd + 2 + size_t(i) * 12;
        if (le16(f, e) != tag) continue;
        Tag t;
        t.type = le16(f, e + 2);
        t.count = le32(f, e + 4);
        t.valueOrOffset = le32(f, e + 8);
        const size_t length = typeBytes(t.type) * t.count;
        const size_t from = length <= 4 ? e + 8 : t.valueOrOffset;
        t.bytes.assign(f.begin() + from, f.begin() + from + length);
        return t;
    }
    return std::nullopt;
}

struct Parsed {
    std::vector<uint8_t> file;
    size_t ifd0 = 0, exif = 0;
    std::optional<Tag> ifd0Tag(uint16_t tag) const { return findTag(file, ifd0, tag); }
    std::optional<Tag> exifTag(uint16_t tag) const { return findTag(file, exif, tag); }
    bool rational(const std::optional<Tag> &t, uint32_t n, uint32_t d, size_t index = 0) const
    {
        if (!t || t->type != 5 || t->bytes.size() < (index + 1) * 8) return false;
        const auto word = [&](size_t at) { return uint32_t(t->bytes[at] | (t->bytes[at + 1] << 8) | (t->bytes[at + 2] << 16) | (uint32_t(t->bytes[at + 3]) << 24)); };
        return word(index * 8) == n && word(index * 8 + 4) == d;
    }
};

Parsed parse(const std::string &path)
{
    Parsed p;
    p.file = slurp(path);
    p.ifd0 = le32(p.file, 4);
    const auto exifPointer = findTag(p.file, p.ifd0, 0x8769);
    p.exif = exifPointer ? exifPointer->valueOrOffset : 0;
    return p;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    const std::string path = dir + "/cdng_lens_test.dng";
    constexpr uint32_t width = 96, height = 70, stride = 200;
    std::vector<uint8_t> bytes(size_t(stride) * height, 0x5a);
    RawFrameView frame;
    frame.data = bytes.data();
    frame.size = bytes.size();
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.bitDepth = RawBitDepth::Bits16;
    frame.unpackedBytesPerSample = 2;
    frame.sampleBits = 16;
    frame.packing = PixelPacking::Unpacked;
    frame.bayerPattern = BayerPattern::RGGB;

    CdngEncoder encoder;
    CdngEncoder::Metadata metadata;
    metadata.compression = CdngCompression::Uncompressed;
    metadata.clipName = "lens_test_C0001";
    metadata.frameFileName = "lens_test.dng";
    metadata.wallClockTimestampUs = 1761955200000000ULL;
    metadata.timecodeWallClockUs = metadata.wallClockTimestampUs;

    // no lens: none of the five tags
    encoder.writeDngFile(path, frame, metadata);
    Parsed none = parse(path);
    CHECK(none.exif != 0 && !none.exifTag(0x829D) && !none.exifTag(0x920A) && !none.exifTag(0x9206) && !none.exifTag(0xA434) && !none.ifd0Tag(0xC630));
    CHECK(none.exifTag(0x829A));   // the exposure time is still there

    // a full lens
    metadata.lens.fNumberX100 = 560;
    metadata.lens.focalLengthX10 = 140;
    metadata.lens.flags = LensMetadata::kFlagHasDistance;
    metadata.lens.distanceCm = 35;
    metadata.lens.focalMinX10 = 100;
    metadata.lens.focalMaxX10 = 220;
    metadata.lens.apertureMinAtMinX100 = 350;
    metadata.lens.apertureMinAtMaxX100 = 450;
    metadata.lens.lensId = 0xEB;
    metadata.lens.setModel("Canon EF-S 10-22mm");
    encoder.writeDngFile(path, frame, metadata);
    Parsed full = parse(path);
    CHECK(full.rational(full.exifTag(0x829D), 560, 100) && full.rational(full.exifTag(0x920A), 140, 10) && full.rational(full.exifTag(0x9206), 35, 100));
    const auto model = full.exifTag(0xA434);
    CHECK(model && model->type == 2 && std::string(model->bytes.begin(), model->bytes.end() - 1) == "Canon EF-S 10-22mm" && model->bytes.back() == 0);
    const auto info = full.ifd0Tag(0xC630);
    CHECK(info && info->type == 5 && info->count == 4);
    CHECK(full.rational(info, 100, 10, 0) && full.rational(info, 220, 10, 1) && full.rational(info, 350, 100, 2) && full.rational(info, 450, 100, 3));

    // the same take, values moved (zoom, iris, focus): the tags follow, the layout is the same
    metadata.lens.fNumberX100 = 800;
    metadata.lens.focalLengthX10 = 220;
    metadata.lens.distanceCm = 120;
    metadata.lens.apertureMinAtMaxX100 = 0;   // unknown: 0/0
    metadata.frameIndex = 1;
    encoder.writeDngFile(path, frame, metadata);
    Parsed moved = parse(path);
    CHECK(moved.rational(moved.exifTag(0x829D), 800, 100) && moved.rational(moved.exifTag(0x920A), 220, 10) && moved.rational(moved.exifTag(0x9206), 120, 100));
    CHECK(moved.rational(moved.ifd0Tag(0xC630), 0, 0, 3));
    CHECK(moved.file.size() == full.file.size());

    // infinity
    metadata.lens.flags |= LensMetadata::kFlagDistanceInfinite;
    metadata.lens.distanceCm = 65535;
    encoder.writeDngFile(path, frame, metadata);
    Parsed infinite = parse(path);
    CHECK(infinite.rational(infinite.exifTag(0x9206), 0xFFFFFFFFu, 1));

    // a value that becomes unknown loses its tag; a different lens changes the model
    metadata.lens.flags = 0;
    metadata.lens.distanceCm = 0;
    metadata.lens.focalLengthX10 = 0;
    metadata.lens.setModel("Sigma 18-35");
    encoder.writeDngFile(path, frame, metadata);
    Parsed later = parse(path);
    CHECK(!later.exifTag(0x9206) && !later.exifTag(0x920A) && later.exifTag(0x829D) && later.exifTag(0xA434));
    const auto laterModel = later.exifTag(0xA434);
    CHECK(laterModel && std::string(laterModel->bytes.begin(), laterModel->bytes.end() - 1) == "Sigma 18-35");
    metadata.lens = LensMetadata{};   // lens removed
    encoder.writeDngFile(path, frame, metadata);
    Parsed gone = parse(path);
    CHECK(!gone.exifTag(0x829D) && !gone.exifTag(0xA434) && !gone.ifd0Tag(0xC630));

    // the cost per frame (small frame, tmpfs): no lens / lens constant / lens values changing every frame / the set of known tags
    // changing every frame (the worst case: the template is rebuilt each time)
    const auto bench = [&](const char *name, auto prepare) {
        CdngEncoder e;
        CdngEncoder::Metadata m = metadata;
        m.lens = LensMetadata{};
        constexpr int rounds = 3000;
        for (int i = 0; i < 50; ++i) { prepare(m, i); m.frameIndex = i; e.writeDngFile(path, frame, m); }
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i) { prepare(m, i); m.frameIndex = i; e.writeDngFile(path, frame, m); }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / rounds;
        std::printf("%-34s %8.1f us/frame\n", name, us);
        return us;
    };
    const auto fullLens = [](CdngEncoder::Metadata &m) {
        m.lens.fNumberX100 = 560; m.lens.focalLengthX10 = 140; m.lens.flags = LensMetadata::kFlagHasDistance; m.lens.distanceCm = 35;
        m.lens.focalMinX10 = 100; m.lens.focalMaxX10 = 220; m.lens.apertureMinAtMinX100 = 350; m.lens.setModel("Canon EF-S 10-22mm");
    };
    bench("no lens", [](CdngEncoder::Metadata &, int) {});
    bench("lens, constant", [&](CdngEncoder::Metadata &m, int) { fullLens(m); });
    bench("lens, f-number+focal change/frame", [&](CdngEncoder::Metadata &m, int i) { fullLens(m); m.lens.fNumberX100 = 400 + (i % 40) * 10; m.lens.focalLengthX10 = 100 + (i % 120); });
    bench("lens, known-tag set flips/frame", [&](CdngEncoder::Metadata &m, int i) { fullLens(m); if (i & 1) { m.lens.focalLengthX10 = 0; m.lens.flags = 0; } });

    std::remove(path.c_str());
    if (failures == 0) std::puts("cdng_lens_test: ok");
    return failures == 0 ? 0 : 1;
}
