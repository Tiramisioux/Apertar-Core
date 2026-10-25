#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace apertar::ccmp {

// IMX585 Clear HDR, 12-bit sensor mode. The sensor gradation-compresses (CCMP) its output to 12-bit
// codes. This image's libcamera (CcmpUnwrap in the PiSP pipeline handler) expands every raw frame back
// to linear 16-bit before Core sees it. To store real 12-bit data, Core maps each value back to its code
// and writes the expansion as a DNG LinearizationTable, so a raw reader gets exactly the linear values
// the 16-bit storage would hold (black 3200, white 65520).
//
// The expansion reproduces libcamera's table entry for entry for these parameters (checked against
// libcamera's own CcmpUnwrap and against every value in a recorded frame):
//   y = code - black                              (y <= 0 gives 0)
//   L = y                                         y <= knee1
//     = ((y - knee1) << slope1) + knee1           y <= C2,  C2 = knee1 + ((knee2 - knee1) >> slope1)
//     = ((y - C2) << slope2) + knee2              above C2
//   out = black * 16 + L * (65520 - black * 16) / L(4095)        (integer division)
// The parameters are the driver defaults. Core never writes the gradient-compression controls; if it
// ever does, the table has to follow the live values.
struct Params {
    uint32_t knee1 = 500;    // CCMP1_EXP
    uint32_t knee2 = 11500;  // CCMP2_EXP
    unsigned slope1 = 6;     // ACMP1_EXP: 1/64
    unsigned slope2 = 4;     // ACMP2_EXP: 1/16
    uint32_t black = 200;    // 12-bit pedestal
};

inline constexpr uint16_t kBlack16 = 200 * 16;

inline uint32_t linearAboveBlack(int y, const Params &p)
{
    if (y <= 0)
        return 0;
    const uint32_t value = static_cast<uint32_t>(y);
    const uint32_t knee2Code = p.knee1 + ((p.knee2 - p.knee1) >> p.slope1);
    if (value <= p.knee1)
        return value;
    if (value <= knee2Code)
        return ((value - p.knee1) << p.slope1) + p.knee1;
    return ((value - knee2Code) << p.slope2) + p.knee2;
}

inline std::array<uint16_t, 4096> buildExpansion(const Params &p = {})
{
    std::array<uint16_t, 4096> table{};
    const uint64_t base = static_cast<uint64_t>(p.black) << 4;
    const uint64_t span = 65520u - base;
    const uint64_t top = linearAboveBlack(static_cast<int>(4095 - p.black), p);
    for (int code = 0; code < 4096; ++code) {
        const uint64_t value = base + linearAboveBlack(code - static_cast<int>(p.black), p) * span / top;
        table[static_cast<size_t>(code)] = static_cast<uint16_t>(value > 65535u ? 65535u : value);
    }
    return table;
}

// 12-bit code -> linear 16-bit value (the DNG LinearizationTable).
inline const std::array<uint16_t, 4096> &expansionTable()
{
    static const std::array<uint16_t, 4096> table = buildExpansion();
    return table;
}

// Linear 16-bit value -> 12-bit code: the lowest code whose expansion reaches the value. Exact for every
// value libcamera produces, so expansionTable()[compressionLut()[v]] == v.
inline const std::vector<uint16_t> &compressionLut()
{
    static const std::vector<uint16_t> lut = [] {
        const auto &table = expansionTable();
        std::vector<uint16_t> out(65536);
        uint16_t code = 0;
        for (uint32_t value = 0; value < 65536; ++value) {
            while (code < 4095 && table[code] < value)
                ++code;
            out[value] = code;
        }
        return out;
    }();
    return lut;
}

} // namespace apertar::ccmp
