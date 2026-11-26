#pragma once
// The Pinefeat CEF168 board's serial port (UART 3.3 V, 115200 8N1, ASCII; pinefeat/cef168 doc/serial.md): read only. Core uses it
// for what the I2C data frame does not carry, the focal length and the aperture range of the mounted lens. Control stays on
// I2C / V4L2, so no command here writes. This header is the pure part (the reply parsers, host-tested by tools/lens_test.cpp)
// and the thread's interface; the port is Cef168Serial.cpp (tools/lens_serial_test.cpp drives it against a pseudo-terminal).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apertar::hardware::cef168::serial {

// ---- reply parsers -------------------------------------------------------------------------------------------------------

inline std::string trimmed(std::string_view text)
{
    size_t first = 0, last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return std::string(text.substr(first, last - first));
}

inline std::string lowered(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

inline bool isErrorReply(std::string_view line) { return lowered(trimmed(line)) == "er"; }
inline bool isNotConnectedReply(std::string_view line) { return lowered(trimmed(line)) == "nc"; }

// The decimal numbers in `text`, in order ("f/3.5-22.6" gives 3.5 and 22.6). The text may hold only the characters in `allowed`
// besides digits and dots: a reply with anything else in it is not a number reply (garbage on a noisy line).
inline std::optional<std::vector<double>> numbersIn(std::string_view text, std::string_view allowed)
{
    std::vector<double> numbers;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t j = i;
            while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '.')) ++j;
            const std::string token(text.substr(i, j - i));
            char *end = nullptr;
            const double value = std::strtod(token.c_str(), &end);
            if (token == "." || *end != '\0') return std::nullopt;
            numbers.push_back(value);
            i = j;
        } else if (allowed.find(c) != std::string_view::npos) {
            ++i;
        } else {
            return std::nullopt;
        }
    }
    return numbers;
}

// `l`: the focal length in mm ("12", or "12 mm"). Nothing for er / nc / anything else.
inline std::optional<double> parseFocalLength(std::string_view reply)
{
    const std::string text = trimmed(reply);
    const auto numbers = numbersIn(text, " \t");
    std::optional<std::vector<double>> parsed = numbers;
    if (!parsed) {   // allow a trailing "mm"
        const std::string low = lowered(text);
        if (low.size() > 2 && low.compare(low.size() - 2, 2, "mm") == 0) parsed = numbersIn(low.substr(0, low.size() - 2), " \t");
    }
    if (!parsed || parsed->size() != 1 || (*parsed)[0] <= 0.0 || (*parsed)[0] > 2000.0) return std::nullopt;
    return (*parsed)[0];
}

// `a` and the Aperture line of `h`: the f-stop range, "3.5-22.6", "f/3.5-22.6" or "f/3.5 - f/22.6". Widest first.
inline std::optional<std::pair<double, double>> parseApertureRange(std::string_view reply)
{
    const auto numbers = numbersIn(trimmed(reply), "fF/-\t \xe2\x80\x93");   // the en dash is bytes e2 80 93
    if (!numbers || numbers->size() != 2) return std::nullopt;
    double low = (*numbers)[0], high = (*numbers)[1];
    if (low > high) std::swap(low, high);
    if (low < 0.5 || high > 128.0) return std::nullopt;
    return std::pair<double, double>{low, high};
}

struct DistanceRange {
    double minMetres = 0.0;
    double maxMetres = 0.0;
    bool infinite = false;   // the far end is infinity
};

// `d` and the Focus distance line of `h`, in metres: "0.23-0.23", "0.23-0.23 m", "0.45 - inf".
inline std::optional<DistanceRange> parseDistanceRange(std::string_view reply)
{
    std::string text = lowered(trimmed(reply));
    bool infinite = false;
    for (const char *word : {"infinity", "inf", "\xe2\x88\x9e"}) {
        const size_t at = text.find(word);
        if (at != std::string::npos) {
            infinite = true;
            text.replace(at, std::string_view(word).size(), " ");
            break;
        }
    }
    if (!text.empty() && text.back() == 'm') text.pop_back();
    const auto numbers = numbersIn(text, "-\t \xe2\x80\x93");
    if (!numbers) return std::nullopt;
    DistanceRange range;
    range.infinite = infinite;
    if (infinite && numbers->size() == 1) { range.minMetres = (*numbers)[0]; range.maxMetres = (*numbers)[0]; }
    else if (!infinite && numbers->size() == 1) { range.minMetres = range.maxMetres = (*numbers)[0]; }
    else if (numbers->size() == 2) { range.minMetres = std::min((*numbers)[0], (*numbers)[1]); range.maxMetres = std::max((*numbers)[0], (*numbers)[1]); }
    else return std::nullopt;
    return range;
}

// `h`, the multi-line lens info. Every field the board printed that makes sense; nothing when none does.
struct LensInfo {
    std::optional<int> lensId;
    std::optional<double> focalMin, focalMax;
    std::optional<std::pair<double, double>> aperture;
    std::optional<DistanceRange> distance;
};

inline std::optional<LensInfo> parseLensInfo(std::string_view text)
{
    LensInfo info;
    bool any = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string line = trimmed(text.substr(start, end - start));
        start = end + 1;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lowered(trimmed(std::string_view(line).substr(0, colon)));
        const std::string value = trimmed(std::string_view(line).substr(colon + 1));
        if (key == "lens id") {
            char *last = nullptr;
            const long id = std::strtol(value.c_str(), &last, 0);
            if (!value.empty() && *last == '\0' && id >= 0 && id <= 255) { info.lensId = static_cast<int>(id); any = true; }
        } else if (key == "focal length") {
            std::string digits = lowered(value);
            if (digits.size() > 2 && digits.compare(digits.size() - 2, 2, "mm") == 0) digits.resize(digits.size() - 2);
            if (const auto numbers = numbersIn(digits, "-\t \xe2\x80\x93"); numbers && !numbers->empty() && numbers->size() <= 2) {
                const double a = (*numbers)[0], b = numbers->size() == 2 ? (*numbers)[1] : a;
                if (a > 0.0 && b > 0.0) { info.focalMin = std::min(a, b); info.focalMax = std::max(a, b); any = true; }
            }
        } else if (key == "aperture") {
            if (const auto range = parseApertureRange(value)) { info.aperture = range; any = true; }
        } else if (key == "focus distance") {
            if (const auto range = parseDistanceRange(value)) { info.distance = range; any = true; }
        }
    }
    if (!any) return std::nullopt;
    return info;
}

// `v` and `n`: one short printable word or number ("1.3", "00A1B2"). Nothing for er / nc / blank / a long or odd line.
inline std::optional<std::string> parseToken(std::string_view reply)
{
    const std::string text = trimmed(reply);
    if (text.empty() || text.size() > 32 || isErrorReply(text) || isNotConnectedReply(text)) return std::nullopt;
    for (const char c : text)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return std::nullopt;
    return text;
}

// ---- what the port knows ---------------------------------------------------------------------------------------------------

struct Values {
    std::string device;               // the configured port; empty = off
    bool connected = false;           // the board answered recently
    bool lensConnected = false;       // ... and it has a lens (the board answers `nc` for the lens commands when it has not)
    std::string firmware, boardSerial, error;
    std::optional<int> lensId;
    std::optional<double> focalLengthMm;                    // `l`, at the lens's current zoom
    std::optional<double> focalMinMm, focalMaxMm;           // `h`
    std::optional<std::pair<double, double>> apertureRange; // `a`, at the current focal length; widest first
    std::optional<double> apertureAtMinFocal, apertureAtMaxFocal;   // the widest f-number seen when the zoom was at either end
    std::optional<DistanceRange> distance;                  // `d`

    bool operator==(const Values &other) const
    {
        const auto same = [](const auto &a, const auto &b) { return a == b; };
        return device == other.device && connected == other.connected && lensConnected == other.lensConnected &&
            firmware == other.firmware && boardSerial == other.boardSerial && error == other.error && same(lensId, other.lensId) &&
            same(focalLengthMm, other.focalLengthMm) && same(focalMinMm, other.focalMinMm) && same(focalMaxMm, other.focalMaxMm) &&
            same(apertureRange, other.apertureRange) && same(apertureAtMinFocal, other.apertureAtMinFocal) &&
            same(apertureAtMaxFocal, other.apertureAtMaxFocal) &&
            distance.has_value() == other.distance.has_value() &&
            (!distance || (distance->minMetres == other.distance->minMetres && distance->maxMetres == other.distance->maxMetres &&
                           distance->infinite == other.distance->infinite));
    }
};

struct Timing {
    std::chrono::milliseconds replyTimeout{150};   // one reply line
    std::chrono::milliseconds quietGap{60};        // the end of the multi-line `h`
    std::chrono::milliseconds poll{250};           // `l` and `a`
    std::chrono::milliseconds distancePoll{1000};  // `d`
    std::chrono::milliseconds retry{5000};         // a link that is down is tried again this often
    int failuresToDrop = 3;                        // poll rounds without a single valid reply
};

class Cef168Serial {
public:
    explicit Cef168Serial(Timing timing = {});
    ~Cef168Serial();
    Cef168Serial(const Cef168Serial &) = delete;
    Cef168Serial &operator=(const Cef168Serial &) = delete;

    void configure(const std::string &device);   // "" = off (the thread idles with no port open); cheap when unchanged
    void setPaused(bool paused);                 // no traffic while a calibration runs
    void setLensHint(int lensId);                // the I2C lens id: a change runs `h` again
    void setOnChange(std::function<void()> onChange);   // called on the port's thread when Values changed
    Values values() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apertar::hardware::cef168::serial
