// The maximum fps of a mode: the sensor's own answer (the startup probe) when there is one,
// Core's hard-coded table only as the fallback. Pure, std only; host-tested by
// tools/fps_ceiling_test.cpp.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace apertar::fpsceiling {

// probed: the fps the sensor probe reports for the mode (absent, or <= 0, when it has none).
// table: Core's hard-coded ceiling. tableOnly: APERTAR_FPS_LIMIT=table, the old min(table, probed).
inline double resolveSensorMaxFps(std::optional<double> probed, double table, bool tableOnly)
{
    if (!probed || !(*probed > 0.0))
        return table;
    return tableOnly ? std::min(table, *probed) : *probed;
}

// A back-end limit that is not the sensor's (e.g. 16-bit Clear HDR at 4K), applied after the sensor value.
inline double applyBackendCap(double sensorMax, std::optional<double> cap)
{
    return cap && *cap > 0.0 ? std::min(sensorMax, *cap) : sensorMax;
}

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
