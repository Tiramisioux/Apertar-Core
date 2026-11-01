#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

namespace apertar {

// A clip-scoped latch: notify once, not once per frame or per dropped frame.
// droppedFrames() is the take's running total of every kind of drop (the UIs show it in place of the "!").
class RecordingDropWarning {
public:
    explicit RecordingDropWarning(std::function<void(bool)> changed)
        : changed_(std::move(changed)) {}

    bool dropped() const { return dropped_.load(std::memory_order_acquire); }
    uint64_t droppedFrames() const { return frames_.load(std::memory_order_acquire); }
    // A storage pre-roll is not a take: while muted, drops are not latched and nobody is told.
    void setMuted(bool muted) { muted_.store(muted, std::memory_order_release); }
    void markDropped(uint64_t frames = 1) {
        if (muted_.load(std::memory_order_acquire))
            return;
        frames_.fetch_add(frames, std::memory_order_acq_rel);
        if (!dropped_.exchange(true, std::memory_order_acq_rel) && changed_)
            changed_(true);
    }
    void reset() {
        frames_.store(0, std::memory_order_release);
        if (dropped_.exchange(false, std::memory_order_acq_rel) && changed_)
            changed_(false);
    }

private:
    std::atomic<bool> dropped_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<bool> muted_{false};
    const std::function<void(bool)> changed_;
};

} // namespace apertar
