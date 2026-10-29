#pragma once

// Apertar playback, design sketch v2 (not in any repository).
// Part 1 of the reader: the tags of a DNG written by Core's CdngEncoder, without Qt, libtiff or
// liblj92, so T47 compiles before liblj92 is copied in T48 (critic B2).
//   - the 8-byte header and IFD0 + EXIF, which Core writes at the END of the file
//     (CdngEncoder.cpp:1384-1392), with one pread of the tail;
//   - every bound checked against the file and a size cap BEFORE any buffer is allocated (M9).
// Part 2 (DngFrameReader.hpp, T48) reads the samples.
// Black level: the first value, as the touch UI does (dngdecoder.cpp:963-966).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace apertar::playback {

constexpr uint32_t kMaxDngWidth = 4096;
constexpr uint32_t kMaxDngHeight = 3072;

enum class Cfa { RGGB = 0, BGGR = 1, GRBG = 2, GBRG = 3 };  // touch UI RawFrame order

struct DngInfo {
    uint32_t width = 0, height = 0;
    uint16_t bits = 0, compression = 1, photometric = 32803, samplesPerPixel = 1;
    bool monochrome = false;
    Cfa cfa = Cfa::RGGB;
    float black = 0.0f, white = 0.0f;
    std::vector<uint16_t> linearization;
    uint32_t stripOffset = 0, stripBytes = 0;
    uint32_t tileWidth = 0, tileLength = 0;
    std::vector<uint32_t> tileOffsets, tileBytes;
    double fps = 0.0;                     // CinemaDNG FrameRate 51044
    std::string dateTimeOriginal;         // 0x9003 "YYYY:MM:DD HH:MM:SS"
    std::string model, pageName;
    bool hasTimecode = false;
    uint8_t timecode[8] = {};             // CinemaDNG TimeCodes 51043 (SMPTE BCD)
    bool hasCm1 = false, hasCm2 = false, hasFm1 = false, hasFm2 = false;
    bool hasNeutral = false, hasAnalog = false;
    float cm1[9] = {}, cm2[9] = {}, fm1[9] = {}, fm2[9] = {};
    float neutral[3] = {1, 1, 1}, analog[3] = {1, 1, 1};
    uint32_t tailBytesRead = 0;           // what readDngInfo cost in I/O
};

// Bytes per stored row of an uncompressed payload: 16-bit LE, or 12/10-bit packed MSB first
// (CdngEncoder.cpp:1155-1184).
inline size_t dngRowBytes(const DngInfo &info)
{
    return info.bits == 16 ? size_t(info.width) * 2 : (size_t(info.width) * info.bits + 7) / 8;
}

// Playback opens frames as Core's writer does: O_CLOEXEC, so a forked zip or sudo helper never
// holds a frame open during an eject (critic M4), and drops what it read from the page cache
// when done (the writer's POSIX_FADV_DONTNEED, CdngEncoder.cpp:1779), so playback leaves memory
// as it found it before the next take (critic M4, operator P19).
inline int openDngFrame(const char *path) { return ::open(path, O_RDONLY | O_CLOEXEC); }
inline void closeDngFrame(int fd)
{
    if (fd < 0) return;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
}

namespace detail {

inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

inline bool preadAll(int fd, void *dst, size_t size, uint64_t offset)
{
    auto *out = static_cast<uint8_t *>(dst);
    while (size > 0) {
        const ssize_t n = ::pread(fd, out, size, static_cast<off_t>(offset));
        if (n <= 0)
            return false;
        out += n;
        size -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

inline int typeSize(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

// The tail buffer starts at file offset `base`; every value Core writes sits after IFD0.
struct Tail {
    std::vector<uint8_t> bytes;
    uint64_t base = 0;
    const uint8_t *at(uint64_t offset, uint64_t size) const
    {
        if (offset < base || offset - base + size > bytes.size())
            return nullptr;
        return bytes.data() + (offset - base);
    }
};

struct Entry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    const uint8_t *data = nullptr;  // points into the tail
};

inline double number(const Entry &e, uint32_t i)
{
    if (!e.data || i >= e.count)
        return 0.0;
    const uint8_t *p = e.data + i * typeSize(e.type);
    switch (e.type) {
    case 1: case 7: return p[0];
    case 3: return rd16(p);
    case 8: return int16_t(rd16(p));
    case 4: return rd32(p);
    case 9: return int32_t(rd32(p));
    case 5: { const uint32_t d = rd32(p + 4); return d ? double(rd32(p)) / d : 0.0; }
    case 10: { const int32_t d = int32_t(rd32(p + 4)); return d ? double(int32_t(rd32(p))) / d : 0.0; }
    case 11: { float f; std::memcpy(&f, p, 4); return f; }
    case 12: { double v; std::memcpy(&v, p, 8); return v; }
    default: return 0.0;
    }
}

inline bool readIfd(const Tail &tail, uint64_t offset, std::vector<Entry> &out)
{
    const uint8_t *head = tail.at(offset, 2);
    if (!head)
        return false;
    const uint16_t count = rd16(head);
    const uint8_t *dir = tail.at(offset + 2, uint64_t(count) * 12u);
    if (!dir)
        return false;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *p = dir + i * 12u;
        Entry e;
        e.tag = rd16(p);
        e.type = rd16(p + 2);
        e.count = rd32(p + 4);
        const uint64_t size = uint64_t(typeSize(e.type)) * e.count;
        e.data = size <= 4 ? p + 8 : tail.at(rd32(p + 8), size);
        out.push_back(e);
    }
    return true;
}

inline const Entry *find(const std::vector<Entry> &entries, uint16_t tag)
{
    for (const Entry &e : entries)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

inline std::string ascii(const Entry *e)
{
    if (!e || !e->data)
        return {};
    std::string s(reinterpret_cast<const char *>(e->data), e->count);
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

inline void matrix(const Entry *e, float *out, bool &has)
{
    if (!e || e->count < 9)
        return;
    for (uint32_t i = 0; i < 9; ++i)
        out[i] = float(number(*e, i));
    has = true;
}

} // namespace detail

// Header + tail only. For Core's files the tail is IFD0, its values and the EXIF IFD.
inline bool readDngInfo(int fd, DngInfo &info, std::string &error)
{
    using namespace detail;
    struct stat st {};
    uint8_t header[8];
    if (::fstat(fd, &st) != 0 || st.st_size < 16 || !preadAll(fd, header, 8, 0)) {
        error = "File is empty or unreadable.";             // a 0-byte DNG after a pulled card
        return false;
    }
    if (header[0] != 'I' || header[1] != 'I' || rd16(header + 2) != 42) {
        error = "Not a little-endian TIFF/DNG.";
        return false;
    }
    const uint32_t ifdOffset = rd32(header + 4);
    if (ifdOffset < 8 || uint64_t(ifdOffset) + 2 > uint64_t(st.st_size)) {
        error = "IFD0 offset is outside the file.";
        return false;
    }
    Tail tail;
    tail.base = ifdOffset;
    const uint64_t tailSize = uint64_t(st.st_size) - ifdOffset;
    if (tailSize > (1u << 20)) {
        error = "DNG tail is larger than 1 MiB; not a Core frame.";
        return false;
    }
    tail.bytes.resize(tailSize);
    if (!preadAll(fd, tail.bytes.data(), tailSize, ifdOffset)) {
        error = "Could not read the DNG tags.";
        return false;
    }
    info.tailBytesRead = uint32_t(8 + tailSize);
    std::vector<Entry> ifd0;
    if (!readIfd(tail, ifdOffset, ifd0)) {
        error = "IFD0 is truncated.";
        return false;
    }
    auto num = [&](uint16_t tag, uint32_t i = 0) -> double {
        const Entry *e = find(ifd0, tag);
        return e ? number(*e, i) : 0.0;
    };
    info.width = uint32_t(num(256));
    info.height = uint32_t(num(257));
    info.bits = uint16_t(num(258));
    info.compression = uint16_t(num(259));
    info.photometric = uint16_t(num(262));
    info.samplesPerPixel = uint16_t(std::max(1.0, num(277)));
    info.monochrome = info.photometric == 34892;
    if (const Entry *cfa = find(ifd0, 0x828E); cfa && cfa->count >= 4 && cfa->data) {
        const uint8_t *c = cfa->data;
        if (c[0] == 2 && c[3] == 0) info.cfa = Cfa::BGGR;
        else if (c[0] == 1 && c[1] == 0) info.cfa = Cfa::GRBG;
        else if (c[0] == 1 && c[1] == 2) info.cfa = Cfa::GBRG;
        else info.cfa = Cfa::RGGB;
    }
    info.black = float(num(0xC61A));
    info.white = float(num(0xC61D));
    if (const Entry *lin = find(ifd0, 0xC618); lin && lin->data) {
        info.linearization.resize(lin->count);
        for (uint32_t i = 0; i < lin->count; ++i)
            info.linearization[i] = uint16_t(number(*lin, i));
    }
    if (info.compression == 7) {
        info.tileWidth = uint32_t(num(322));
        info.tileLength = uint32_t(num(323));
        const Entry *offsets = find(ifd0, 324), *counts = find(ifd0, 325);
        if (!offsets || !counts || offsets->count != counts->count) {
            error = "Tile tables are missing.";
            return false;
        }
        for (uint32_t i = 0; i < offsets->count; ++i) {
            info.tileOffsets.push_back(uint32_t(number(*offsets, i)));
            info.tileBytes.push_back(uint32_t(number(*counts, i)));
        }
    } else {
        info.stripOffset = uint32_t(num(273));
        info.stripBytes = uint32_t(num(279));
    }
    info.fps = num(51044);
    info.model = ascii(find(ifd0, 272));
    info.pageName = ascii(find(ifd0, 285));
    info.dateTimeOriginal = ascii(find(ifd0, 0x9003));
    if (const Entry *tc = find(ifd0, 51043); tc && tc->count >= 8 && tc->data) {
        std::memcpy(info.timecode, tc->data, 8);
        info.hasTimecode = true;
    }
    matrix(find(ifd0, 0xC621), info.cm1, info.hasCm1);
    matrix(find(ifd0, 0xC622), info.cm2, info.hasCm2);
    matrix(find(ifd0, 0xC714), info.fm1, info.hasFm1);
    matrix(find(ifd0, 0xC715), info.fm2, info.hasFm2);
    if (const Entry *n = find(ifd0, 0xC628); n && n->count >= 3) {
        for (int i = 0; i < 3; ++i) info.neutral[i] = float(number(*n, i));
        info.hasNeutral = true;
    }
    if (const Entry *a = find(ifd0, 0xC627); a && a->count >= 3) {
        for (int i = 0; i < 3; ++i) info.analog[i] = float(number(*a, i));
        info.hasAnalog = true;
    }
    // Size cap before anything is allocated (critic M9). The largest frame Core writes is the
    // IMX477's 4056 x 3040 (CameraController.cpp:417, :471-472); IMX585 4K is 3856 x 2180 and the
    // 16-bit Clear HDR readout 3840 x 2200 (:393-395). 4096 x 3072 covers all with room.
    if (info.width < 2 || info.height < 2 || info.width > kMaxDngWidth || info.height > kMaxDngHeight ||
        uint64_t(info.width) * info.height > uint64_t(kMaxDngWidth) * kMaxDngHeight ||
        (info.bits != 10 && info.bits != 12 && info.bits != 16) ||
        (info.compression != 1 && info.compression != 7)) {
        error = "Unsupported DNG layout (size, bit depth or compression).";
        return false;
    }
    // Every payload byte must lie inside the file before anything is allocated for it.
    const uint64_t fileSize = uint64_t(st.st_size);
    if (info.compression == 1) {
        if (uint64_t(info.stripOffset) + info.stripBytes > fileSize) {
            error = "The frame is shorter than its tags say (truncated file?).";
            return false;
        }
        // The strip must hold every row the tags promise, checked here, before any buffer exists
        // (v1 checked it after samples.resize(), critic M9: 512 MB for a 16384 x 16384 lie).
        if (uint64_t(dngRowBytes(info)) * info.height > info.stripBytes) {
            error = "Strip is shorter than the image.";
            return false;
        }
    } else {
        if (info.tileWidth < 16 || info.tileWidth > info.width || info.tileLength < 1 || info.tileLength > info.height ||
            info.tileOffsets.size() != ((info.width + info.tileWidth - 1) / info.tileWidth) *
                                       ((info.height + info.tileLength - 1) / info.tileLength)) {
            error = "Tile layout does not match the image size.";
            return false;
        }
        for (size_t i = 0; i < info.tileOffsets.size(); ++i)
            if (uint64_t(info.tileOffsets[i]) + info.tileBytes[i] > fileSize) {
                error = "A lossless tile lies outside the file (truncated file?).";
                return false;
            }
    }
    return true;
}

} // namespace apertar::playback
