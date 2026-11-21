// The IMX585 lossless cDNG fps ceiling and its APERTAR_LOSSLESS_FPS_LIMIT knob. Pure, std only; host-tested by
// tools/fps_ceiling_test.cpp.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace apertar::fpsceiling {

// APERTAR_LOSSLESS_FPS_LIMIT=<n>: a number > 0 caps the IMX585 lossless-JPEG cDNG fps at n (still within the
// sensor mode's maximum). Anything else (unset, empty, 0, negative, text, trailing junk) is no override.
inline std::optional<double> parseLosslessFpsLimit(const char *value)
{
    if (!value || !*value)
        return std::nullopt;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || !(parsed > 0.0))
        return std::nullopt;
    return parsed;
}

// The IMX585 lossless-JPEG cDNG fps ceiling: the mode's maximum (modeMax, the probe and back-end cap), or
// min(override, modeMax) when APERTAR_LOSSLESS_FPS_LIMIT is set. 30 restores the old fixed cap.
inline double losslessImx585Fps(double modeMax, std::optional<double> overrideLimit)
{
    return overrideLimit && *overrideLimit > 0.0 ? std::min(modeMax, *overrideLimit) : modeMax;
}

} // namespace apertar::fpsceiling
