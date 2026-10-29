#pragma once

// Apertar playback, design sketch v2 (not in any repository).
// Part 2 of the reader (T48): the samples. Uncompressed 16-bit LE or 12/10-bit packed, read in
// chunks of rows; lossless JPEG tiles (SOF3, 2 components; tile size from tags 322/323, two tile
// columns when width % 16 != 0, CdngEncoder.cpp:1237-1241) decoded with liblj92, the same vendored
// file the touch UI uses (ApertarUI third_party/liblj92). LinearizationTable applied.
// v2 (critics M4, M8, m2): before each chunk or tile the caller's stop() is asked (playback yields
// to a take within one chunk); each range read is dropped from the page cache right after.

#include "DngTags.hpp"

#include <functional>

extern "C" {
#include "lj92.h"
}

namespace apertar::playback {

struct ReadOptions {
    std::function<bool()> stop;   // true = give up now (a hold started); asked before every chunk/tile
    uint32_t chunkRows = 128;     // uncompressed rows per pread (lossless: one tile per step)
    bool dropPages = true;        // POSIX_FADV_DONTNEED on each range once it is in memory
};

enum class ReadStatus { Ok, Failed, Stopped };

// Every sample of the frame, unpacked to 16-bit, row-major. LinearizationTable applied.
// samples and scratch belong to the caller's decode slot and keep their capacity between frames
// (critic M5: a fresh 33 MB buffer per request cost +65 % on a 16-bit frame).
inline ReadStatus readDngSamples(int fd, const DngInfo &info, std::vector<uint16_t> &samples,
                                 std::vector<uint8_t> &scratch, std::string &error,
                                 const ReadOptions &options = {})
{
    using namespace detail;
    const size_t w = info.width, h = info.height;
    auto stopped = [&]() {
        if (options.stop && options.stop()) {
            error = "Playback is held.";
            return true;
        }
        return false;
    };
    auto drop = [&](uint64_t offset, uint64_t length) {
        if (options.dropPages)
            ::posix_fadvise(fd, off_t(offset), off_t(length), POSIX_FADV_DONTNEED);
    };
    samples.resize(w * h);   // bounded: readDngInfo capped w x h and checked the strip/tiles
    if (info.compression == 1) {
        const size_t rowBytes = dngRowBytes(info);
        const size_t chunkRows = std::max<size_t>(1, options.chunkRows);
        scratch.resize(rowBytes * std::min(chunkRows, h));
        for (size_t y0 = 0; y0 < h; y0 += chunkRows) {
            if (stopped())
                return ReadStatus::Stopped;
            const size_t rows = std::min(chunkRows, h - y0);
            const uint64_t offset = uint64_t(info.stripOffset) + uint64_t(y0) * rowBytes;
            if (!preadAll(fd, scratch.data(), rows * rowBytes, offset)) {
                error = "Could not read the frame (short file?).";
                return ReadStatus::Failed;
            }
            drop(offset, uint64_t(rows) * rowBytes);
            for (size_t y = y0; y < y0 + rows; ++y) {
                const uint8_t *src = scratch.data() + (y - y0) * rowBytes;
                uint16_t *dst = samples.data() + y * w;
                if (info.bits == 16) {
                    for (size_t x = 0; x < w; ++x) dst[x] = rd16(src + 2 * x);
                } else if (info.bits == 12) {
                    for (size_t x = 0; x + 1 < w; x += 2, src += 3) {
                        dst[x] = uint16_t((src[0] << 4) | (src[1] >> 4));
                        dst[x + 1] = uint16_t(((src[1] & 0x0F) << 8) | src[2]);
                    }
                } else {  // 10-bit, 4 samples in 5 bytes
                    for (size_t x = 0; x + 3 < w; x += 4, src += 5) {
                        dst[x] = uint16_t((src[0] << 2) | (src[1] >> 6));
                        dst[x + 1] = uint16_t(((src[1] & 0x3F) << 4) | (src[2] >> 4));
                        dst[x + 2] = uint16_t(((src[2] & 0x0F) << 6) | (src[3] >> 2));
                        dst[x + 3] = uint16_t(((src[3] & 0x03) << 8) | src[4]);
                    }
                }
            }
        }
    } else {
        const size_t tilesAcross = (w + info.tileWidth - 1) / info.tileWidth;
        std::vector<uint16_t> tile(size_t(info.tileWidth) * info.tileLength);
        for (size_t t = 0; t < info.tileOffsets.size(); ++t) {
            if (stopped())
                return ReadStatus::Stopped;
            scratch.resize(info.tileBytes[t]);
            if (!preadAll(fd, scratch.data(), scratch.size(), info.tileOffsets[t])) {
                error = "Could not read a lossless tile (short file?).";
                return ReadStatus::Failed;
            }
            drop(info.tileOffsets[t], info.tileBytes[t]);
            lj92 handle = nullptr;
            int jw = 0, jh = 0, jbits = 0, jcomp = 0;
            if (lj92_open(&handle, scratch.data(), int(scratch.size()), &jw, &jh, &jbits, &jcomp) != LJ92_ERROR_NONE) {
                error = "Lossless tile header is corrupt.";
                return ReadStatus::Failed;
            }
            const int rc = (size_t(jw) * size_t(jcomp) == info.tileWidth && size_t(jh) == info.tileLength)
                ? lj92_decode(handle, tile.data(), jw * jcomp, 0, nullptr, 0) : LJ92_ERROR_CORRUPT;
            lj92_close(handle);
            if (rc != LJ92_ERROR_NONE) {
                error = "Lossless tile does not decode.";
                return ReadStatus::Failed;
            }
            const size_t tx = (t % tilesAcross) * info.tileWidth, ty = (t / tilesAcross) * info.tileLength;
            const size_t cols = std::min<size_t>(info.tileWidth, w - tx);
            for (size_t r = 0; r < info.tileLength && ty + r < h; ++r)
                std::memcpy(samples.data() + (ty + r) * w + tx, tile.data() + r * info.tileWidth, cols * 2);
        }
    }
    if (!info.linearization.empty()) {
        // The touch UI rejects a code past the table (dngdecoder.cpp:734); this copy clamps to the
        // last entry. They differ only on a corrupt file (critic m18), so the parity test never sees it.
        const size_t last = info.linearization.size() - 1;
        for (uint16_t &s : samples)
            s = info.linearization[std::min<size_t>(s, last)];
    }
    return ReadStatus::Ok;
}

} // namespace apertar::playback
