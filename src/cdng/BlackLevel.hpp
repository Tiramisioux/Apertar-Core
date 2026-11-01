#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <apertar/core/Types.hpp>

namespace apertar {

inline float blackLevelForSampleBits(float black, unsigned bits, bool onContainerScale = false)
{
    if (!std::isfinite(black) || bits == 0 || bits > 16)
        return 0.0f;
    const float white = static_cast<float>((1u << bits) - 1u);
    const float scaled = onContainerScale ? black : black * white / 4095.0f;
    return std::clamp(scaled, 0.0f, white - 1.0f);
}

// The white level a raw container declares for samples stored at `bitDepth` (full scale of that depth).
inline uint32_t encodedWhiteLevel(RawBitDepth bitDepth)
{
    const unsigned int bits = static_cast<unsigned int>(bitDepth);
    return bits >= 16 ? 65535u : ((1u << bits) - 1u);
}

// The black level a raw container declares for samples stored at `bitDepth`. cDNG (per CFA tile entry) and
// MLV (one RAWI value) both call this, so the two formats of one take declare the same black. The metadata
// values are on a 12-bit reference scale unless the encoder is told they already are on the container scale.
inline int32_t encodedBlackLevel(float sourceBlack, RawBitDepth bitDepth, bool onContainerScale = false)
{
    return static_cast<int32_t>(std::lround(blackLevelForSampleBits(
        sourceBlack, static_cast<unsigned>(bitDepth), onContainerScale)));
}

// The pedestal both formats assume when the frame carries no black level (12-bit reference scale).
inline constexpr int32_t kDefaultSourceBlackLevel = 200;

// Metadata is R, Gr, Gb, B; a DNG's 2x2 BlackLevel array follows the CFA tile.
inline std::array<unsigned, 4> blackLevelCfaIndices(BayerPattern pattern)
{
    switch (pattern) {
    case BayerPattern::RGGB: return {0, 1, 2, 3};
    case BayerPattern::GRBG: return {1, 0, 3, 2};
    case BayerPattern::GBRG: return {2, 3, 0, 1};
    case BayerPattern::BGGR: return {3, 2, 1, 0};
    case BayerPattern::Monochrome: return {0, 0, 0, 0};
    }
    return {0, 1, 2, 3};
}

} // namespace apertar
