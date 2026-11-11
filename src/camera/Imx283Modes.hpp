// The IMX283 sensor modes Core offers: the 57 non-experimental modes of the cinemate-port driver
// (Tiramisioux/imx283-v4l2-driver, branch cinemate-port, commit cinemate-port@68b7dc9, experimental_modes=0). The table matches the
// driver's mode tables at 68b7dc9, the build on the rig since 2026-11-10 (they are the same in
// cinemate-modes cb925e5); recheck it when the driver changes. Pure, std only;
// host-tested by tools/imx283_modes_test.cpp.
//
// Core's resolution is the ACTIVE picture. The driver delivers a larger TRANSPORT frame: the active
// picture sits after the optical-black columns on the LEFT and ends above the optical-black rows at
// the BOTTOM (both zero-filled), so the active picture starts at (0 + hob, 0) in the frame:
//   transport = active + (96, 16) at 1x1, (48, 4) at 2x2, (32, 4) at 3x3.
// floorFps is the driver's frame-rate floor with crop_vmax=0; the sensor probe may report less
// (the ISP pixel-rate clamp), Core uses the lower of the two.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace apertar::imx283 {

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Mode {
    uint32_t bits;      // sensor bit depth: 12 (MODE_0/2/2A/3) or 10 (MODE_1/1A/1C)
    uint32_t binning;   // 1, 2 or 3 (per axis)
    uint32_t width;     // active picture
    uint32_t height;
    double floorFps;
};

inline constexpr std::array<Mode, 57> kModes{{
    // 12-bit
    {12, 1, 5472, 3648, 21.401}, {12, 2, 2736, 1824, 51.796}, {12, 3, 1824, 1216, 60.362},
    {12, 1, 3648, 3648, 21.401}, {12, 1, 4864, 3648, 21.401}, {12, 1, 5016, 3648, 21.401},
    {12, 1, 5472, 3080, 21.401}, {12, 1, 5472, 2956, 21.401}, {12, 1, 5472, 2880, 21.401},
    {12, 1, 5472, 2736, 21.401}, {12, 1, 5472, 2488, 21.401}, {12, 1, 5472, 2464, 21.401},
    {12, 1, 5472, 2328, 21.401}, {12, 1, 5472, 2288, 21.401}, {12, 1, 5472, 2188, 21.401},
    {12, 1, 5472, 2144, 21.401},
    {12, 2, 1824, 1824, 51.796}, {12, 2, 2432, 1824, 51.796}, {12, 2, 2508, 1824, 51.796},
    {12, 2, 2736, 1540, 60.271}, {12, 2, 2736, 1478, 60.271}, {12, 2, 2736, 1440, 60.271},
    {12, 2, 2736, 1368, 60.271}, {12, 2, 2736, 1244, 60.271}, {12, 2, 2736, 1232, 60.271},
    {12, 2, 2736, 1164, 60.271}, {12, 2, 2736, 1144, 60.271}, {12, 2, 2736, 1094, 60.271},
    {12, 2, 2736, 1072, 60.271},
    {12, 3, 1216, 1216, 60.362}, {12, 3, 1620, 1216, 60.362}, {12, 3, 1672, 1216, 60.362},
    {12, 3, 1820, 1024, 60.362}, {12, 3, 1824, 984, 60.362},  {12, 3, 1824, 960, 60.362},
    {12, 3, 1824, 912, 60.362},  {12, 3, 1824, 828, 60.362},  {12, 3, 1824, 820, 60.362},
    {12, 3, 1824, 776, 60.362},  {12, 3, 1824, 764, 60.362},  {12, 3, 1820, 728, 60.362},
    {12, 3, 1824, 716, 60.362},
    // 10-bit
    {10, 1, 5472, 3648, 25.48},  {10, 1, 5472, 3076, 30.173}, {10, 1, 3840, 2160, 60.16},
    {10, 1, 3648, 3648, 25.48},  {10, 1, 4864, 3648, 25.48},  {10, 1, 5016, 3648, 25.48},
    {10, 1, 5472, 2956, 30.173}, {10, 1, 5472, 2880, 30.173}, {10, 1, 5472, 2736, 30.173},
    {10, 1, 5472, 2488, 30.173}, {10, 1, 5472, 2464, 30.173}, {10, 1, 5472, 2328, 30.173},
    {10, 1, 5472, 2288, 30.173}, {10, 1, 5472, 2188, 30.173}, {10, 1, 5472, 2144, 30.173},
}};

// The optical-black columns (leading, left) and rows (trailing, bottom) a binning adds to the active picture.
inline constexpr Size opticalBlack(uint32_t binning)
{
    return binning == 2 ? Size{48, 4} : binning == 3 ? Size{32, 4} : Size{96, 16};
}

inline constexpr Size transportOf(const Mode &mode)
{
    const Size black = opticalBlack(mode.binning);
    return Size{mode.width + black.width, mode.height + black.height};
}

// The mode with this ACTIVE size at this bit depth, or nullptr.
inline const Mode *find(uint32_t bits, uint32_t width, uint32_t height)
{
    for (const Mode &mode : kModes)
        if (mode.bits == bits && mode.width == width && mode.height == height)
            return &mode;
    return nullptr;
}

// The mode whose TRANSPORT size (what the driver and the probe call the mode) is this, or nullptr.
inline const Mode *findTransport(uint32_t bits, uint32_t width, uint32_t height)
{
    for (const Mode &mode : kModes) {
        const Size transport = transportOf(mode);
        if (mode.bits == bits && transport.width == width && transport.height == height)
            return &mode;
    }
    return nullptr;
}

// Whether this active size is a mode at any bit depth.
inline bool isActiveSize(uint32_t width, uint32_t height)
{
    return find(12, width, height) || find(10, width, height);
}

// The active size of a transport size, nullopt-like {0,0} when it is no mode.
inline Size activeOfTransport(uint32_t bits, uint32_t width, uint32_t height)
{
    const Mode *mode = findTransport(bits, width, height);
    return mode ? Size{mode->width, mode->height} : Size{};
}

// 1920x1080 and 4K (and their padded forms) are what the other sensors call "full HD" and "UHD".
inline constexpr bool isGenericAlias(uint32_t width, uint32_t height)
{
    return (width == 1920 && height == 1080) || (width == 1928 && height == 1090) ||
           (width == 3840 && height == 2160) || (width == 3856 && height == 2180);
}

// The active size Core uses for a request at a bit depth (10 or 12; anything else counts as 12).
// A size that is a mode at that depth stays. The generic full HD / UHD aliases become 2736x1540 at
// 12-bit (the 16:9 mode there) and 3840x2160 at 10-bit. Any other size takes the nearest mode of the
// depth (squared distance, the first in the table on a tie). Idempotent.
inline Size normalise(Size requested, uint32_t bits)
{
    bits = bits == 10 ? 10 : 12;
    if (find(bits, requested.width, requested.height))
        return requested;
    if (isGenericAlias(requested.width, requested.height))
        return bits == 10 ? Size{3840, 2160} : Size{2736, 1540};
    const Mode *best = nullptr;
    uint64_t bestDistance = 0;
    for (const Mode &mode : kModes) {
        if (mode.bits != bits)
            continue;
        const int64_t dw = static_cast<int64_t>(mode.width) - static_cast<int64_t>(requested.width);
        const int64_t dh = static_cast<int64_t>(mode.height) - static_cast<int64_t>(requested.height);
        const uint64_t distance = static_cast<uint64_t>(dw * dw + dh * dh);
        if (!best || distance < bestDistance) {
            best = &mode;
            bestDistance = distance;
        }
    }
    return Size{best->width, best->height};
}

// The driver's fps floor for an active size at a bit depth; 0 when it is no mode.
inline double floorFps(uint32_t bits, uint32_t width, uint32_t height)
{
    const Mode *mode = find(bits == 10 ? 10 : 12, width, height);
    return mode ? mode->floorFps : 0.0;
}

} // namespace apertar::imx283
