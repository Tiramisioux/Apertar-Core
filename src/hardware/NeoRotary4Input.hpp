#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace apertar {

class NeoRotary4Input {
public:
    using ActionCallback = std::function<void(const std::string &)>;

    explicit NeoRotary4Input(ActionCallback actionCallback, ActionCallback rotationCallback = {});
    ~NeoRotary4Input();

    bool start();
    void stop();
    // Called with "neorotary4|encoderN_..." each time a knob turns or its push fires (the WebUI's Listen).
    void setSeenCallback(ActionCallback seenCallback) { seenCallback_ = std::move(seenCallback); }

    bool enabled() const;
    std::string configPath() const;
    std::string lastError() const;

    struct EncoderActions {
        std::string clockwise;
        std::string counterClockwise;
        std::string press;
        std::string single;       // encoderN_single, _double, _triple, _hold: the push counted like Cinemate (ButtonGesture)
        std::string doubleClick;
        std::string triple;
        std::string hold;
    };

private:
    friend struct HardwareInputTestAccess;
    struct Config {
        bool configured = false;
        bool enabled = false;
        int busNumber = 1;
        int address = 0x49;
        int pollMs = 30;
        int debounceMs = 60;
        bool invertDirection = false;
        bool resetOnStart = false;
        int maxEventsPerPoll = 8;
        std::array<EncoderActions, 4> encoders{};
        std::string configPath;
    };

    static Config loadConfig();
    void run();
    bool openDevice(int *fd, std::string *errorMessage) const;
    bool verifyDevice(int fd, std::string *errorMessage) const;
    bool configureButtons(int fd, std::string *errorMessage) const;
    bool writeRegister(int fd, uint8_t base, uint8_t reg, const uint8_t *data, size_t size) const;
    bool readRegister(int fd, uint8_t base, uint8_t reg, uint8_t *data, size_t size, int delayUs = 8000) const;
    bool readEncoderDelta(int fd, int encoder, int32_t *delta) const;
    bool readButtonMask(int fd, uint32_t *mask) const;
    void closeDevice(int *fd) const;
    void emitAction(const std::string &action);
    void setLastError(const std::string &error);

    ActionCallback actionCallback_;
    ActionCallback rotationCallback_;
    ActionCallback seenCallback_;
    Config config_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex stateMutex_;
    std::string lastError_;
};

} // namespace apertar
