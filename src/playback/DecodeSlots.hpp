#pragma once

// Apertar playback, design sketch v2 (not in any repository).
// Decode slots that own their buffers (critic M5).
//
// Core's web server runs every connection on a new detached thread (WebServer.cpp:276-287), so a
// buffer that belongs to the thread or to the call is allocated and faulted in again for every
// frame: on this host a 16-bit 4K frame took 40.4 ms that way against 24.5 ms with kept buffers
// (critic A.3). Here the buffers belong to the slot; a request borrows a slot with try_lock and
// answers 503 when none is free (Cinemate's "refuse rather than queue", playback.py:57-62).
// trimIdle() is called from Core's media watcher loop, which already wakes every second
// (ApertarCoreApp.cpp:9383): after 30 s without a frame, or when a hold starts, the slots give
// their memory back. No new thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace apertar::playback {

struct DecodeSlot {
    std::mutex busy;
    std::vector<uint16_t> samples;   // up to 4096 x 3072 x 2 B = 25 MB (4K: 16.6 MB)
    std::vector<uint8_t> scratch;    // one chunk of rows or one lossless tile (~1-2 MB)
    std::vector<uint8_t> rgb;        // the rendered picture before JPEG (1920 w: ~6 MB)
    std::atomic<int64_t> lastUsedMs{0};

    size_t bytesHeld() const
    {
        return samples.capacity() * sizeof(uint16_t) + scratch.capacity() + rgb.capacity();
    }
    void release()
    {
        std::vector<uint16_t>().swap(samples);
        std::vector<uint8_t>().swap(scratch);
        std::vector<uint8_t>().swap(rgb);
    }
};

inline int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class DecodeSlots {
public:
    explicit DecodeSlots(int count)
    {
        for (int i = 0; i < count; ++i) slots_.push_back(std::make_unique<DecodeSlot>());
    }

    // RAII lease: empty when every slot is busy (the route answers 503).
    class Lease {
    public:
        Lease() = default;
        explicit Lease(DecodeSlot *slot) : slot_(slot) {}
        Lease(Lease &&o) noexcept : slot_(o.slot_) { o.slot_ = nullptr; }
        Lease &operator=(Lease &&o) noexcept { reset(); slot_ = o.slot_; o.slot_ = nullptr; return *this; }
        ~Lease() { reset(); }
        explicit operator bool() const { return slot_ != nullptr; }
        DecodeSlot *operator->() const { return slot_; }
        DecodeSlot &operator*() const { return *slot_; }
    private:
        void reset()
        {
            if (!slot_) return;
            slot_->lastUsedMs.store(steadyMs(), std::memory_order_relaxed);
            slot_->busy.unlock();
            slot_ = nullptr;
        }
        DecodeSlot *slot_ = nullptr;
    };

    Lease tryTake()
    {
        for (auto &slot : slots_)
            if (slot->busy.try_lock()) return Lease(slot.get());
        return Lease();
    }

    // From the media watcher, once a second: free idle slots' buffers. A slot in use is skipped.
    size_t trimIdle(int64_t nowMs, int64_t idleMs, bool holdActive)
    {
        size_t freed = 0;
        for (auto &slot : slots_) {
            if (!slot->busy.try_lock()) continue;
            std::lock_guard<std::mutex> lock(slot->busy, std::adopt_lock);
            if (slot->bytesHeld() == 0) continue;
            if (holdActive || nowMs - slot->lastUsedMs.load(std::memory_order_relaxed) >= idleMs) {
                freed += slot->bytesHeld();
                slot->release();
            }
        }
        return freed;
    }

    size_t bytesHeld()
    {
        size_t total = 0;
        for (auto &slot : slots_) {
            std::lock_guard<std::mutex> lock(slot->busy);
            total += slot->bytesHeld();
        }
        return total;
    }

private:
    std::vector<std::unique_ptr<DecodeSlot>> slots_;
};

} // namespace apertar::playback
