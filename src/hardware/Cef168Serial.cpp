#include "hardware/Cef168Serial.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace apertar::hardware::cef168::serial {

namespace {
using Clock = std::chrono::steady_clock;

// 115200 8N1, raw, no flow control. Never the controlling terminal.
int openPort(const std::string &device, std::string *error)
{
    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *error = "Cannot open " + device + ": " + std::strerror(errno);
        return -1;
    }
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        *error = device + " is not a serial port: " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, B115200);
    ::cfsetospeed(&tio, B115200);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        *error = "Cannot set up " + device + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    ::tcflush(fd, TCIOFLUSH);
    return fd;
}

// Reads until `done(buffer)` or `deadline`; `gap` > 0 ends the read after that much silence once something arrived.
template <typename Done>
std::string readUntil(int fd, Clock::time_point deadline, std::chrono::milliseconds gap, Done done)
{
    std::string buffer;
    auto quietUntil = Clock::time_point::max();
    while (true) {
        const auto now = Clock::now();
        auto limit = deadline;
        if (quietUntil < limit) limit = quietUntil;
        if (now >= limit) break;
        pollfd pfd{fd, POLLIN, 0};
        const int wait = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(limit - now).count()) + 1;
        const int ready = ::poll(&pfd, 1, wait);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (pfd.revents & POLLIN)) {
            char chunk[256];
            const ssize_t got = ::read(fd, chunk, sizeof(chunk));
            if (got > 0) {
                buffer.append(chunk, static_cast<size_t>(got));
                if (done(buffer)) break;
                if (gap.count() > 0) quietUntil = Clock::now() + gap;
            }
        }
    }
    return buffer;
}

bool sendCommand(int fd, const std::string &command)
{
    const std::string line = command + "\r";
    ::tcflush(fd, TCIFLUSH);   // whatever is left of an earlier answer
    return ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
}

// One command, one reply line (a timeout or a dead port gives nothing).
std::optional<std::string> ask(int fd, const std::string &command, std::chrono::milliseconds timeout)
{
    if (!sendCommand(fd, command)) return std::nullopt;
    const std::string raw = readUntil(fd, Clock::now() + timeout, std::chrono::milliseconds(0),
                                      [](const std::string &text) { return text.find('\n') != std::string::npos; });
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) return std::nullopt;   // no complete line
        const std::string line = trimmed(std::string_view(raw).substr(start, end - start));
        start = end + 1;
        if (!line.empty()) return line;
    }
    return std::nullopt;
}

// A multi-line reply: the first byte within `timeout`, then until the line is quiet for `gap` (at most 800 ms in all).
std::optional<std::string> askMulti(int fd, const std::string &command, std::chrono::milliseconds timeout, std::chrono::milliseconds gap)
{
    if (!sendCommand(fd, command)) return std::nullopt;
    pollfd first{fd, POLLIN, 0};
    if (::poll(&first, 1, static_cast<int>(timeout.count())) <= 0) return std::nullopt;   // nothing started within the timeout
    const std::string text = readUntil(fd, Clock::now() + std::chrono::milliseconds(800), gap, [](const std::string &) { return false; });
    if (trimmed(text).empty()) return std::nullopt;
    return text;
}
} // namespace

struct Cef168Serial::Impl {
    Timing timing;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::string device;
    bool paused = false;
    std::optional<int> hint;
    bool changed = false;     // configure / hint / pause was touched: the loop looks again at once
    bool stop = false;
    Values values;
    std::function<void()> onChange;
    std::thread thread;

    explicit Impl(Timing t) : timing(t) {}
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    // Applies `edit` to the values under the lock and tells the listener when something changed.
    template <typename Edit>
    void update(Edit edit)
    {
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const Values before = values;
            edit(values);
            if (!(values == before)) notify = onChange;
        }
        if (notify) notify();
    }

    // Sleeps up to `span`, or less when asked to look again or to stop. False = stop.
    bool sleep(std::chrono::milliseconds span)
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, span, [this] { return stop || changed; });
        return !stop;
    }

    void run()
    {
        int fd = -1;
        std::string opened;
        bool linked = false;
        int failures = 0;
        int lastHint = -1;
        bool lensWasConnected = false;
        bool infoWanted = false;
        auto nextProbe = Clock::now();
        auto nextPoll = Clock::now();
        auto nextDistance = Clock::now();
        std::optional<double> lastFocal;

        const auto drop = [&](const std::string &why) {
            if (fd >= 0) ::close(fd);
            fd = -1;
            linked = false;
            lensWasConnected = false;
            update([&](Values &v) {
                v.connected = false;
                v.lensConnected = false;
                v.error = why;
            });
        };

        while (true) {
            std::string wanted;
            bool pause = false;
            std::optional<int> hintNow;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stop) break;
                wanted = device;
                pause = paused;
                hintNow = hint;
                changed = false;
            }
            if (wanted != opened) {
                if (fd >= 0) ::close(fd);
                fd = -1;
                linked = false;
                opened = wanted;
                nextProbe = Clock::now();
                update([&](Values &v) { v = Values{}; v.device = wanted; });
            }
            if (wanted.empty()) { if (!sleep(std::chrono::seconds(1))) break; continue; }
            if (pause) { if (!sleep(std::chrono::milliseconds(100))) break; continue; }
            if (hintNow && *hintNow != lastHint) {
                if (lastHint >= 0) infoWanted = true;   // another lens: its range
                lastHint = *hintNow;
            }

            const auto now = Clock::now();
            if (!linked) {
                if (now < nextProbe) {
                    if (!sleep(std::chrono::duration_cast<std::chrono::milliseconds>(nextProbe - now) + std::chrono::milliseconds(1))) break;
                    continue;
                }
                std::string error;
                if (fd < 0) fd = openPort(wanted, &error);
                if (fd < 0) {
                    nextProbe = Clock::now() + timing.retry;
                    update([&](Values &v) { v.connected = false; v.error = error; });
                    continue;
                }
                // `v`, `n`, then `h`: any valid answer is a board.
                const auto version = ask(fd, "v", timing.replyTimeout);
                const auto number = ask(fd, "n", timing.replyTimeout);
                const auto info = askMulti(fd, "h", timing.replyTimeout, timing.quietGap);
                const bool alive = version || number || (info && !isErrorReply(*info));
                if (!alive) {
                    ::close(fd);
                    fd = -1;
                    nextProbe = Clock::now() + timing.retry;
                    update([&](Values &v) {
                        v.connected = false;
                        v.error = "The CEF168 board does not answer on " + wanted + ". Check the cable (board RXD to the Pi TX GPIO14).";
                    });
                    continue;
                }
                linked = true;
                failures = 0;
                infoWanted = false;
                                const auto parsedInfo = info ? parseLensInfo(*info) : std::nullopt;
                update([&](Values &v) {
                    v.connected = true;
                    v.error.clear();
                    if (version) if (const auto token = parseToken(*version)) v.firmware = *token;
                    if (number) if (const auto token = parseToken(*number)) v.boardSerial = *token;
                    applyInfo(v, parsedInfo);
                });
                nextPoll = Clock::now();
                nextDistance = nextPoll;
                continue;
            }

            if (now < nextPoll) {
                if (!sleep(std::chrono::duration_cast<std::chrono::milliseconds>(nextPoll - now) + std::chrono::milliseconds(1))) break;
                continue;
            }
            nextPoll = Clock::now() + timing.poll;
            if (infoWanted) {
                infoWanted = false;
                const auto info = askMulti(fd, "h", timing.replyTimeout, timing.quietGap);
                const auto parsedInfo = info ? parseLensInfo(*info) : std::nullopt;
                update([&](Values &v) { applyInfo(v, parsedInfo); });
            }
            bool answered = false, lensAnswered = false;
            std::optional<double> focal;
            if (const auto reply = ask(fd, "l", timing.replyTimeout)) {
                answered = true;
                focal = parseFocalLength(*reply);
                lensAnswered = focal.has_value();
                const bool noLens = isNotConnectedReply(*reply);
                if (noLens) {
                    update([&](Values &v) {
                        v.lensConnected = false;
                        v.focalLengthMm.reset();
                        v.focalMinMm.reset();
                        v.focalMaxMm.reset();
                        v.apertureRange.reset();
                        v.apertureAtMinFocal.reset();
                        v.apertureAtMaxFocal.reset();
                        v.distance.reset();
                        v.lensId.reset();
                    });
                }
            }
            std::optional<std::pair<double, double>> aperture;
            if (lensAnswered) {
                if (const auto reply = ask(fd, "a", timing.replyTimeout)) {
                    answered = true;
                    aperture = parseApertureRange(*reply);
                }
            }
            std::optional<DistanceRange> distance;
            bool distanceAsked = false;
            if (lensAnswered && Clock::now() >= nextDistance) {
                nextDistance = Clock::now() + timing.distancePoll;
                distanceAsked = true;
                if (const auto reply = ask(fd, "d", timing.replyTimeout)) {
                    answered = true;
                    distance = parseDistanceRange(*reply);
                }
            }
            if (lensAnswered && !lensWasConnected) infoWanted = true;   // a lens has just appeared: its range
            lensWasConnected = lensAnswered;
            if (focal) lastFocal = focal;
            update([&](Values &v) {
                v.connected = true;
                v.error.clear();
                if (lensAnswered) {
                    v.lensConnected = true;
                    v.focalLengthMm = focal;
                    // An `a` that did not parse keeps the last range: a noisy line must not flicker the iris limits.
                    if (aperture) {
                        v.apertureRange = aperture;
                        if (v.focalMinMm && *focal <= *v.focalMinMm + 0.01) v.apertureAtMinFocal = aperture->first;
                        if (v.focalMaxMm && *focal >= *v.focalMaxMm - 0.01) v.apertureAtMaxFocal = aperture->first;
                    }
                    if (distanceAsked && distance) v.distance = distance;
                }
            });
            if (answered) failures = 0;
            else if (++failures >= timing.failuresToDrop) {
                drop("The CEF168 board stopped answering on " + wanted + ".");
                nextProbe = Clock::now() + timing.retry;
            }
        }
        if (fd >= 0) ::close(fd);
    }

    static void applyInfo(Values &v, const std::optional<LensInfo> &info)
    {
        if (!info) return;
        if (info->lensId) v.lensId = info->lensId;
        if (info->focalMin) {
            v.focalMinMm = info->focalMin;
            v.focalMaxMm = info->focalMax;
        }
        if (info->distance) v.distance = info->distance;
        // `h` prints the aperture at the zoom the lens is at now: the poll reads `a` right after and files it under the focal length.
        if (info->aperture && !v.apertureRange) v.apertureRange = info->aperture;
        // A prime has one focal length: its widest f-number is both ends.
        if (v.focalMinMm && v.focalMaxMm && std::abs(*v.focalMaxMm - *v.focalMinMm) < 0.01 && v.apertureRange) {
            v.apertureAtMinFocal = v.apertureRange->first;
            v.apertureAtMaxFocal = v.apertureRange->first;
        }
    }
};

Cef168Serial::Cef168Serial(Timing timing) : impl_(std::make_unique<Impl>(timing)) {}
Cef168Serial::~Cef168Serial() = default;

void Cef168Serial::configure(const std::string &device)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (device == impl_->device && impl_->thread.joinable()) return;
        impl_->device = device;
        impl_->changed = true;
        if (!impl_->thread.joinable() && !device.empty()) impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    }
    impl_->wake.notify_all();
}

void Cef168Serial::setPaused(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->paused == paused) return;
        impl_->paused = paused;
        impl_->changed = true;
    }
    impl_->wake.notify_all();
}

void Cef168Serial::setLensHint(int lensId)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->hint && *impl_->hint == lensId) return;
        impl_->hint = lensId;
        impl_->changed = true;
    }
    impl_->wake.notify_all();
}

void Cef168Serial::setOnChange(std::function<void()> onChange)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onChange = std::move(onChange);
}

Values Cef168Serial::values() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->values;
}

} // namespace apertar::hardware::cef168::serial
