// Host test for the FORMAT row in src/playback/ClipProbe.hpp: only the Clear HDR CCMP expansion table on a
// 12-bit clip reads "Clear HDR 12-bit companded"; any other LinearizationTable is a Log clip.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/clip_probe_format_test.cpp src/cdng/CdngEncoder.cpp src/cdng/LosslessJpegEncoder.cpp src/color/ColorProfileConfig.cpp -o /tmp/cpft -lyaml -lpthread && /tmp/cpft
#include "cdng/CdngEncoder.hpp"
#include "cdng/ClearHdrCcmp.hpp"
#include "playback/ClipProbe.hpp"
#include <apertar/core/Types.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace apertar;

static int g_failed = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

// One uncompressed one-frame clip: <root>/<clip>/<clip>_000000.dng, 16x16 RGGB, unpacked 2 bytes per sample.
static std::filesystem::path writeClip(const std::filesystem::path &root, const std::string &clip, RawBitDepth bitDepth,
                                       const std::vector<uint16_t> &table)
{
    const std::filesystem::path dir = root / clip;
    std::filesystem::create_directories(dir);
    const uint32_t width = 16, height = 16;
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * 2u * height);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i & 0x3f);
    RawFrameView frame;
    frame.data = bytes.data();
    frame.size = bytes.size();
    frame.width = width;
    frame.height = height;
    frame.stride = width * 2u;
    frame.bitDepth = bitDepth;
    frame.unpackedBytesPerSample = 2;
    frame.sampleBits = static_cast<unsigned int>(bitDepth);
    frame.packing = PixelPacking::Unpacked;
    frame.bayerPattern = BayerPattern::RGGB;
    CdngEncoder::Metadata metadata;
    metadata.compression = CdngCompression::Uncompressed;
    metadata.linearizationTable = std::span<const uint16_t>(table);
    metadata.clipName = clip;
    metadata.frameFileName = clip + "_000000.dng";
    metadata.reelName = clip;
    metadata.wallClockTimestampUs = 1761955200000000ULL;
    metadata.timecodeWallClockUs = metadata.wallClockTimestampUs;
    metadata.sensorTimestampUs = 1000000;
    CdngEncoder encoder;
    encoder.writeDngFile((dir / metadata.frameFileName).string(), frame, metadata);
    return dir;
}

static std::vector<uint16_t> ramp(size_t size)
{
    std::vector<uint16_t> table(size);
    for (size_t i = 0; i < size; ++i)
        table[i] = static_cast<uint16_t>(i * 65535u / (size - 1));
    return table;
}

static void expectFormat(const std::filesystem::path &dir, const std::string &want)
{
    const playback::ClipFacts facts = playback::probeClip(dir);
    check(facts.ok && facts.formatText.rfind(want, 0) == 0,
          dir.filename().string() + ": \"" + facts.formatText + "\" starts with \"" + want + "\"");
}

int main()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "apertar-clip-probe-format-test";
    std::filesystem::remove_all(root);
    try {
        const auto &ccmpTable = ccmp::expansionTable();
        const std::vector<uint16_t> ccmpCopy(ccmpTable.begin(), ccmpTable.end());
        expectFormat(writeClip(root, "H12_C0001", RawBitDepth::Bits12, ccmpCopy), "Clear HDR 12-bit companded");
        expectFormat(writeClip(root, "Log12_C0002", RawBitDepth::Bits12, ramp(4096)), "Log 12-bit");
        expectFormat(writeClip(root, "Log10_C0003", RawBitDepth::Bits10, ramp(1024)), "Log 10-bit");
        expectFormat(writeClip(root, "SDR12_C0004", RawBitDepth::Bits12, {}), "SDR 12-bit");
    } catch (const std::exception &e) {
        check(false, std::string("write a clip: ") + e.what());
    }
    std::filesystem::remove_all(root);
    std::printf("%s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
