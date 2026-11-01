// Host test for the DNG payload write path in src/cdng/CdngEncoder.cpp: the rows that need no
// repacking (unpacked 16-bit, unpacked 1-byte 8-bit) go to the file straight from the frame; every
// other layout still passes through the scratch buffer. The files must be byte-identical either way.
//
//   tools/dng_payload_write_test.sh      (builds, runs both paths, compares the files)
//
// This binary writes one DNG per case into the directory given as its argument and prints the sizes.
#include "cdng/CdngEncoder.hpp"
#include <apertar/core/Types.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace apertar;

namespace {

struct Case {
    const char *name;
    uint32_t width, height, stride;
    RawBitDepth bitDepth;
    uint8_t bytesPerSample;
    PixelPacking packing;
};

// 3840x2160 16-bit is the Clear HDR frame; stride 7744 exercises the padded-stride (writev) path;
// the 12-bit unpacked and the CSI2-packed cases are controls that keep the scratch path.
const Case kCases[] = {
    { "hdr16_contiguous", 3840, 2160, 7680, RawBitDepth::Bits16, 2, PixelPacking::Unpacked },
    { "hdr16_padded_stride", 3840, 2160, 7744, RawBitDepth::Bits16, 2, PixelPacking::Unpacked },
    { "mono8_contiguous", 1920, 1080, 1920, RawBitDepth::Bits8, 1, PixelPacking::Unpacked },
    { "sdr12_unpacked_pack_path", 1920, 1080, 3840, RawBitDepth::Bits12, 2, PixelPacking::Unpacked },
    { "sdr12_csi2_pack_path", 1920, 1080, 2880, RawBitDepth::Bits12, 0, PixelPacking::Csi2Packed },
    { "hdr16_small_odd_stride", 96, 70, 200, RawBitDepth::Bits16, 2, PixelPacking::Unpacked },
};

void fill(std::vector<uint8_t> &bytes, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 12345u;
    for (uint8_t &b : bytes) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    CdngEncoder encoder;
    int failed = 0;
    unsigned int seed = 1;
    for (const Case &c : kCases) {
        std::vector<uint8_t> bytes(static_cast<size_t>(c.stride) * c.height);
        fill(bytes, seed++);
        RawFrameView frame;
        frame.data = bytes.data();
        frame.size = bytes.size();
        frame.width = c.width;
        frame.height = c.height;
        frame.stride = c.stride;
        frame.bitDepth = c.bitDepth;
        frame.unpackedBytesPerSample = c.bytesPerSample;
        frame.sampleBits = static_cast<unsigned int>(c.bitDepth);
        frame.packing = c.packing;
        frame.bayerPattern = c.bitDepth == RawBitDepth::Bits8 ? BayerPattern::Monochrome : BayerPattern::RGGB;
        CdngEncoder::Metadata metadata;
        metadata.compression = CdngCompression::Uncompressed;
        metadata.clipName = "cDNG_test_C0001";
        metadata.frameFileName = std::string(c.name) + ".dng";
        metadata.reelName = metadata.clipName;
        metadata.wallClockTimestampUs = 1761955200000000ULL;   // fixed: the two runs must produce identical files
        metadata.timecodeWallClockUs = metadata.wallClockTimestampUs;
        metadata.sensorTimestampUs = 1000000;
        const std::string path = dir + "/" + c.name + ".dng";
        try {
            encoder.writeDngFile(path, frame, metadata);
        } catch (const std::exception &e) {
            std::printf("FAIL %s: %s\n", c.name, e.what());
            ++failed;
            continue;
        }
        FILE *f = std::fopen(path.c_str(), "rb");
        long size = -1;
        if (f) {
            std::fseek(f, 0, SEEK_END);
            size = std::ftell(f);
            std::fclose(f);
        }
        const uint64_t rowBytes = c.packing == PixelPacking::Csi2Packed ? static_cast<uint64_t>(c.width) * 12u / 8u
            : c.bitDepth == RawBitDepth::Bits16 ? static_cast<uint64_t>(c.width) * 2u
            : c.bitDepth == RawBitDepth::Bits8 ? c.width
            : static_cast<uint64_t>(c.width) * 12u / 8u;
        const uint64_t payload = rowBytes * c.height;
        const bool ok = size > 0 && static_cast<uint64_t>(size) > payload && static_cast<uint64_t>(size) < payload + 65536;
        std::printf("%s %s size=%ld payload=%llu\n", ok ? "ok  " : "FAIL", c.name, size,
                    static_cast<unsigned long long>(payload));
        if (!ok)
            ++failed;
    }
    return failed == 0 ? 0 : 1;
}
