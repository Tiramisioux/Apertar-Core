#pragma once

#include <atomic>
#include <functional>
#include <utility>

namespace apertar {

// A clip-scoped latch: notify once, not once per frame or per dropped frame.
class RecordingDropWarning {
public:
    explicit RecordingDropWarning(std::function<void(bool)> changed)
        : changed_(std::move(changed)) {}

    bool dropped() const { return dropped_.load(std::memory_order_acquire); }
    // A storage pre-roll is not a take: while muted, drops are not latched and nobody is told.
    void setMuted(bool muted) { muted_.store(muted, std::memory_order_release); }
    void markDropped() {
        if (muted_.load(std::memory_order_acquire))
            return;
        if (!dropped_.exchange(true, std::memory_order_acq_rel) && changed_)
            changed_(true);
    }
    void reset() {
        if (dropped_.exchange(false, std::memory_order_acq_rel) && changed_)
            changed_(false);
    }

private:
    std::atomic<bool> dropped_{false};
    std::atomic<bool> muted_{false};
    const std::function<void(bool)> changed_;
};

} // namespace apertar
