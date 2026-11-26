#pragma once
// The lens as a recording sees it: a small plain struct with fixed-size fields, so a frame takes a copy of it with no
// allocation (CdngEncoder::Metadata::lens), and a one-writer / many-readers snapshot (a seqlock over atomic words) the
// lens code publishes to and the capture path reads from without a lock or any I/O. Nothing here knows the lens board.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace apertar {

struct LensMetadata {
    static constexpr uint32_t kFlagHasDistance = 1u;       // distanceCm is known
    static constexpr uint32_t kFlagDistanceInfinite = 2u;  // the focus distance is infinity (the board says 65535 cm)
    static constexpr size_t kModelBytes = 60;

    uint32_t fNumberX100 = 0;      // the commanded f-number x 100 (the iris cannot be read back); 0 = unknown
    uint32_t focalLengthX10 = 0;   // mm x 10; 0 = unknown
    uint32_t distanceCm = 0;       // the focus distance in cm (see the flags)
    uint32_t flags = 0;
    uint32_t focalMinX10 = 0;      // the lens range, mm x 10; 0 = unknown
    uint32_t focalMaxX10 = 0;
    uint32_t apertureMinAtMinX100 = 0;   // the widest f-number at the shortest / longest focal length, x 100; 0 = unknown
    uint32_t apertureMinAtMaxX100 = 0;
    uint32_t lensId = 0;           // the Canon EF lens id the board reports; 0 = none
    char model[kModelBytes] = {};  // NUL terminated; empty = unknown

    bool hasFNumber() const { return fNumberX100 > 0; }
    bool hasFocalLength() const { return focalLengthX10 > 0; }
    bool hasDistance() const { return (flags & kFlagHasDistance) != 0; }
    bool hasModel() const { return model[0] != '\0'; }
    bool hasLensInfo() const { return focalMinX10 > 0 && focalMaxX10 > 0; }
    bool any() const { return hasFNumber() || hasFocalLength() || hasDistance() || hasModel() || hasLensInfo(); }
    std::string_view modelView() const { return std::string_view(model, strnlen(model, kModelBytes)); }
    void setModel(std::string_view text)
    {
        std::memset(model, 0, sizeof(model));
        std::memcpy(model, text.data(), text.size() < kModelBytes - 1 ? text.size() : kModelBytes - 1);
    }
    bool operator==(const LensMetadata &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};
static_assert(sizeof(LensMetadata) % 8 == 0, "LensMetadata is copied as 64-bit words");

namespace lensmeta_detail {
struct Store {
    static constexpr size_t kWords = sizeof(LensMetadata) / 8;
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint64_t> words[kWords];
    Store() { for (auto &word : words) word.store(0, std::memory_order_relaxed); }
};
inline Store &store() { static Store instance; return instance; }
} // namespace lensmeta_detail

// One publisher at a time (the lens code serialises its calls).
inline void publishLensMetadata(const LensMetadata &lens)
{
    using namespace lensmeta_detail;
    Store &s = store();
    uint64_t words[Store::kWords];
    std::memcpy(words, &lens, sizeof(lens));
    const uint32_t seq = s.sequence.load(std::memory_order_relaxed);
    s.sequence.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < Store::kWords; ++i) s.words[i].store(words[i], std::memory_order_relaxed);
    s.sequence.store(seq + 2, std::memory_order_release);
}

// The last published lens, never blocking: a read that keeps meeting a write in progress gives an empty lens (no tags).
inline LensMetadata currentLensMetadata()
{
    using namespace lensmeta_detail;
    Store &s = store();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint32_t before = s.sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        uint64_t words[Store::kWords];
        for (size_t i = 0; i < Store::kWords; ++i) words[i] = s.words[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.sequence.load(std::memory_order_relaxed) != before) continue;
        LensMetadata lens;
        std::memcpy(&lens, words, sizeof(lens));
        return lens;
    }
    return LensMetadata{};
}

} // namespace apertar
