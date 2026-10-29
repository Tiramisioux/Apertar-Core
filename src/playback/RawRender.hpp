#pragma once

// Apertar playback, design sketch (not in any repository).
// A Qt-free copy of the touch UI's raw preview look, so a WebUI frame looks like the
// touch-UI frame of the same DNG. Source of truth today: ApertarUI d8098d0
// src/cdngimageprovider.cpp:380-698 (matrices, white balance, quad debayer, Reinhard,
// saturation 1.35, highlight roll-off, gamma 2.2) and src/dngdecoder.cpp:455-536 plus
// src/playbackcontroller.cpp:262-263 (48-bin raw histogram, ~16000 samples, counts^0.65).
// Named copy (KB rule M5): change one, change the other; playback_bench checks this one.

#include "DngTags.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace apertar::playback {

struct Rgb8Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3
};

namespace render {

struct M3 { float m[9]; };

constexpr M3 kXyzToSrgb = {{ 3.2406f, -1.5372f, -0.4986f, -0.9689f, 1.8758f, 0.0415f, 0.0557f, -0.2040f, 1.0570f }};
constexpr M3 kD50ToD65 = {{ 0.9555766f, -0.0230393f, 0.0631636f, -0.0282895f, 1.0099416f, 0.0210077f,
                            0.0122982f, -0.0204830f, 1.3299098f }};

inline M3 mul(const M3 &a, const M3 &b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}

inline bool invert(const M3 &x, M3 &out)
{
    const float *m = x.m;
    const float c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    const float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (std::fabs(det) < 1e-8f)
        return false;
    const float k = 1.0f / det;
    out = {{ c0 * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k,
             c1 * k, (m[0] * m[8] - m[2] * m[6]) * k, (m[2] * m[3] - m[0] * m[5]) * k,
             c2 * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k }};
    return true;
}

inline M3 fromArray(const float *v) { M3 r{}; std::copy(v, v + 9, r.m); return r; }
inline M3 blend(const float *a, const float *b, float t)
{
    M3 r{};
    for (int i = 0; i < 9; ++i) r.m[i] = a[i] + (b[i] - a[i]) * t;
    return r;
}

inline float smooth(float e0, float e1, float v)
{
    if (e1 <= e0) return v >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float profileBlend(const DngInfo &f)   // cdngimageprovider.cpp:379-395
{
    if (!f.hasNeutral) return 0.5f;
    const float ratio = std::max(f.neutral[2], 0.0001f) / std::max(f.neutral[0], 0.0001f);
    return smooth(0.30f, 0.90f, ratio);
}

inline bool cameraToSrgb(const DngInfo &f, M3 &out)  // cdngimageprovider.cpp:413-452
{
    const float t = profileBlend(f);
    if (f.hasFm1 || f.hasFm2) {
        const M3 fm = (f.hasFm1 && f.hasFm2) ? blend(f.fm1, f.fm2, t) : fromArray(f.hasFm1 ? f.fm1 : f.fm2);
        out = mul(kXyzToSrgb, mul(kD50ToD65, fm));
        return true;
    }
    if (!f.hasCm1 && !f.hasCm2) return false;
    const M3 cm = (f.hasCm1 && f.hasCm2) ? blend(f.cm1, f.cm2, t) : fromArray(f.hasCm1 ? f.cm1 : f.cm2);
    M3 inv{};
    if (!invert(cm, inv)) return false;
    out = mul(kXyzToSrgb, mul(kD50ToD65, inv));
    return true;
}

inline std::array<float, 3> gains(const DngInfo &f)   // cdngimageprovider.cpp:454-475
{
    if (!f.hasNeutral) return {2.8f, 1.0f, 1.6f};
    float r = 1.0f / std::max(f.neutral[0], 0.0001f), g = 1.0f / std::max(f.neutral[1], 0.0001f),
          b = 1.0f / std::max(f.neutral[2], 0.0001f);
    if (f.hasAnalog) { r *= f.analog[0]; g *= f.analog[1]; b *= f.analog[2]; }
    const float ref = std::max(g, 0.0001f);
    return {r / ref, 1.0f, b / ref};
}

inline uint8_t gamma8(float v)
{
    static const std::array<uint8_t, 4097> lut = [] {
        std::array<uint8_t, 4097> t{};
        for (int i = 0; i <= 4096; ++i)
            t[size_t(i)] = uint8_t(std::clamp(int(std::lround(std::pow(i / 4096.0f, 1.0f / 2.2f) * 255.0f)), 0, 255));
        return t;
    }();
    return lut[size_t(std::clamp(int(std::lround(v * 4096.0f)), 0, 4096))];
}

inline uint8_t srgb8(float v)
{
    static const std::array<uint8_t, 4097> lut = [] {
        std::array<uint8_t, 4097> t{};
        for (size_t i = 0; i < t.size(); ++i) {
            const float l = float(i) / 4096.0f;
            const float e = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            t[i] = uint8_t(std::clamp(int(std::lround(e * 255.0f)), 0, 255));
        }
        return t;
    }();
    return lut[size_t(std::clamp(int(std::lround(v * 4096.0f)), 0, 4096))];
}

} // namespace render

// Output size keeps the quad aspect: width `outWidth`, height from the picture.
inline Rgb8Image renderPreview(const DngInfo &f, const std::vector<uint16_t> &s, uint32_t outWidth)
{
    using namespace render;
    Rgb8Image img;
    const int W = int(f.width), H = int(f.height);
    const int quadW = f.monochrome ? W : (W / 2), quadH = f.monochrome ? H : (H / 2);
    img.width = std::max<uint32_t>(2, std::min<uint32_t>(outWidth, uint32_t(quadW)));
    img.height = std::max<uint32_t>(2, uint32_t(std::lround(double(img.width) * quadH / quadW)));
    img.rgb.assign(size_t(img.width) * img.height * 3, 0);
    const float inv = 1.0f / std::max(1.0f, f.white - f.black);
    auto norm = [&](int x, int y) {
        return std::clamp((float(s[size_t(y) * W + x]) - f.black) * inv, 0.0f, 1.0f);
    };
    auto quad = [&](int qx, int qy) -> std::array<float, 3> {
        const int x = qx * 2, y = qy * 2;
        const float p00 = norm(x, y), p10 = norm(x + 1, y), p01 = norm(x, y + 1), p11 = norm(x + 1, y + 1);
        switch (f.cfa) {
        case Cfa::BGGR: return {p11, (p10 + p01) * 0.5f, p00};
        case Cfa::GRBG: return {p10, (p00 + p11) * 0.5f, p01};
        case Cfa::GBRG: return {p01, (p00 + p11) * 0.5f, p10};
        default: return {p00, (p10 + p01) * 0.5f, p11};
        }
    };
    M3 c2s{};
    const bool colour = !f.monochrome && cameraToSrgb(f, c2s);
    const auto g = gains(f);
    for (uint32_t oy = 0; oy < img.height; ++oy) {
        uint8_t *row = img.rgb.data() + size_t(oy) * img.width * 3;
        const double sy = std::clamp((oy + 0.5) * quadH / img.height, 0.0, double(quadH - 1));
        const int qy0 = int(sy), qy1 = std::min(qy0 + 1, quadH - 1);
        const float ty = float(sy - qy0);
        for (uint32_t ox = 0; ox < img.width; ++ox) {
            const double sx = std::clamp((ox + 0.5) * quadW / img.width, 0.0, double(quadW - 1));
            if (f.monochrome) {
                const uint8_t grey = srgb8(norm(int(sx), int(sy)));
                row[ox * 3] = row[ox * 3 + 1] = row[ox * 3 + 2] = grey;
                continue;
            }
            const int qx0 = int(sx), qx1 = std::min(qx0 + 1, quadW - 1);
            const float tx = float(sx - qx0);
            const auto c00 = quad(qx0, qy0), c10 = quad(qx1, qy0), c01 = quad(qx0, qy1), c11 = quad(qx1, qy1);
            std::array<float, 3> cam{};
            for (int c = 0; c < 3; ++c) {
                const float top = c00[c] + (c10[c] - c00[c]) * tx, bottom = c01[c] + (c11[c] - c01[c]) * tx;
                cam[c] = top + (bottom - top) * ty;
            }
            const float sensorHighlight = std::max({cam[0], cam[1], cam[2]});
            const float bal[3] = {cam[0] * g[0], cam[1] * g[1], cam[2] * g[2]};
            float r = bal[0], gg = bal[1], b = bal[2];
            if (colour) {
                r = c2s.m[0] * bal[0] + c2s.m[1] * bal[1] + c2s.m[2] * bal[2];
                gg = c2s.m[3] * bal[0] + c2s.m[4] * bal[1] + c2s.m[5] * bal[2];
                b = c2s.m[6] * bal[0] + c2s.m[7] * bal[1] + c2s.m[8] * bal[2];
            }
            r = std::max(0.0f, r); gg = std::max(0.0f, gg); b = std::max(0.0f, b);
            r /= 1.0f + r; gg /= 1.0f + gg; b /= 1.0f + b;
            const float peak = std::max({r, gg, b});
            const float hl = std::max(smooth(0.88f, 1.0f, sensorHighlight), smooth(0.84f, 0.98f, peak));
            const float luma = 0.2126f * r + 0.7152f * gg + 0.0722f * b;
            const float sat = 1.0f + (1.35f - 1.0f) * (1.0f - hl);
            r = std::clamp(luma + (r - luma) * sat, 0.0f, 1.0f);
            gg = std::clamp(luma + (gg - luma) * sat, 0.0f, 1.0f);
            b = std::clamp(luma + (b - luma) * sat, 0.0f, 1.0f);
            const float wb = smooth(0.93f, 1.0f, sensorHighlight);
            const float wl = std::clamp(std::max({luma, r, gg, b}), 0.0f, 1.0f);
            row[ox * 3] = gamma8(r * (1 - wb) + wl * wb);
            row[ox * 3 + 1] = gamma8(gg * (1 - wb) + wl * wb);
            row[ox * 3 + 2] = gamma8(b * (1 - wb) + wl * wb);
        }
    }
    return img;
}

// The touch UI's DNG histogram: 48 bins of raw linear values, ~16000 samples on a square grid.
inline std::array<float, 48> rawHistogram(const DngInfo &f, const std::vector<uint16_t> &s)
{
    std::array<int, 48> counts{};
    const double total = double(f.width) * f.height;
    const int step = std::max(1, int(std::sqrt(total / 16000.0)));
    const float range = std::max(1.0f, f.white - f.black);
    int maxCount = 0;
    for (uint32_t y = 0; y < f.height; y += uint32_t(step))
        for (uint32_t x = 0; x < f.width; x += uint32_t(step)) {
            const float n = std::clamp((float(s[size_t(y) * f.width + x]) - f.black) / range, 0.0f, 1.0f);
            const int bin = std::min(47, int(n * 47.0f));
            maxCount = std::max(maxCount, ++counts[size_t(bin)]);
        }
    std::array<float, 48> bins{};
    for (size_t i = 0; i < 48; ++i)
        bins[i] = maxCount > 0 ? float(std::pow(double(counts[i]) / maxCount, 0.65)) : 0.0f;
    return bins;
}

} // namespace apertar::playback
