#pragma once

#include "hardware/ButtonGesture.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apertar {

class GpioInput {
public:
    using ActionCallback = std::function<void(const std::string &)>;

    explicit GpioInput(ActionCallback actionCallback, ActionCallback rotationCallback = {});
    ~GpioInput();

    bool start();
    void stop();

    bool enabled() const;
    std::string configPath() const;
    std::string lastError() const;

private:
    friend struct HardwareInputTestAccess;
    struct ButtonMapping {
        int gpio = -1;
        std::string action;       // press (dN)
        std::string single;       // dN_single, dN_double, dN_triple, dN_hold: counted like Cinemate (ButtonGesture)
        std::string doubleClick;
        std::string triple;
        std::string hold;
    };

    struct EncoderMapping {
        int gpioA = -1;
        int gpioB = -1;
        int pressGpio = -1;
        std::string clockwise;
        std::string counterClockwise;
        std::string press;
        bool invertDirection = false;
        int stepsPerEvent = 4;
    };

    struct Config {
        bool configured = false;
        bool enabled = false;
        std::string chipPath = "auto";
        bool activeLow = true;
        std::string pull = "up";
        int pollMs = 12;
        int debounceMs = 60;
        std::vector<ButtonMapping> buttons;
        std::vector<EncoderMapping> encoders;
        std::string configPath;
    };

    struct RuntimeButton {
        int lineIndex = -1;
        int gpio = -1;
        std::string action;
        std::string single;
        std::string doubleClick;
        std::string triple;
        std::string hold;
        bool observedPressed = false;
        bool stablePressed = false;
        bool haveSample = false;
        bool inverted = false;    // read pressed at start: wired the other way round (a 1-0-1 button)
        std::chrono::steady_clock::time_point observedChangedAt{};
        hardware::ButtonGesture gesture;
    };

    struct RuntimeEncoder {
        int lineAIndex = -1;
        int lineBIndex = -1;
        std::string clockwise;
        std::string counterClockwise;
        bool invertDirection = false;
        int stepsPerEvent = 4;
        int lastState = -1;
        int accumulatedSteps = 0;
    };

    static Config loadConfig();
    void run();
    bool requestLines(const std::vector<int> &offsets, bool edges, int *lineFd, std::string *errorMessage) const;
    bool requestLinesFromChip(const std::string &chipPath,
                              const std::vector<int> &offsets,
                              bool edges,
                              int *lineFd,
                              std::string *errorMessage) const;
    bool readLineValues(int lineFd, size_t lineCount, uint64_t *bits) const;
    void closeFd(int *fd) const;
    void emitAction(const std::string &action);
    void setLastError(const std::string &error);

    ActionCallback actionCallback_;
    ActionCallback rotationCallback_;
    Config config_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex stateMutex_;
    std::string lastError_;
};

} // namespace apertar
