#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apertar {

struct AudioCaptureMode {
    std::string format;
    unsigned rate = 0;
    unsigned channels = 0;
    int nativeFormat = 0;
};

// ALSA is loaded only when recording audio; preview/boot has no new dependency load.
class TimestampedAudioCapture {
public:
    TimestampedAudioCapture();
    ~TimestampedAudioCapture();
    bool open(const std::string &device, unsigned rate, unsigned channels, std::string *error);
    // bufferUs/periodUs: the ALSA ring and wake-up interval. The record path asks
    // for a deep ring (storage stalls must not overrun it) with a short period
    // (VU meter, timestamp granularity); the two are independent only when the
    // hardware parameters are set by hand, see open().
    bool open(const std::string &device, AudioCaptureMode &mode, unsigned firstChannel, std::string *error,
              unsigned bufferUs = 100000, unsigned periodUs = 10000);
    static std::vector<AudioCaptureMode> probe(const std::string &device, std::string *error);
    int read(void *data, size_t frames, int64_t *firstSampleBootUs, std::string *error);
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apertar
