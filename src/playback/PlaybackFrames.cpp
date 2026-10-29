// Apertar playback, T48: see PlaybackFrames.hpp.

#include "PlaybackFrames.hpp"

#include "DecodeSlots.hpp"
#include "DngFrameReader.hpp"
#include "RawRender.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#if APERTARCORE_HAS_TURBOJPEG
#include <turbojpeg.h>
#elif APERTARCORE_HAS_JPEG
#include <cstdio>
#include <jpeglib.h>
#endif

namespace apertar::playback {
namespace {

struct CachedFrame {
    std::vector<uint8_t> jpeg;
    uint64_t number = 0, index = 0;
    std::string histogram;
};

// A byte-limited LRU: thumbnails (w <= 480) 8 MB, play and paused pictures 24 MB (design §4.7).
class JpegCache {
public:
    explicit JpegCache(size_t limitBytes) : limit_(limitBytes) {}
    bool get(const std::string &key, CachedFrame &out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = map_.find(key);
        if (it == map_.end()) return false;
        lru_.splice(lru_.begin(), lru_, it->second);
        out = it->second->second;
        return true;
    }
    void put(const std::string &key, CachedFrame frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t size = frame.jpeg.size();
        if (size > limit_) return;
        if (const auto it = map_.find(key); it != map_.end()) {
            bytes_ -= it->second->second.jpeg.size();
            lru_.erase(it->second);
            map_.erase(it);
        }
        lru_.emplace_front(key, std::move(frame));
        map_[key] = lru_.begin();
        bytes_ += size;
        while (bytes_ > limit_ && !lru_.empty()) {
            bytes_ -= lru_.back().second.jpeg.size();
            map_.erase(lru_.back().first);
            lru_.pop_back();
        }
    }
private:
    std::mutex mutex_;
    size_t limit_, bytes_ = 0;
    std::list<std::pair<std::string, CachedFrame>> lru_;
    std::unordered_map<std::string, std::list<std::pair<std::string, CachedFrame>>::iterator> map_;
};

JpegCache &thumbCache() { static JpegCache cache(8u << 20); return cache; }
JpegCache &playCache() { static JpegCache cache(24u << 20); return cache; }

DecodeSlots &slots()
{
    static DecodeSlots s([] {
        const char *env = std::getenv("APERTAR_PLAYBACK_WORKERS");
        const int n = env ? std::atoi(env) : 2;
        return std::clamp(n, 1, 4);
    }());
    return s;
}

bool encodeRgbJpeg(const std::vector<uint8_t> &rgb, uint32_t width, uint32_t height, int quality, std::vector<uint8_t> &out)
{
#if APERTARCORE_HAS_TURBOJPEG
    thread_local tjhandle handle = tjInitCompress();
    if (!handle) return false;
    const unsigned long bound = tjBufSize(int(width), int(height), TJSAMP_420);
    out.resize(bound);
    unsigned char *buffer = out.data();
    unsigned long size = bound;
    const int rc = tjCompress2(handle, rgb.data(), int(width), 0, int(height), TJPF_RGB, &buffer, &size, TJSAMP_420, quality,
                               TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
    if (rc != 0 || size == 0) { out.clear(); return false; }
    out.resize(size);
    return true;
#elif APERTARCORE_HAS_JPEG
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char *compressed = nullptr;
    unsigned long compressedSize = 0;
    jpeg_mem_dest(&cinfo, &compressed, &compressedSize);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    const size_t stride = size_t(width) * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<uint8_t *>(rgb.data()) + size_t(cinfo.next_scanline) * stride;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    out.assign(compressed, compressed + compressedSize);
    jpeg_destroy_compress(&cinfo);
    std::free(compressed);
    return true;
#else
    (void)rgb; (void)width; (void)height; (void)quality; (void)out;
    return false;
#endif
}

constexpr bool kHaveJpeg =
#if APERTARCORE_HAS_TURBOJPEG || APERTARCORE_HAS_JPEG
    true;
#else
    false;
#endif

// Decoding runs on the HTTP client's own thread (one per connection); play quietly beside the take's writers.
void niceThread()
{
    thread_local bool done = false;
    if (done) return;
    done = true;
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
}

} // namespace

bool allowedFrameWidth(uint32_t width)
{
    return width == 320 || width == 480 || width == 640 || width == 960 || width == 1280 || width == 1920;
}

FrameOut frameJpeg(const std::string &clipId, const std::filesystem::path &dir, const FrameList &frames, uint64_t number,
                   uint32_t width, const std::string &token, const std::function<bool()> &stop)
{
    FrameOut out;
    out.count = frames.count();
    if (!kHaveJpeg) {
        out.status = 503;
        out.error = "JPEG support was not compiled in.";
        return out;
    }
    if (frames.files.empty() || number < frames.first || number > frames.last) {
        out.status = 404;
        out.error = "Frame not found.";
        return out;
    }
    // The picture at or before the number: over a hole, or a 0-byte file, the frame before it (the viewer draws
    // those black itself, R2; thumbnails and the touch screen's rule hold the last picture).
    const size_t position = frames.positionAtNumber(number);
    const size_t file = frames.pictureAtPosition(position);
    if (file >= frames.files.size()) {
        out.status = 404;
        out.error = "No frame at or before this number has a picture.";
        return out;
    }
    const FrameFile &frame = frames.files[file];
    out.number = frame.number;
    out.index = file;

    const std::string key = clipId + '|' + std::to_string(frame.number) + '|' + std::to_string(width) + '|' + token;
    JpegCache &cache = width <= 480 ? thumbCache() : playCache();
    CachedFrame cached;
    if (cache.get(key, cached)) {
        out.jpeg = std::move(cached.jpeg);
        out.histogram = std::move(cached.histogram);
        out.fromCache = true;
        return out;
    }

    DecodeSlots::Lease slot = slots().tryTake();
    if (!slot) {
        out.status = 503;
        out.busy = true;
        out.error = "Busy.";
        return out;
    }
    niceThread();
    const auto started = std::chrono::steady_clock::now();
    const int fd = openDngFrame((dir / frame.name).string().c_str());
    if (fd < 0) {
        out.status = 404;
        out.error = "The frame file is gone.";
        return out;
    }
    DngInfo info;
    std::string error;
    ReadOptions options;
    options.stop = stop;
    ReadStatus status = ReadStatus::Failed;
    if (readDngInfo(fd, info, error))
        status = readDngSamples(fd, info, slot->samples, slot->scratch, error, options);
    closeDngFrame(fd);
    if (status == ReadStatus::Stopped) {
        out.status = 409;
        out.error = "Playback is held.";
        return out;
    }
    if (status != ReadStatus::Ok) {
        out.status = 500;
        out.error = "Decode failed: " + error;
        return out;
    }
    const Rgb8Image image = renderPreview(info, slot->samples, width);
    const std::array<float, 48> bins = rawHistogram(info, slot->samples);
    std::ostringstream hist;
    for (size_t i = 0; i < bins.size(); ++i)
        hist << (i ? "," : "") << int(std::lround(std::clamp(bins[i], 0.0f, 1.0f) * 1000.0f));
    out.histogram = hist.str();
    if (!encodeRgbJpeg(image.rgb, image.width, image.height, width <= 480 ? 75 : 80, out.jpeg)) {
        out.status = 500;
        out.error = "JPEG encoding failed.";
        return out;
    }
    out.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    cache.put(key, CachedFrame{out.jpeg, out.number, out.index, out.histogram});
    return out;
}

size_t trimIdleFrameSlots(int64_t nowMs, bool hold)
{
    return slots().trimIdle(nowMs, 30000, hold);
}

} // namespace apertar::playback
