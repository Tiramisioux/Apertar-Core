#pragma once
// Press, single, double and triple click, and hold for one button, the way Cinemate's SmartButton counts them
// (cinemate src/module/gpio_input.py): Press fires on every press, at once. Releases are counted as clicks and
// read 0.5 s after the last one: 1 is single, 2 double, 3 or more triple. A press held 0.5 s is no click and
// clears the count; one held 3 s fires Hold. Pure: fed with the debounced state and a millisecond clock, on every
// edge and again at nextWakeMs().
#include <cstdint>
#include <vector>

namespace apertar::hardware {

class ButtonGesture {
public:
    enum class Event { Press, Single, Double, Triple, Hold };

    static constexpr int64_t kClickWindowMs = 500;
    static constexpr int64_t kLongPressMs = 500;
    static constexpr int64_t kHoldMs = 3000;

    // Call on every poll; returns what fired since the last call.
    std::vector<Event> update(bool pressed, int64_t nowMs)
    {
        std::vector<Event> events;
        if (pressed && !down_) {
            down_ = true;
            downAtMs_ = nowMs;
            longPress_ = false;
            holdFired_ = false;
            events.push_back(Event::Press);
        } else if (!pressed && down_) {
            down_ = false;
            // Measured here too: with edge events nothing may have called update() while the button was down.
            if (longPress_ || nowMs - downAtMs_ >= kLongPressMs)
                clicks_ = 0;
            else
                ++clicks_;
            lastReleaseMs_ = nowMs;
        }
        if (down_) {
            if (!longPress_ && nowMs - downAtMs_ >= kLongPressMs) {
                longPress_ = true;
                clicks_ = 0;
            }
            if (!holdFired_ && nowMs - downAtMs_ >= kHoldMs) {
                holdFired_ = true;
                events.push_back(Event::Hold);
            }
        } else if (clicks_ > 0 && nowMs - lastReleaseMs_ >= kClickWindowMs) {
            events.push_back(clicks_ == 1 ? Event::Single : clicks_ == 2 ? Event::Double : Event::Triple);
            clicks_ = 0;
        }
        return events;
    }

    // When update() next has something to do with no edge in between (a long press, Hold, clicks to read out),
    // or -1 when it can wait for the next edge.
    int64_t nextWakeMs() const
    {
        if (down_) {
            if (!longPress_)
                return downAtMs_ + kLongPressMs;
            return holdFired_ ? -1 : downAtMs_ + kHoldMs;
        }
        return clicks_ > 0 ? lastReleaseMs_ + kClickWindowMs : -1;
    }

private:
    bool down_ = false;
    bool longPress_ = false;
    bool holdFired_ = false;
    int clicks_ = 0;
    int64_t downAtMs_ = 0;
    int64_t lastReleaseMs_ = 0;
};

} // namespace apertar::hardware
