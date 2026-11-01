#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace apertar {

struct NativeScalerCrop {
    int x = 0;
    int y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

inline std::optional<NativeScalerCrop> centeredRawWindowScalerCrop(
    uint32_t sourceWidth, uint32_t sourceHeight,
    uint32_t targetWidth, uint32_t targetHeight, NativeScalerCrop nativeBounds)
{
    if (targetWidth < 2 || targetHeight < 2 || targetWidth > sourceWidth ||
        targetHeight > sourceHeight || nativeBounds.width < 2 || nativeBounds.height < 2)
        return std::nullopt;
    const uint32_t left = ((sourceWidth - targetWidth) / 2u) & ~1u;
    const uint32_t top = ((sourceHeight - targetHeight) / 2u) & ~1u;
    return NativeScalerCrop{
        nativeBounds.x + static_cast<int>((uint64_t(nativeBounds.width) * left / sourceWidth) & ~1ull),
        nativeBounds.y + static_cast<int>((uint64_t(nativeBounds.height) * top / sourceHeight) & ~1ull),
        static_cast<uint32_t>((uint64_t(nativeBounds.width) * targetWidth / sourceWidth) & ~1ull),
        static_cast<uint32_t>((uint64_t(nativeBounds.height) * targetHeight / sourceHeight) & ~1ull)};
}

inline std::optional<NativeScalerCrop> centeredPreviewScalerCrop(
    uint32_t sourceWidth, uint32_t sourceHeight,
    uint32_t targetWidth, uint32_t targetHeight, NativeScalerCrop nativeBounds)
{
    if (sourceWidth < 2 || sourceHeight < 2 || targetWidth < 2 || targetHeight < 2 ||
        nativeBounds.width < 2 || nativeBounds.height < 2)
        return std::nullopt;

    uint32_t cropWidth = sourceWidth;
    uint32_t cropHeight = sourceHeight;
    const uint64_t sourceAspect = uint64_t(sourceWidth) * targetHeight;
    const uint64_t targetAspect = uint64_t(sourceHeight) * targetWidth;
    if (sourceAspect > targetAspect) {
        cropWidth = std::min(sourceWidth, static_cast<uint32_t>(
            std::max<uint64_t>(2, (uint64_t(sourceHeight) * targetWidth / targetHeight) & ~1ull)));
    } else if (sourceAspect < targetAspect) {
        cropHeight = std::min(sourceHeight, static_cast<uint32_t>(
            std::max<uint64_t>(2, (uint64_t(sourceWidth) * targetHeight / targetWidth) & ~1ull)));
    }

    // ScalerCrop uses native pixels, not the (possibly binned/padded) RAW output.
    // Preserve the driver's whole active area unless a software aspect crop is needed.
    const uint32_t nativeWidth = cropWidth == sourceWidth ? nativeBounds.width
        : std::min(nativeBounds.width, static_cast<uint32_t>(std::max<uint64_t>(2,
            (uint64_t(nativeBounds.width) * cropWidth / sourceWidth) & ~1ull)));
    const uint32_t nativeHeight = cropHeight == sourceHeight ? nativeBounds.height
        : std::min(nativeBounds.height, static_cast<uint32_t>(std::max<uint64_t>(2,
            (uint64_t(nativeBounds.height) * cropHeight / sourceHeight) & ~1ull)));
    return NativeScalerCrop{
        nativeBounds.x + static_cast<int>(((nativeBounds.width - nativeWidth) / 2) & ~1u),
        nativeBounds.y + static_cast<int>(((nativeBounds.height - nativeHeight) / 2) & ~1u),
        nativeWidth, nativeHeight};
}

// A sensor whose delivered (transport) frame carries leading-left optical-black columns and
// trailing-bottom optical-black rows (IMX283): the active picture is (outW - activeW) columns in from
// the left and starts at the top. The crop is derived from the runtime ScalerCropMaximum (nativeBounds):
// its origin may be offset and its size is the driver's, so no native coordinates are hard-coded.
// Offsets and sizes round up (a pixel more optical black is better than a pixel less picture), and the
// result is held inside nativeBounds. The caller centres the target aspect inside it.
inline std::optional<NativeScalerCrop> activeAreaScalerCrop(
    uint32_t outWidth, uint32_t outHeight, uint32_t activeWidth, uint32_t activeHeight,
    NativeScalerCrop nativeBounds)
{
    if (outWidth < 2 || outHeight < 2 || activeWidth < 2 || activeHeight < 2 ||
        activeWidth > outWidth || activeHeight > outHeight ||
        nativeBounds.width < 2 || nativeBounds.height < 2)
        return std::nullopt;
    const auto ceilDiv = [](uint64_t numerator, uint64_t denominator) {
        return (numerator + denominator - 1) / denominator;
    };
    const uint64_t left = ceilDiv(uint64_t(outWidth - activeWidth) * nativeBounds.width, outWidth);
    const uint64_t width = std::min<uint64_t>(ceilDiv(uint64_t(activeWidth) * nativeBounds.width, outWidth),
                                              nativeBounds.width - std::min<uint64_t>(left, nativeBounds.width));
    const uint64_t height = std::min<uint64_t>(ceilDiv(uint64_t(activeHeight) * nativeBounds.height, outHeight),
                                               nativeBounds.height);
    if (width < 2 || height < 2)
        return std::nullopt;
    return NativeScalerCrop{nativeBounds.x + static_cast<int>(left), nativeBounds.y,
                            static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

} // namespace apertar
