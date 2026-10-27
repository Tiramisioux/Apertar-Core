#pragma once

#include <apertar/core/Types.hpp>
#include "RecordingClock.hpp"
#include "TimestampedAudioCapture.hpp"
#include "util/RecordingFailure.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace apertar {

class AudioRecordingSession {
public:
    using AudioLevelCallback = std::function<void(int)>;

    AudioRecordingSession() = default;
    ~AudioRecordingSession();

    bool start(const std::filesystem::path &clipFolder,
               const AudioRecordingOptions &options,
               AudioLevelCallback levelCallback,
               std::string *error,
               const RecordingClock *clock = nullptr);
    void observeVideoFrame(uint64_t sensorBootUs, int64_t frameDurationUs);
    // finalize=false: the medium is gone; the WAV is not written and the raw PCM
    // capture file is left where it is.
    void stop(bool finalize = true);

    bool active() const;
    std::string error() const { return failure_.message(); }

private:
    void readerLoop();
    void writerLoop();
    void enqueueWrite(const char *data, size_t size);  // data == nullptr writes zeros
    void finalizeWav();
    void resetState();
    void emitLevel(int level);
    static void writeWavHeader(std::ofstream &stream,
                               uint32_t sampleRate,
                               uint16_t channels,
                               uint16_t bitsPerSample,
                               bool floatingPoint,
                               uint64_t dataSize,
                               uint16_t firstChannel,
                               uint64_t clipStartWallClockUs,
                               const std::string &originatorReference,
                               TimecodeMode timecodeMode,
                               double fps);

    AudioRecordingOptions options_{};
    TimestampedAudioCapture capture_;
    RecordingClock recordingClock_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> sessionActive_{false};
    RecordingFailure failure_;
    std::filesystem::path clipFolder_;
    std::filesystem::path rawPath_;
    std::filesystem::path wavPath_;
    std::ofstream rawStream_;
    AudioLevelCallback levelCallback_;

    std::thread readerThread_;
    // The reader never touches the medium: it hands blocks to this queue and the
    // writer thread drains them into rawStream_ (Cinemate ffb61fd: an exFAT
    // write() under DNG pressure blocked the capture thread for hundreds of ms
    // and the ALSA ring overran).
    std::thread writerThread_;
    std::mutex writeMutex_;
    std::condition_variable writeCv_;
    std::deque<std::vector<char>> writeQueue_;
    size_t queuedWriteBytes_ = 0;
    size_t peakQueuedWriteBytes_ = 0;
    bool writerStop_ = false;
    std::atomic<bool> writeFailed_{false};
    std::atomic<bool> running_{false};
    std::atomic<pid_t> playbackPid_{-1};
    int playbackWriteFd_ = -1;
    int playbackStderrFd_ = -1;

    mutable std::mutex syncMutex_;
    uint64_t audioStartBootUs_ = 0;
    uint64_t videoFirstBootUs_ = 0;
    uint64_t videoLastBootUs_ = 0;
    int64_t videoLastDurationUs_ = 0;
    uint64_t videoEndBootUs_ = 0;
    uint64_t capturedBytes_ = 0;
    uint64_t audioEndBootUs_ = 0;
    // Least-squares fit of samples captured against each block's BOOTTIME stamp,
    // for the clock-drift estimate (see finalizeWav).
    int64_t fitOriginBootUs_ = 0;
    uint64_t fitFrames_ = 0;
    uint64_t fitPoints_ = 0;
    double fitSumT_ = 0, fitSumN_ = 0, fitSumTT_ = 0, fitSumTN_ = 0;
};

} // namespace apertar
