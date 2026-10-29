#pragma once
// Press, single, double and triple click, and hold for one button, the way Cinemate's SmartButton counts them
// (cinemate src/module/gpio_input.py): Press fires on every press, at once. Releases are counted as clicks and
// read 0.5 s after the last one: 1 is single, 2 double, 3 or more triple. A press held 0.5 s is no click and
// clears the count; one held 3 s fires Hold. Pure: fed with the debounced state and a millisecond clock.
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
            if (!longPress_)
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

private:
    bool down_ = false;
    bool longPress_ = false;
    bool holdFired_ = false;
    int clicks_ = 0;
    int64_t downAtMs_ = 0;
    int64_t lastReleaseMs_ = 0;
};

} // namespace apertar::hardware
