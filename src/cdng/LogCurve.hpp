#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apertar::cdnglog {

// CineMate Log: mu-law companding of the signal above black plus a short linear footroom below it, so
// sub-black sensor noise survives. Applied per sample before packing; the inverse table is written as
// the DNG LinearizationTable (0xC618) while BlackLevel/WhiteLevel stay in the linear domain.
//   codes [0, F)     linear footroom over [BL - foot, BL)
//   codes [F, CMAX]  mu-law over [BL, WL],  TOP = CMAX - F
// Same curve as cinepi-raw (cinepi/log_lut.hpp), so files from both cameras share one look.
struct Params {
    double mu = 0.0;
    int sourceBits = 0;
    int targetBits = 0;
    int footroomCodes = 0;  // F
    int footroomLsb = 0;    // foot
    int blackLevel = 0;     // linear, in sourceBits
    int whiteLevel = 0;

    int codeMax() const { return (targetBits >= 1 && targetBits <= 16) ? (1 << targetBits) - 1 : 0; }
    int span() const { return whiteLevel - blackLevel; }
    int top() const { return codeMax() - footroomCodes; }
    bool hasFootroom() const { return footroomCodes > 0 && footroomLsb > 0; }

    bool valid() const
    {
        if (sourceBits < 1 || sourceBits > 16 || targetBits < 1 || targetBits > 16)
            return false;
        if (targetBits >= sourceBits || !(mu > 0.0))  // Log only steps down
            return false;
        if (blackLevel < 0 || whiteLevel <= blackLevel || whiteLevel > (1 << sourceBits) - 1)
            return false;
        if (footroomCodes < 0 || footroomCodes >= codeMax())
            return false;
        return footroomLsb >= 0 && footroomLsb <= blackLevel;
    }

    std::string describe() const
    {
        return "CineMate Log " + std::to_string(sourceBits) + "->" + std::to_string(targetBits) +
               " mu=" + std::to_string(static_cast<long long>(mu)) + " black=" + std::to_string(blackLevel) +
               " white=" + std::to_string(whiteLevel) + " footroom=" + std::to_string(footroomCodes) + "/" +
               std::to_string(footroomLsb);
    }
};

struct PairConstants {
    int sourceBits;
    int targetBits;
    double mu;
    int footroomCodes;
    int footroomLsb;
};

// 16->12 and 12->10 are cinepi-raw's shipped specs. 10->8 has no cinepi spec: derived with its
// generator's rules (mu = span / 6, footroom reaching about 8 sensor sigmas, F about 3% of the codes).
inline constexpr PairConstants kPairs[] = {
    {16, 12, 10000.0, 32, 487},
    {12, 10, 1500.0, 32, 200},
    {10, 8, 160.0, 8, 22},
};

inline std::optional<PairConstants> pairConstants(int sourceBits, int targetBits)
{
    for (const PairConstants &pair : kPairs)
        if (pair.sourceBits == sourceBits && pair.targetBits == targetBits)
            return pair;
    return std::nullopt;
}

// Black and white are not constants: callers pass the camera's own levels (in sourceBits).
inline std::optional<Params> makeParams(int sourceBits, int targetBits, int blackLevel, int whiteLevel)
{
    const auto pair = pairConstants(sourceBits, targetBits);
    if (!pair)
        return std::nullopt;
    Params p;
    p.mu = pair->mu;
    p.sourceBits = sourceBits;
    p.targetBits = targetBits;
    p.footroomCodes = pair->footroomCodes;
    p.footroomLsb = pair->footroomLsb;
    p.blackLevel = blackLevel;
    p.whiteLevel = whiteLevel;
    return p.valid() ? std::optional<Params>(p) : std::nullopt;
}

// Precision rules, all load-bearing (they change the table if "cleaned up"):
//  1. Below black: floor() on encode and the bin CENTRE (C + 0.5) on decode. The bin edge would bias the
//     reconstructed black point by +1.5 LSB.
//  2. nearbyint(), not round(): ties-to-even under the default rounding mode, as the reference tables.
//  3. Keep the division-then-multiply order ("/ log1p(mu) * TOP", "/ foot * F", "/ F * foot");
//     reassociating changes the last bit on some entries.
inline uint16_t encodeCode(const Params &p, uint32_t level)
{
    const double L = static_cast<double>(level);
    const double F = p.footroomCodes;
    double code;
    if (p.hasFootroom() && L < p.blackLevel) {
        const double foot = p.footroomLsb;
        code = std::floor((L - (p.blackLevel - foot)) / foot * F);
        code = std::min(std::max(code, 0.0), F - 1.0);
    } else {
        double x = (L - p.blackLevel) / p.span();
        x = std::min(std::max(x, 0.0), 1.0);
        code = std::nearbyint(std::log1p(p.mu * x) / std::log1p(p.mu) * p.top()) + F;
    }
    return static_cast<uint16_t>(std::min(std::max(code, 0.0), static_cast<double>(p.codeMax())));
}

inline uint32_t decodeLevel(const Params &p, uint32_t code)
{
    const double C = static_cast<double>(code);
    const double F = p.footroomCodes;
    double L;
    if (p.hasFootroom() && code < static_cast<uint32_t>(p.footroomCodes)) {
        const double foot = p.footroomLsb;
        L = (p.blackLevel - foot) + (C + 0.5) / F * foot;
    } else {
        const double up = (std::pow(1.0 + p.mu, std::max(0.0, C - F) / p.top()) - 1.0) / p.mu;
        L = p.blackLevel + up * p.span();
    }
    L = std::nearbyint(L);
    return static_cast<uint32_t>(std::min(std::max(L, 0.0), static_cast<double>(p.whiteLevel)));
}

struct Tables {
    std::vector<uint16_t> forward;  // linear level -> code
    std::vector<uint16_t> inverse;  // code -> linear level: the DNG LinearizationTable
};

inline std::vector<uint16_t> buildInverse(const Params &p)
{
    std::vector<uint16_t> inverse(static_cast<size_t>(1) << p.targetBits);
    for (size_t code = 0; code < inverse.size(); ++code)
        inverse[code] = static_cast<uint16_t>(decodeLevel(p, static_cast<uint32_t>(code)));
    return inverse;
}

// Empty tables when the params are invalid. Built from scratch on every call; callers cache.
inline Tables buildTables(const Params &p)
{
    Tables tables;
    if (!p.valid())
        return tables;
    tables.forward.resize(static_cast<size_t>(1) << p.sourceBits);
    for (size_t level = 0; level < tables.forward.size(); ++level)
        tables.forward[level] = encodeCode(p, static_cast<uint32_t>(level));
    tables.inverse = buildInverse(p);
    return tables;
}

// 12-bit Clear HDR frames hold CCMP codes, not linear values. Encoding them directly would compand twice
// while the LinearizationTable claims linear input. So the forward table is indexed by the 4096 companded
// codes and decompands first (ccmp::expansionTable(), already in 16-bit linear with black), then applies
// the 16-bit curve p16. The inverse table is exactly buildTables(p16).inverse: after decompanding,
// the 16-bit domain is what the data is in.
inline Tables buildComposed(const Params &p16, std::span<const uint16_t> expansion4096)
{
    Tables tables;
    if (!p16.valid() || p16.sourceBits != 16 || expansion4096.size() != 4096)
        return tables;
    tables.forward.resize(expansion4096.size());
    for (size_t code = 0; code < expansion4096.size(); ++code)
        tables.forward[code] = encodeCode(p16, expansion4096[code]);
    tables.inverse = buildInverse(p16);
    return tables;
}

// In-place pass over a frame copy: samples[i] = table[samples[i]]. The table must cover every sample
// value; containerTable() builds one that does.
inline void applyTable(uint16_t *samples, size_t count, const uint16_t *table)
{
    for (size_t i = 0; i < count; ++i)
        samples[i] = table[samples[i]];
}

// The table the copy worker applies: always 65536 entries, so no sample read from a 16-bit container can
// index past it (values above the source range clamp to the top code). `shift` moves the code up inside the
// container (8 for an 8-bit target: the unpacked 8-bit path reads the high byte). `viaIndex`, when given
// (65536 entries), is looked up first: Clear HDR 12-bit passes ccmp::compressionLut() so one pass takes the
// linear sample to its CCMP code and on to the composed Log code.
inline std::vector<uint16_t> containerTable(std::span<const uint16_t> forward, unsigned shift,
                                            std::span<const uint16_t> viaIndex = {})
{
    if (forward.empty() || shift > 8 || (!viaIndex.empty() && viaIndex.size() != 65536))
        return {};
    std::vector<uint16_t> out(65536);
    const size_t last = forward.size() - 1;
    for (size_t value = 0; value < out.size(); ++value) {
        const size_t index = viaIndex.empty() ? value : viaIndex[value];
        out[value] = static_cast<uint16_t>(forward[std::min(index, last)] << shift);
    }
    return out;
}

} // namespace apertar::cdnglog
