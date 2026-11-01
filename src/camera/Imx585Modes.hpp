#pragma once

// The IMX585 modes the cinemate-port driver advertises, per class, and the size rules around them.
// std-only: tools/imx585_modes_test.cpp includes it without libcamera.
//
// Sizes are the ACTIVE picture, the size Core records and shows. What the driver delivers (the
// transport size) is the same for the three classes below; only 16-bit Clear HDR prepends
// optical-black rows: +40 rows at 1x1, +20 rows binned, split evenly top and bottom, so the
// centred raw crop in CameraController lands on the active picture.
//
//   S12 SDR 12-bit      S10 SDR 10-bit      H12 Clear HDR 12-bit      H16 Clear HDR 16-bit
//
// A row with `binned` is a 2x2 binned mode. (1920, 1080) in H16 is listed twice, 1x1 and binned; the
// 1x1 window (transport 1920x1120) is not offered, so every lookup answers the binned row.
// Modes narrower than minModeWidth() (1280 px; APERTAR_IMX585_MIN_MODE_WIDTH overrides) are hidden
// as in cinemate: find() and nearest() do not see them.

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <vector>

namespace apertar::imx585 {

enum class Class { S12, S10, H12, H16 };

struct Mode {
    uint32_t width;     // active picture
    uint32_t height;
    bool binned;        // 2x2 binned readout
};

struct Size {
    uint32_t width;
    uint32_t height;
    bool operator==(const Size &other) const { return width == other.width && height == other.height; }
};

inline constexpr Mode kS12Modes[] = {
    { 1920, 1080, true }, { 3840, 2160, false }, { 2880, 2160, false }, { 2160, 2160, false },
    { 2976, 2160, false }, { 3840, 2072, false }, { 3840, 2032, false }, { 3840, 2024, false },
    { 3840, 1920, false }, { 3840, 1744, false }, { 3840, 1728, false }, { 3840, 1632, false },
    { 3840, 1608, false }, { 3840, 1536, false }, { 3840, 1504, false }, { 1920, 1040, false },
    { 1920, 1008, false }, { 1920, 800, false }, { 2048, 1152, false }, { 2048, 1104, false },
    { 2048, 1080, false }, { 2048, 1024, false }, { 2048, 928, false }, { 2048, 920, false },
    { 2048, 872, false }, { 2048, 856, false }, { 2048, 816, false }, { 2048, 800, false },
    { 1440, 1080, false }, { 1080, 1080, true }, { 1488, 1080, true }, { 1920, 1036, true },
    { 1920, 1016, true }, { 1920, 1012, true }, { 1920, 960, true }, { 1920, 872, true },
    { 1920, 864, true }, { 1920, 816, true }, { 1920, 804, true }, { 1920, 768, true },
    { 1920, 752, true }, { 960, 540, true }, { 960, 520, true }, { 960, 508, true },
    { 960, 504, true }, { 960, 480, true }, { 960, 436, true }, { 960, 432, true },
    { 960, 408, true }, { 960, 400, true }, { 960, 384, true }, { 960, 376, true },
    { 1024, 576, true }, { 1024, 552, true }, { 1024, 540, true }, { 1024, 512, true },
    { 1024, 464, true }, { 1024, 460, true }, { 1024, 436, true }, { 1024, 428, true },
    { 1024, 408, true }, { 1024, 400, true },
};

inline constexpr Mode kS10Modes[] = {
    { 3840, 2160, false }, { 1920, 1080, true }, { 1440, 1080, true }, { 2880, 2160, false },
    { 2160, 2160, false }, { 2976, 2160, false }, { 3840, 2072, false }, { 3840, 2032, false },
    { 3840, 2024, false }, { 3840, 1920, false }, { 3840, 1744, false }, { 3840, 1728, false },
    { 3840, 1632, false }, { 3840, 1608, false }, { 3840, 1536, false }, { 3840, 1504, false },
    { 1920, 1040, false }, { 1920, 1008, false }, { 1920, 800, false }, { 2048, 1152, false },
    { 2048, 1104, false }, { 2048, 1080, false }, { 2048, 1024, false }, { 2048, 928, false },
    { 2048, 920, false }, { 2048, 872, false }, { 2048, 856, false }, { 2048, 816, false },
    { 2048, 800, false }, { 1080, 1080, true }, { 1488, 1080, true }, { 1920, 1036, true },
    { 1920, 1016, true }, { 1920, 1012, true }, { 1920, 960, true }, { 1920, 872, true },
    { 1920, 864, true }, { 1920, 816, true }, { 1920, 804, true }, { 1920, 768, true },
    { 1920, 752, true }, { 960, 540, true }, { 960, 520, true }, { 960, 508, true },
    { 960, 504, true }, { 960, 480, true }, { 960, 436, true }, { 960, 432, true },
    { 960, 408, true }, { 960, 400, true }, { 960, 384, true }, { 960, 376, true },
    { 1024, 576, true }, { 1024, 552, true }, { 1024, 540, true }, { 1024, 512, true },
    { 1024, 464, true }, { 1024, 460, true }, { 1024, 436, true }, { 1024, 428, true },
    { 1024, 408, true }, { 1024, 400, true },
};

inline constexpr Mode kH12Modes[] = {
    { 1920, 1080, true }, { 3840, 2160, false }, { 2880, 2160, false }, { 2160, 2160, false },
    { 2976, 2160, false }, { 3840, 2072, false }, { 3840, 2032, false }, { 3840, 2024, false },
    { 3840, 1920, false }, { 3840, 1744, false }, { 3840, 1728, false }, { 3840, 1632, false },
    { 3840, 1608, false }, { 3840, 1536, false }, { 3840, 1504, false }, { 1920, 1040, false },
    { 1920, 1008, false }, { 1920, 800, false }, { 2048, 1152, false }, { 2048, 1104, false },
    { 2048, 1080, false }, { 2048, 1024, false }, { 2048, 928, false }, { 2048, 920, false },
    { 2048, 872, false }, { 2048, 856, false }, { 2048, 816, false }, { 2048, 800, false },
    { 1440, 1080, false },
};

inline constexpr Mode kH16Modes[] = {
    { 3840, 2160, false }, { 2880, 2160, false }, { 2160, 2160, false }, { 2976, 2160, false },
    { 3840, 2072, false }, { 3840, 2032, false }, { 3840, 2024, false }, { 3840, 1920, false },
    { 3840, 1744, false }, { 3840, 1728, false }, { 3840, 1632, false }, { 3840, 1608, false },
    { 3840, 1536, false }, { 3840, 1504, false }, { 1920, 1080, false }, { 1920, 1016, false },
    { 1920, 1008, false }, { 1920, 960, false }, { 1920, 872, false }, { 1920, 864, false },
    { 1920, 816, false }, { 1920, 800, false }, { 1920, 768, false }, { 1920, 752, false },
    { 2048, 1152, false }, { 2048, 1104, false }, { 2048, 1080, false }, { 2048, 1024, false },
    { 2048, 928, false }, { 2048, 920, false }, { 2048, 872, false }, { 2048, 856, false },
    { 2048, 800, false }, { 1920, 1080, true },
};

constexpr uint32_t kDefaultMinModeWidth = 1280;

// APERTAR_IMX585_MIN_MODE_WIDTH=<n>: modes narrower than n are hidden; 0 shows them all; a negative or
// unparsable n is ignored.
inline uint32_t minModeWidth()
{
    if (const char *value = std::getenv("APERTAR_IMX585_MIN_MODE_WIDTH"); value && *value) {
        char *end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (end && end != value && *end == '\0' && parsed >= 0 && parsed <= 0xffffffffLL)
            return static_cast<uint32_t>(parsed);       // a negative or unparsable value keeps the default
    }
    return kDefaultMinModeWidth;
}

// 16-bit is always Clear HDR on this driver; 10-bit never is.
constexpr Class classFor(uint32_t bitDepth, bool clearHdr)
{
    if (clearHdr)
        return bitDepth == 16 ? Class::H16 : Class::H12;
    return bitDepth == 10 ? Class::S10 : Class::S12;
}

constexpr bool clearHdrClass(Class cls) { return cls == Class::H12 || cls == Class::H16; }

constexpr uint32_t bitDepthOf(Class cls)
{
    switch (cls) {
    case Class::S12: case Class::H12: return 12;
    case Class::S10: return 10;
    case Class::H16: return 16;
    }
    return 12;
}

// Every row of the class as the driver lists it (62, 62, 29, 34), hidden and shadowed rows included.
struct Rows {
    const Mode *first;
    const Mode *last;
    const Mode *begin() const { return first; }
    const Mode *end() const { return last; }
    size_t size() const { return static_cast<size_t>(last - first); }
};

inline Rows all(Class cls)
{
    switch (cls) {
    case Class::S12: return { std::begin(kS12Modes), std::end(kS12Modes) };
    case Class::S10: return { std::begin(kS10Modes), std::end(kS10Modes) };
    case Class::H12: return { std::begin(kH12Modes), std::end(kH12Modes) };
    case Class::H16: return { std::begin(kH16Modes), std::end(kH16Modes) };
    }
    return { std::begin(kS12Modes), std::end(kS12Modes) };
}

// The 1x1 row of an H16 size that also has a binned row is not offered.
inline bool shadowed(Class cls, const Mode &mode)
{
    if (mode.binned)
        return false;
    for (const Mode &other : all(cls)) {
        if (other.binned && other.width == mode.width && other.height == mode.height)
            return true;
    }
    return false;
}

// What the UI may offer: not hidden by the width policy, not shadowed. In the driver's order.
inline std::vector<Mode> offered(Class cls, uint32_t minWidth = minModeWidth())
{
    std::vector<Mode> out;
    for (const Mode &mode : all(cls)) {
        if (mode.width >= minWidth && !shadowed(cls, mode))
            out.push_back(mode);
    }
    return out;
}

// The offered row for an active size: the binned one if the class lists both.
inline const Mode *find(Class cls, uint32_t width, uint32_t height, uint32_t minWidth = minModeWidth())
{
    if (width < minWidth)
        return nullptr;
    const Mode *found = nullptr;
    for (const Mode &mode : all(cls)) {
        if (mode.width == width && mode.height == height && (!found || mode.binned))
            found = &mode;
    }
    return found;
}

// Any class lists the size (hidden or not): is it a size of this sensor at all?
inline bool anyClassHas(uint32_t width, uint32_t height)
{
    for (const Class cls : { Class::S12, Class::S10, Class::H12, Class::H16 }) {
        if (find(cls, width, height, 0))
            return true;
    }
    return false;
}

// The driver delivers the active picture, plus the optical-black rows in 16-bit Clear HDR.
inline Size transportFor(Class cls, const Mode &mode)
{
    if (cls != Class::H16)
        return { mode.width, mode.height };
    return { mode.width, mode.height + (mode.binned ? 20u : 40u) };
}

inline std::optional<Size> transportFor(Class cls, uint32_t width, uint32_t height,
                                        uint32_t minWidth = minModeWidth())
{
    if (const Mode *mode = find(cls, width, height, minWidth))
        return transportFor(cls, *mode);
    return std::nullopt;
}

// The active picture inside a delivered frame: the transport itself except in 16-bit, where the
// optical-black rows come off. nullopt: no offered 16-bit row has that transport size.
inline std::optional<Size> activeFor(Size transport, uint32_t bitDepth)
{
    if (bitDepth != 16)
        return transport;
    const Mode *found = nullptr;
    for (const Mode &mode : all(Class::H16)) {
        if (transportFor(Class::H16, mode) == transport && !shadowed(Class::H16, mode))
            found = &mode;
    }
    if (!found)
        return std::nullopt;
    return Size{ found->width, found->height };
}

// The offered row closest to a size (Manhattan distance on the active picture; the driver's order
// breaks ties). nullptr only when the width policy hides the whole class.
inline const Mode *nearest(Class cls, uint32_t width, uint32_t height, uint32_t minWidth = minModeWidth())
{
    const Mode *best = nullptr;
    uint64_t bestDistance = 0;
    for (const Mode &mode : all(cls)) {
        if (mode.width < minWidth || shadowed(cls, mode))
            continue;
        const uint64_t distance =
            (mode.width > width ? mode.width - width : width - mode.width) +
            (mode.height > height ? mode.height - height : height - mode.height);
        if (!best || distance < bestDistance) {
            best = &mode;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace apertar::imx585
