#include "hardware/GpioInput.hpp"
#include "hardware/ActionCatalog.hpp"
#include "hardware/ButtonActionList.hpp"
#include "hardware/GpioChipInfo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
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

std::string normalizeAction(std::string action)
{
    return hardware::normalizeButtonMapping(action, hardware::actions::canonicalAction);
}

std::optional<int> gpioFromDigitalKey(std::string key)
{
    key = normalizeToken(std::move(key));
    if (key.size() > 7 && key.substr(key.size() - 7) == "_action")
        key.resize(key.size() - 7);
    if (key.size() > 9 && key.substr(key.size() - 9) == "_function")
        key.resize(key.size() - 9);

    std::string digits;
    for (const std::string prefix : { "gpio_", "gpio", "pin_", "pin", "bcm_", "bcm", "d_", "d",
                                     "grove_d_", "grove_d" }) {
        if (key.rfind(prefix, 0) == 0) {
            digits = key.substr(prefix.size());
            break;
        }
    }

    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        return std::nullopt;
    }

    const int gpio = std::stoi(digits);
    if (gpio < 0 || gpio > 63)
        return std::nullopt;
    return gpio;
}

std::optional<int> encoderIndexFromKeyPrefix(const std::string &key)
{
    for (const std::string prefix : { "encoder", "enc", "rotary" }) {
        if (key.rfind(prefix, 0) != 0)
            continue;
        std::string rest = key.substr(prefix.size());
        if (!rest.empty() && rest.front() == '_')
            rest.erase(rest.begin());
        if (rest.empty() || !std::isdigit(static_cast<unsigned char>(rest.front())))
            continue;
        const int encoder = rest.front() - '0';
        if (encoder < 0 || encoder > 7)
            return std::nullopt;
        return encoder;
    }
    return std::nullopt;
}

std::string encoderFieldFromKey(std::string key)
{
    key = normalizeToken(std::move(key));
    for (const std::string prefix : { "encoder", "enc", "rotary" }) {
        if (key.rfind(prefix, 0) != 0)
            continue;
        std::string rest = key.substr(prefix.size());
        if (!rest.empty() && rest.front() == '_')
            rest.erase(rest.begin());
        if (rest.size() < 2 || !std::isdigit(static_cast<unsigned char>(rest.front())))
            continue;
        rest.erase(rest.begin());
        if (!rest.empty() && rest.front() == '_')
            rest.erase(rest.begin());
        return rest;
    }
    return {};
}

int8_t quadratureDelta(int previousState, int currentState)
{
    static constexpr std::array<int8_t, 16> table{
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0
    };
    if (previousState < 0 || previousState > 3 || currentState < 0 || currentState > 3)
        return 0;
    return table[static_cast<size_t>((previousState << 2) | currentState)];
}

} // namespace

GpioInput::GpioInput(ActionCallback actionCallback, ActionCallback rotationCallback)
    : actionCallback_(std::move(actionCallback))
    , rotationCallback_(rotationCallback ? std::move(rotationCallback) : actionCallback_)
{
}

GpioInput::~GpioInput()
{
    stop();
}

bool GpioInput::start()
{
    stop();
    config_ = loadConfig();
    if (!config_.configured || !config_.enabled)
        return false;

    if (config_.buttons.empty() && config_.encoders.empty()) {
        std::cerr << "GPIO input config in " << config_.configPath
                  << " has no buttons or encoders; input disabled.\n";
        return false;
    }

    running_ = true;
    thread_ = std::thread(&GpioInput::run, this);
    std::cerr << "GPIO input enabled: chip=" << config_.chipPath
              << " config=" << config_.configPath << "\n";
    return true;
}

void GpioInput::stop()
{
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

bool GpioInput::enabled() const
{
    return config_.configured && config_.enabled;
}

std::string GpioInput::configPath() const
{
    return config_.configPath;
}

std::string GpioInput::lastError() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastError_;
}

GpioInput::Config GpioInput::loadConfig()
{
    Config config;
    std::error_code ec;

    for (const auto &path : hardwareConfigCandidatePaths()) {
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec))
            continue;

        std::ifstream input(path);
        if (!input.is_open())
            continue;

        bool inButtonsSection = false;
        bool inEncodersSection = false;
        bool buttonsEnabled = false;
        bool encodersEnabled = false;
        std::array<EncoderMapping, 8> encoderDrafts{};
        std::array<bool, 8> encoderSeen{};
        std::map<int, ButtonMapping> buttonDrafts;   // by GPIO: press, clicks and hold gathered from dN, dN_single...

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
                const std::string section = normalizeToken(trimmed.substr(1, trimmed.size() - 2));
                inButtonsSection = section == "gpio_buttons" || section == "gpio" ||
                                   section == "grove_digital" || section == "grove_buttons";
                inEncodersSection = section == "gpio_encoders" || section == "grove_encoders";
                if (inButtonsSection || inEncodersSection) {
                    config.configured = true;
                    config.configPath = path.string();
                }
                continue;
            }

            if (!inButtonsSection && !inEncodersSection)
                continue;

            const std::size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = normalizeToken(trimmed.substr(0, eq));
            const std::string value = trimAscii(trimmed.substr(eq + 1));
            if (key == "enabled") {
                if (inButtonsSection)
                    buttonsEnabled = parseConfigBool(value, buttonsEnabled);
                else if (inEncodersSection)
                    encodersEnabled = parseConfigBool(value, encodersEnabled);
            } else if (key == "chip" || key == "gpiochip" || key == "gpio_chip") {
                config.chipPath = trimAscii(value).empty() ? "auto" : trimAscii(value);
            } else if (key == "active_low") {
                config.activeLow = parseConfigBool(value, config.activeLow);
            } else if (key == "pull" || key == "bias") {
                config.pull = normalizeToken(value);
            } else if (key == "poll_ms" || key == "poll_interval_ms") {
                if (const auto parsed = parseConfigInteger(value))
                    config.pollMs = std::clamp(*parsed, 2, 1000);
            } else if (key == "debounce_ms") {
                if (const auto parsed = parseConfigInteger(value))
                    config.debounceMs = std::clamp(*parsed, 0, 1000);
            } else if (inButtonsSection) {
                std::string base = key;
                std::string gesture;
                for (const std::string suffix : { "_single", "_double", "_triple", "_hold" }) {
                    if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        base.resize(base.size() - suffix.size());
                        gesture = suffix;
                        break;
                    }
                }
                if (const auto gpio = gpioFromDigitalKey(base)) {
                    ButtonMapping &mapping = buttonDrafts[*gpio];
                    mapping.gpio = *gpio;
                    const std::string action = normalizeAction(value);
                    if (gesture.empty())
                        mapping.action = action;
                    else if (gesture == "_single")
                        mapping.single = action;
                    else if (gesture == "_double")
                        mapping.doubleClick = action;
                    else if (gesture == "_triple")
                        mapping.triple = action;
                    else
                        mapping.hold = action;
                }
            } else if (inEncodersSection) {
                if (const auto encoder = encoderIndexFromKeyPrefix(key)) {
                    const std::string field = encoderFieldFromKey(key);
                    EncoderMapping &mapping = encoderDrafts[*encoder];
                    encoderSeen[*encoder] = true;
                    if (field == "a" || field == "gpio_a" || field == "pin_a" || field == "clk") {
                        if (const auto parsed = parseConfigInteger(value))
                            mapping.gpioA = *parsed;
                    } else if (field == "b" || field == "gpio_b" || field == "pin_b" || field == "dt") {
                        if (const auto parsed = parseConfigInteger(value))
                            mapping.gpioB = *parsed;
                    } else if (field == "cw" || field == "clockwise" || field == "right" || field == "up" ||
                               field == "plus") {
                        mapping.clockwise = normalizeAction(value);
                    } else if (field == "ccw" || field == "counter_clockwise" || field == "counterclockwise" ||
                               field == "left" || field == "down" || field == "minus") {
                        mapping.counterClockwise = normalizeAction(value);
                    } else if (field == "press_gpio" || field == "button_gpio" || field == "push_gpio" ||
                               field == "switch_gpio" || field == "sw") {
                        if (const auto parsed = parseConfigInteger(value))
                            mapping.pressGpio = *parsed;
                    } else if (field == "press" || field == "button" || field == "push" || field == "click") {
                        mapping.press = normalizeAction(value);
                    } else if (field == "invert" || field == "invert_direction" || field == "reverse_direction") {
                        mapping.invertDirection = parseConfigBool(value, mapping.invertDirection);
                    } else if (field == "steps_per_event" || field == "steps" || field == "detent_steps") {
                        if (const auto parsed = parseConfigInteger(value))
                            mapping.stepsPerEvent = std::clamp(*parsed, 1, 8);
                    }
                }
            }
        }

        for (auto &[gpio, mapping] : buttonDrafts) {
            if (!mapping.action.empty() || !mapping.single.empty() || !mapping.doubleClick.empty() ||
                !mapping.triple.empty() || !mapping.hold.empty())
                config.buttons.push_back(std::move(mapping));
        }

        if (!buttonsEnabled)
            config.buttons.clear();

        if (encodersEnabled) {
            for (size_t i = 0; i < encoderDrafts.size(); ++i) {
                EncoderMapping mapping = encoderDrafts[i];
                if (!encoderSeen[i] || mapping.gpioA < 0 || mapping.gpioB < 0)
                    continue;
                if (mapping.clockwise.empty() && mapping.counterClockwise.empty() && mapping.press.empty())
                    continue;
                config.encoders.push_back(std::move(mapping));
            }
        }

        config.enabled = buttonsEnabled || encodersEnabled;

        if (config.configured)
            return config;
    }

    return config;
}

bool GpioInput::requestLines(const std::vector<int> &offsets, bool edges, int *lineFd, std::string *errorMessage) const
{
    if (config_.chipPath != "auto")
        return requestLinesFromChip(config_.chipPath, offsets, edges, lineFd, errorMessage);

    std::string lastError;
    std::vector<std::pair<int, std::string>> candidates;
    for (int chip = 0; chip <= 9; ++chip) {
        const std::string path = "/dev/gpiochip" + std::to_string(chip);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            continue;
        const int score = hardware::gpioChipScore(path);
        if (score >= 0)
            candidates.emplace_back(score, path);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
        return left.first > right.first;
    });

    for (const auto &candidate : candidates) {
        const std::string &path = candidate.second;
        std::string error;
        if (requestLinesFromChip(path, offsets, edges, lineFd, &error))
            return true;
        if (!error.empty())
            lastError = error;
    }

    if (errorMessage)
        *errorMessage = lastError.empty() ? "No usable /dev/gpiochip found for configured GPIO lines" : lastError;
    return false;
}

bool GpioInput::requestLinesFromChip(const std::string &chipPath,
                                     const std::vector<int> &offsets,
                                     bool edges,
                                     int *lineFd,
                                     std::string *errorMessage) const
{
    if (!lineFd || offsets.empty() || offsets.size() > GPIO_V2_LINES_MAX)
        return false;

    const int chipFd = ::open(chipPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0) {
        if (errorMessage)
            *errorMessage = chipPath + ": " + std::strerror(errno);
        return false;
    }

    gpio_v2_line_request request{};
    std::snprintf(request.consumer, sizeof(request.consumer), "apertar-gpio-input");
    request.num_lines = static_cast<__u32>(offsets.size());
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    if (edges)
        request.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    if (config_.activeLow)
        request.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    const std::string pull = normalizeToken(config_.pull);
    if (pull == "up" || pull == "pull_up" || pull == "pullup")
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    else if (pull == "down" || pull == "pull_down" || pull == "pulldown")
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    else if (pull == "off" || pull == "none" || pull == "disabled")
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;

    for (size_t i = 0; i < offsets.size(); ++i)
        request.offsets[i] = static_cast<__u32>(offsets[i]);

    if (::ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        if (errorMessage)
            *errorMessage = chipPath + " request failed: " + std::strerror(errno);
        ::close(chipFd);
        return false;
    }

    ::close(chipFd);
    *lineFd = request.fd;
    return *lineFd >= 0;
}

bool GpioInput::readLineValues(int lineFd, size_t lineCount, uint64_t *bits) const
{
    if (lineFd < 0 || !bits || lineCount == 0 || lineCount > 64)
        return false;

    gpio_v2_line_values values{};
    values.mask = lineCount == 64 ? ~0ULL : ((1ULL << lineCount) - 1ULL);
    if (::ioctl(lineFd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return false;
    *bits = values.bits;
    return true;
}

void GpioInput::closeFd(int *fd) const
{
    if (fd && *fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

void GpioInput::emitAction(const std::string &action)
{
    if (action.empty() || !actionCallback_)
        return;
    actionCallback_(action);
}

void GpioInput::setLastError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (lastError_ == error)
        return;
    lastError_ = error;
    if (!error.empty())
        std::cerr << "GPIO input: " << error << "\n";
}

void GpioInput::run()
{
    int lineFd = -1;
    std::vector<int> offsets;
    offsets.reserve(config_.buttons.size() + config_.encoders.size() * 3);
    const auto addOffset = [&offsets](int gpio) {
        if (gpio >= 0 && std::find(offsets.begin(), offsets.end(), gpio) == offsets.end())
            offsets.push_back(gpio);
    };

    for (const ButtonMapping &button : config_.buttons)
        addOffset(button.gpio);
    for (const EncoderMapping &encoder : config_.encoders) {
        addOffset(encoder.gpioA);
        addOffset(encoder.gpioB);
        addOffset(encoder.pressGpio);
    }

    std::vector<RuntimeButton> buttons;
    std::vector<RuntimeEncoder> encoders;
    std::unordered_map<int, int> indexByOffset;
    for (size_t i = 0; i < offsets.size(); ++i)
        indexByOffset[offsets[i]] = static_cast<int>(i);
    bool edgeEvents = false;
    uint64_t bits = 0;

    const auto toMs = [](std::chrono::steady_clock::time_point at) {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count());
    };

    // One sample of every line: buttons are debounced and counted, encoders stepped.
    const auto process = [&](uint64_t levels, std::chrono::steady_clock::time_point now) {
        const int64_t nowMs = toMs(now);
        for (RuntimeButton &button : buttons) {
            const bool level = (levels & (1ULL << button.lineIndex)) != 0;
            if (!button.haveSample) {
                // Like Cinemate: a button that already reads pressed at start is wired the other way round (a 1-0-1,
                // normally closed button), so its levels are swapped from now on.
                button.inverted = level;
                if (level)
                    std::cerr << "GPIO input: GPIO " << button.gpio << " reads pressed at start; taken as inverted (a 1-0-1 button).\n";
                button.observedChangedAt = now;
                button.haveSample = true;
            }
            const bool pressed = level != button.inverted;
            if (pressed != button.observedPressed) {
                button.observedPressed = pressed;
                button.observedChangedAt = now;
            } else {
                const auto stableForMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - button.observedChangedAt).count();
                if (stableForMs >= config_.debounceMs && button.observedPressed != button.stablePressed)
                    button.stablePressed = button.observedPressed;
            }
            for (const auto event : button.gesture.update(button.stablePressed, nowMs)) {
                switch (event) {
                case hardware::ButtonGesture::Event::Press: emitAction(button.action); break;
                case hardware::ButtonGesture::Event::Single: emitAction(button.single); break;
                case hardware::ButtonGesture::Event::Double: emitAction(button.doubleClick); break;
                case hardware::ButtonGesture::Event::Triple: emitAction(button.triple); break;
                case hardware::ButtonGesture::Event::Hold: emitAction(button.hold); break;
                }
            }
        }

        for (RuntimeEncoder &encoder : encoders) {
            const int a = (levels & (1ULL << encoder.lineAIndex)) ? 1 : 0;
            const int b = (levels & (1ULL << encoder.lineBIndex)) ? 1 : 0;
            const int state = (a << 1) | b;
            if (encoder.lastState < 0) {
                encoder.lastState = state;
                continue;
            }
            int delta = quadratureDelta(encoder.lastState, state);
            encoder.lastState = state;
            if (encoder.invertDirection)
                delta = -delta;
            if (delta == 0)
                continue;

            encoder.accumulatedSteps += delta;
            while (encoder.accumulatedSteps >= encoder.stepsPerEvent) {
                if (!encoder.clockwise.empty() && rotationCallback_)
                    rotationCallback_(encoder.clockwise);
                encoder.accumulatedSteps -= encoder.stepsPerEvent;
            }
            while (encoder.accumulatedSteps <= -encoder.stepsPerEvent) {
                if (!encoder.counterClockwise.empty() && rotationCallback_)
                    rotationCallback_(encoder.counterClockwise);
                encoder.accumulatedSteps += encoder.stepsPerEvent;
            }
        }
    };

    // How long the thread may sleep with no edge: until a press settles or a click, long press or Hold is due.
    // Never past kIdleWakeMs, so stop() is noticed.
    const auto sleepMs = [&](int64_t nowMs) {
        constexpr int64_t kIdleWakeMs = 250;
        int64_t wakeMs = nowMs + kIdleWakeMs;
        for (const RuntimeButton &button : buttons) {
            if (button.observedPressed != button.stablePressed)   // + 1: the change time was rounded down
                wakeMs = std::min(wakeMs, toMs(button.observedChangedAt) + config_.debounceMs + 1);
            if (const int64_t gestureMs = button.gesture.nextWakeMs(); gestureMs >= 0)
                wakeMs = std::min(wakeMs, gestureMs);
        }
        return static_cast<int>(std::max<int64_t>(0, wakeMs - nowMs));
    };

    while (running_.load(std::memory_order_relaxed)) {
        if (lineFd < 0) {
            std::string errorMessage;
            // Edge events first, so the thread sleeps until a pin changes, the way Cinemate's gpiozero buttons wait.
            // A chip that cannot give them (no interrupt behind its lines) is read every poll_ms instead.
            edgeEvents = requestLines(offsets, true, &lineFd, &errorMessage);
            if (!edgeEvents && !requestLines(offsets, false, &lineFd, &errorMessage)) {
                setLastError(errorMessage);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            if (!edgeEvents)
                std::cerr << "GPIO input: these lines give no edge events; reading them every " << config_.pollMs
                          << " ms.\n";

            buttons.clear();
            encoders.clear();
            for (const ButtonMapping &button : config_.buttons) {
                if (const auto it = indexByOffset.find(button.gpio); it != indexByOffset.end()) {
                    RuntimeButton runtime;
                    runtime.lineIndex = it->second;
                    runtime.gpio = button.gpio;
                    runtime.action = button.action;
                    runtime.single = button.single;
                    runtime.doubleClick = button.doubleClick;
                    runtime.triple = button.triple;
                    runtime.hold = button.hold;
                    buttons.push_back(std::move(runtime));
                }
            }
            for (const EncoderMapping &encoder : config_.encoders) {
                const auto a = indexByOffset.find(encoder.gpioA);
                const auto b = indexByOffset.find(encoder.gpioB);
                if (a == indexByOffset.end() || b == indexByOffset.end())
                    continue;
                encoders.push_back({
                    a->second,
                    b->second,
                    encoder.clockwise,
                    encoder.counterClockwise,
                    encoder.invertDirection,
                    std::max(1, encoder.stepsPerEvent),
                });
                if (encoder.pressGpio >= 0 && !encoder.press.empty()) {
                    if (const auto press = indexByOffset.find(encoder.pressGpio); press != indexByOffset.end()) {
                        RuntimeButton runtime;
                        runtime.lineIndex = press->second;
                        runtime.gpio = encoder.pressGpio;
                        runtime.action = encoder.press;
                        buttons.push_back(std::move(runtime));
                    }
                }
            }
            setLastError({});
            if (edgeEvents) {
                // The starting levels, before any edge: they decide which buttons are 1-0-1.
                if (!readLineValues(lineFd, offsets.size(), &bits)) {
                    setLastError("read failed: " + std::string(std::strerror(errno)));
                    closeFd(&lineFd);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
                process(bits, std::chrono::steady_clock::now());
            }
        }

        if (edgeEvents) {
            pollfd ready{lineFd, POLLIN, 0};
            const int count = ::poll(&ready, 1, sleepMs(toMs(std::chrono::steady_clock::now())));
            if (count < 0 && errno != EINTR) {
                setLastError("wait failed: " + std::string(std::strerror(errno)));
                closeFd(&lineFd);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (count > 0 && (ready.revents & POLLIN)) {
                // Each edge in order, so an encoder sees every step even when several came in between.
                std::array<gpio_v2_line_event, 16> events{};
                const ssize_t got = ::read(lineFd, events.data(), sizeof(events));
                const auto now = std::chrono::steady_clock::now();
                for (ssize_t i = 0; got > 0 && i < got / static_cast<ssize_t>(sizeof(gpio_v2_line_event)); ++i) {
                    const auto it = indexByOffset.find(static_cast<int>(events[i].offset));
                    if (it == indexByOffset.end())
                        continue;
                    const uint64_t bit = 1ULL << it->second;
                    bits = events[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE ? (bits | bit) : (bits & ~bit);
                    process(bits, now);
                }
            }
        }

        // With edge events this also catches up with any edge the kernel dropped, and moves the timers on.
        if (!readLineValues(lineFd, offsets.size(), &bits)) {
            setLastError("read failed: " + std::string(std::strerror(errno)));
            closeFd(&lineFd);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        process(bits, std::chrono::steady_clock::now());

        if (!edgeEvents)
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollMs));
    }

    closeFd(&lineFd);
}

} // namespace apertar
