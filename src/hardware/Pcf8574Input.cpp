#include "hardware/Pcf8574Input.hpp"
#include "hardware/ActionCatalog.hpp"
#include "hardware/ButtonActionList.hpp"
#include "hardware/ButtonGesture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace apertar {
namespace {

std::string trimAscii(std::string value)
{
    const auto notSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalizeToken(std::string value)
{
    value = lowerAscii(trimAscii(value));
    for (char &ch : value) {
        if (ch == '-' || ch == ' ' || ch == '.')
            ch = '_';
    }
    return value;
}

std::filesystem::path defaultHardwareConfigPath()
{
    return "/home/pi/apertar-hardware.conf";
}

std::vector<std::filesystem::path> hardwareConfigCandidatePaths()
{
    std::vector<std::filesystem::path> candidates;
    if (const char *envPath = std::getenv("APERTAR_HARDWARE_CONFIG"); envPath && *envPath)
        candidates.emplace_back(envPath);

    candidates.push_back(defaultHardwareConfigPath());
    candidates.emplace_back("/boot/firmware/apertar-hardware.conf");
    candidates.emplace_back("/boot/apertar-hardware.conf");

    std::vector<std::filesystem::path> unique;
    for (const auto &path : candidates) {
        if (!path.empty() && std::find(unique.begin(), unique.end(), path) == unique.end())
            unique.push_back(path);
    }
    return unique;
}

std::optional<int> parseConfigInteger(const std::string &text)
{
    const std::string trimmed = trimAscii(text);
    if (trimmed.empty())
        return std::nullopt;

    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(trimmed.c_str(), &end, 0);
    if (errno != 0 || end == trimmed.c_str())
        return std::nullopt;

    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end)))
            return std::nullopt;
        ++end;
    }
    return static_cast<int>(value);
}

bool parseConfigBool(const std::string &text, bool fallback)
{
    const std::string normalized = normalizeToken(text);
    if (normalized == "true" || normalized == "yes" || normalized == "on" || normalized == "1")
        return true;
    if (normalized == "false" || normalized == "no" || normalized == "off" || normalized == "0")
        return false;
    return fallback;
}

std::optional<int> pinIndexForKey(std::string key)
{
    key = normalizeToken(std::move(key));
    if (key.size() > 7 && key.substr(key.size() - 7) == "_action")
        key.resize(key.size() - 7);
    if (key.size() > 9 && key.substr(key.size() - 9) == "_function")
        key.resize(key.size() - 9);

    std::string digits;
    if (key.rfind("pin_", 0) == 0)
        digits = key.substr(4);
    else if (key.rfind("pin", 0) == 0)
        digits = key.substr(3);
    else if (key.rfind("gpio_", 0) == 0)
        digits = key.substr(5);
    else if (key.rfind("gpio", 0) == 0)
        digits = key.substr(4);
    else if (key.size() == 2 && key[0] == 'p' && std::isdigit(static_cast<unsigned char>(key[1])))
        digits = key.substr(1);

    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        return std::nullopt;
    }

    const int pin = std::stoi(digits);
    if (pin < 0 || pin > 7)
        return std::nullopt;
    return pin;
}

std::string normalizeAction(std::string action)
{
    return hardware::normalizeButtonMapping(action, hardware::actions::canonicalAction);
}

bool pinMapped(const std::array<std::string, 8> &actions, const std::array<std::array<std::string, 4>, 8> &gestureActions, int pin)
{
    return !actions[pin].empty() || std::any_of(gestureActions[pin].begin(), gestureActions[pin].end(),
                                                [](const std::string &action) { return !action.empty(); });
}

bool hasMappedAction(const std::array<std::string, 8> &actions, const std::array<std::array<std::string, 4>, 8> &gestureActions)
{
    for (int pin = 0; pin < 8; ++pin)
        if (pinMapped(actions, gestureActions, pin))
            return true;
    return false;
}

constexpr std::array<const char *, 4> kGestureSuffixes{"_single", "_double", "_triple", "_hold"};

std::string i2cDevicePath(int busNumber)
{
    return "/dev/i2c-" + std::to_string(busNumber);
}

} // namespace

Pcf8574Input::Pcf8574Input(ActionCallback actionCallback)
    : actionCallback_(std::move(actionCallback))
{
}

Pcf8574Input::~Pcf8574Input()
{
    stop();
}

bool Pcf8574Input::start()
{
    stop();
    config_ = loadConfig();
    if (!config_.configured || !config_.enabled)
        return false;

    if (!hasMappedAction(config_.actions, config_.gestureActions)) {
        std::cerr << "PCF8574 config in " << config_.configPath
                  << " has no pin actions; input disabled.\n";
        return false;
    }

    running_ = true;
    thread_ = std::thread(&Pcf8574Input::run, this);
    std::cerr << "PCF8574 input enabled: " << i2cDevicePath(config_.busNumber)
              << " address=0x" << std::hex << config_.address << std::dec
              << " config=" << config_.configPath << "\n";
    return true;
}

void Pcf8574Input::stop()
{
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

bool Pcf8574Input::enabled() const
{
    return config_.configured && config_.enabled;
}

std::string Pcf8574Input::configPath() const
{
    return config_.configPath;
}

std::string Pcf8574Input::lastError() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastError_;
}

Pcf8574Input::Config Pcf8574Input::loadConfig()
{
    Config config;
    std::error_code ec;

    for (const auto &path : hardwareConfigCandidatePaths()) {
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec))
            continue;

        std::ifstream input(path);
        if (!input.is_open())
            continue;

        bool inPcfSection = false;
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimAscii(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
                continue;

            const auto comment = trimmed.find_first_of("#;");
            if (comment != std::string::npos)
                trimmed = trimAscii(trimmed.substr(0, comment));
            if (trimmed.empty())
                continue;

            if (trimmed.front() == '[' && trimmed.back() == ']') {
                const std::string section = lowerAscii(trimAscii(trimmed.substr(1, trimmed.size() - 2)));
                inPcfSection = section == "pcf8574" || section == "pcf8574_buttons";
                if (inPcfSection) {
                    config.configured = true;
                    config.enabled = true;
                    config.configPath = path.string();
                }
                continue;
            }

            if (!inPcfSection)
                continue;

            const std::size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = lowerAscii(trimAscii(trimmed.substr(0, eq)));
            const std::string value = trimAscii(trimmed.substr(eq + 1));
            if (key == "enabled") {
                config.enabled = parseConfigBool(value, config.enabled);
            } else if (key == "bus" || key == "channel" || key == "i2c_bus") {
                if (const auto parsed = parseConfigInteger(value); parsed && *parsed >= 0 && *parsed <= 255)
                    config.busNumber = *parsed;
            } else if (key == "address" || key == "addr") {
                if (const auto parsed = parseConfigInteger(value); parsed && *parsed >= 0x03 && *parsed <= 0x77)
                    config.address = *parsed;
            } else if (key == "poll_ms" || key == "poll_interval_ms") {
                if (const auto parsed = parseConfigInteger(value))
                    config.pollMs = std::clamp(*parsed, 10, 1000);
            } else if (key == "debounce_ms") {
                if (const auto parsed = parseConfigInteger(value))
                    config.debounceMs = std::clamp(*parsed, 0, 1000);
            } else if (key == "active_low") {
                config.activeLow = parseConfigBool(value, config.activeLow);
            } else {
                std::string base = key;
                int gesture = -1;
                for (size_t i = 0; i < kGestureSuffixes.size(); ++i) {
                    const std::string suffix = kGestureSuffixes[i];
                    if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        base.resize(base.size() - suffix.size());
                        gesture = static_cast<int>(i);
                        break;
                    }
                }
                if (const auto pin = pinIndexForKey(base)) {
                    if (gesture < 0)
                        config.actions[*pin] = normalizeAction(value);
                    else
                        config.gestureActions[*pin][static_cast<size_t>(gesture)] = normalizeAction(value);
                }
            }
        }

        if (config.configured)
            return config;
    }

    return config;
}

bool Pcf8574Input::openDevice(int *fd, std::string *errorMessage) const
{
    if (!fd)
        return false;

    const std::string path = i2cDevicePath(config_.busNumber);
    *fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (*fd < 0) {
        if (errorMessage)
            *errorMessage = path + ": " + std::strerror(errno);
        return false;
    }

    if (::ioctl(*fd, I2C_SLAVE, config_.address) < 0) {
        if (errorMessage)
            *errorMessage = "PCF8574 address 0x" + [&] {
                std::ostringstream out;
                out << std::hex << config_.address;
                return out.str();
            }() + ": " + std::strerror(errno);
        closeDevice(fd);
        return false;
    }

    if (!configureAsInputs(*fd)) {
        if (errorMessage)
            *errorMessage = "Could not configure PCF8574 pins as inputs: " + std::string(std::strerror(errno));
        closeDevice(fd);
        return false;
    }

    return true;
}

bool Pcf8574Input::configureAsInputs(int fd) const
{
    const uint8_t inputLatch = 0xff;
    return ::write(fd, &inputLatch, 1) == 1;
}

bool Pcf8574Input::readByte(int fd, uint8_t *value) const
{
    if (!value)
        return false;

    uint8_t byte = 0;
    if (::read(fd, &byte, 1) != 1)
        return false;

    *value = byte;
    return true;
}

void Pcf8574Input::closeDevice(int *fd) const
{
    if (fd && *fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

void Pcf8574Input::emitAction(const std::string &action)
{
    if (action.empty() || !actionCallback_)
        return;
    actionCallback_(action);
}

void Pcf8574Input::setLastError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (lastError_ == error)
        return;
    lastError_ = error;
    if (!error.empty())
        std::cerr << "PCF8574 input: " << error << "\n";
}

void Pcf8574Input::run()
{
    int fd = -1;
    uint8_t observedPressedMask = 0;
    uint8_t stablePressedMask = 0;
    uint8_t invertedMask = 0;
    auto observedChangedAt = std::chrono::steady_clock::now();
    bool haveSample = false;
    std::array<hardware::ButtonGesture, 8> gestures{};

    while (running_.load(std::memory_order_relaxed)) {
        if (fd < 0) {
            std::string errorMessage;
            if (!openDevice(&fd, &errorMessage)) {
                setLastError(errorMessage);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            setLastError({});
            haveSample = false;
            gestures = {};
        }

        uint8_t raw = 0xff;
        if (!readByte(fd, &raw)) {
            setLastError("read failed: " + std::string(std::strerror(errno)));
            closeDevice(&fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        const uint8_t levelMask = config_.activeLow ? static_cast<uint8_t>(~raw) : raw;
        if (!haveSample) {
            // Like the GPIO buttons (Cinemate): a mapped pin that reads pressed at start is wired the other way round
            // (a 1-0-1, normally closed button), so its levels are swapped from now on.
            invertedMask = 0;
            for (int pin = 0; pin < 8; ++pin) {
                if ((levelMask & (1u << pin)) && pinMapped(config_.actions, config_.gestureActions, pin)) {
                    invertedMask |= static_cast<uint8_t>(1u << pin);
                    std::cerr << "PCF8574 input: pin " << pin << " reads pressed at start; taken as inverted (a 1-0-1 button).\n";
                }
            }
        }
        const uint8_t pressedMask = static_cast<uint8_t>(levelMask ^ invertedMask);
        const auto now = std::chrono::steady_clock::now();
        if (!haveSample) {
            observedPressedMask = pressedMask;
            stablePressedMask = pressedMask;
            observedChangedAt = now;
            haveSample = true;
        } else if (pressedMask != observedPressedMask) {
            observedPressedMask = pressedMask;
            observedChangedAt = now;
        } else {
            const auto stableForMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - observedChangedAt).count();
            if (stableForMs >= config_.debounceMs && observedPressedMask != stablePressedMask)
                stablePressedMask = observedPressedMask;
        }

        // Press at once; single, double and triple click and hold counted like the GPIO buttons (ButtonGesture).
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        for (int pin = 0; pin < 8; ++pin) {
            const auto &more = config_.gestureActions[pin];
            for (const auto event : gestures[pin].update((stablePressedMask & (1u << pin)) != 0, nowMs)) {
                if (seenCallback_) {
                    static const char *const suffixes[] = {"", "_single", "_double", "_triple", "_hold"};
                    seenCallback_("pcf8574|pin" + std::to_string(pin) + suffixes[static_cast<size_t>(event)]);
                }
                switch (event) {
                case hardware::ButtonGesture::Event::Press: emitAction(config_.actions[pin]); break;
                case hardware::ButtonGesture::Event::Single: emitAction(more[0]); break;
                case hardware::ButtonGesture::Event::Double: emitAction(more[1]); break;
                case hardware::ButtonGesture::Event::Triple: emitAction(more[2]); break;
                case hardware::ButtonGesture::Event::Hold: emitAction(more[3]); break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollMs));
    }

    closeDevice(&fd);
}

} // namespace apertar
