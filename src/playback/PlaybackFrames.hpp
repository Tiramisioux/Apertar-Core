#pragma once

// Apertar playback, T48: one frame of a cDNG clip as a JPEG (design §4.4, §4.7). Written for Core;
// the host test is working/tools/playback/frames_test.cpp.
//
// What happens for a request: the number n -> the file at or before it that has bytes -> a cache hit,
// or a decode slot (none free: busy, the route answers 503), the DNG read in chunks that stop for a take
// (the yield window), rendered with the touch UI's look, the touch UI's histogram, RGB -> JPEG.

#include "ClipFrames.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace apertar::playback {

struct FrameOut {
    int status = 200;               // 200, 404 (no such frame), 409 (held), 500 (decode failed), 503 (busy or no JPEG support)
    bool busy = false;
    std::string error;
    std::vector<uint8_t> jpeg;
    uint64_t number = 0;            // the frame number served (<= the request)
    uint64_t index = 0;             // its 0-based position among the frame files
    uint64_t count = 0;             // the number of frame files
    std::string histogram;          // 48 integers 0-1000, comma separated
    bool fromCache = false;
    double decodeMs = 0.0;
};

// The widths the route accepts, so the cache stays small: cards 320/480, playing 640/960, paused 1280/1920.
bool allowedFrameWidth(uint32_t width);

// `stop` is asked before every chunk of the read: true = a take is starting, give the card back.
FrameOut frameJpeg(const std::string &clipId, const std::filesystem::path &dir, const FrameList &frames, uint64_t number,
                   uint32_t width, const std::string &token, const std::function<bool()> &stop);

// From the media watcher, once a second: give idle slots' memory back (30 s idle, or at once in a hold).
size_t trimIdleFrameSlots(int64_t nowMs, bool hold);

} // namespace apertar::playback
