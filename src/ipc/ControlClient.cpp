#include "ipc/ControlClient.hpp"

#include "ipc/Cam1State.hpp"
#include "ipc/ControlProtocol.hpp"
#include "ipc/JsonKeys.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace apertar {
namespace {

constexpr size_t kMaxLineBytes = 4u * 1024 * 1024;   // a get_state reply is some KiB; this only stops a runaway
constexpr int kPollMs = 1000;                        // cam1's get_state, once a second (§4.1)
constexpr int kUnknownGraceMs = 2000;                // §3.4: 2 s of failed connects clear the "cam1 unknown" flag
constexpr int kIoStepMs = 100;
constexpr int kForwardIdBase = 1 << 20;
constexpr int kPollIdBase = 1 << 21;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool hasPrefix(const std::string &line, const char *prefix)
{
    return line.compare(0, std::strlen(prefix), prefix) == 0;
}

std::string forwardLine(int id, const std::string &name, const std::map<std::string, std::string> &fields)
{
    std::string line = "{\"cmd\":\"" + escapeJson(name) + "\",\"id\":" + std::to_string(id);
    for (const auto &[key, value] : fields)
        line += ",\"" + escapeJson(key) + "\":\"" + escapeJson(value) + "\"";
    line += "}";
    return line;
}

} // namespace

ControlClient::~ControlClient()
{
    stop();
}

void ControlClient::start(Options options)
{
    if (running_.exchange(true))
        return;
    options_ = std::move(options);
    startMs_ = nowMs();
    nextTickMs_ = 0;
    // §3.4: hold the trim and the Start pre-roll until cam1 answers, whenever the rig may have a cam1 running a take.
    bool unknown = false;
    if (options_.switchOn && options_.switchOn()) {
        struct stat st{};
        unknown = (options_.configNames && options_.configNames("cam1")) || ::stat(options_.socketPath.c_str(), &st) == 0;
    }
    unknown_.store(unknown, std::memory_order_release);
    if (::pipe(wakeFds_) == 0) {
        for (int fd : wakeFds_) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    thread_ = std::thread(&ControlClient::run, this);
}

void ControlClient::stop()
{
    if (!running_.exchange(false))
        return;
    wake();
    if (thread_.joinable())
        thread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    for (int &fd : wakeFds_) {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
    linked_.store(false, std::memory_order_release);
    configured_.store(false, std::memory_order_release);
    unknown_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    inFlight_ = false;
    pending_ = 0;
    idleCv_.notify_all();
}

void ControlClient::wake()
{
    if (wakeFds_[1] >= 0) {
        const char byte = 1;
        const ssize_t ignored = ::write(wakeFds_[1], &byte, 1);
        (void)ignored;
    }
}

ControlClient::Snapshot ControlClient::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot copy = state_;
    copy.configured = configured_.load(std::memory_order_acquire);
    copy.linked = linked_.load(std::memory_order_acquire);
    copy.pending = pending_;
    copy.busyForMs = pending_ > 0 ? nowMs() - busySinceMs_ : 0;
    return copy;
}

bool ControlClient::enqueue(const std::string &name, const std::map<std::string, std::string> &fields)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueueLocked(name, fields);
}

bool ControlClient::enqueueLocked(const std::string &name, const std::map<std::string, std::string> &fields)
{
    if (!linked_.load(std::memory_order_acquire))
        return false;   // dropped: the resync on relink catches up
    static int nextForwardId = kForwardIdBase;
    Forward forward;
    forward.id = nextForwardId++;
    forward.name = name;
    forward.fields = fields;
    queue_.push_back(std::move(forward));
    if (pending_++ == 0)
        busySinceMs_ = nowMs();
    wake();
    return true;
}

bool ControlClient::waitForIdle(int ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return pending_ == 0; });
}

void ControlClient::setMessage(const std::string &message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.message = message;
}

void ControlClient::setRefused(bool refused, const std::string &message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.refused = refused;
    state_.message = message;
}

bool ControlClient::idleToUnlink() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ == 0 && !state_.recording;
}

// ---- the relay thread -----------------------------------------------------------------------------------------------

void ControlClient::run()
{
    while (running_.load(std::memory_order_acquire)) {
        const int64_t now = nowMs();
        if (now >= nextTickMs_) {
            nextTickMs_ = now + kPollMs;
            tick();
        }
        ioStep(kIoStepMs);
    }
}

bool ControlClient::sameInodeAsOwn() const
{
    struct stat theirs{};
    struct stat ours{};
    if (options_.ownSocketPath.empty() || ::stat(options_.socketPath.c_str(), &theirs) != 0 ||
        ::stat(options_.ownSocketPath.c_str(), &ours) != 0)
        return false;
    return theirs.st_dev == ours.st_dev && theirs.st_ino == ours.st_ino;
}

int ControlClient::connectSocket() const
{
    sockaddr_un address{};
    if (options_.socketPath.size() >= sizeof(address.sun_path))
        return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, options_.socketPath.c_str(), options_.socketPath.size() + 1);
    timeval timeout{1, 0};   // a send to a stuck cam1 gives up after 1 s and drops the link
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Once a second: re-read the switch and config.txt, link or unlink, poll cam1's get_state.
void ControlClient::tick()
{
    const bool switchOn = options_.switchOn && options_.switchOn();
    if (!switchOn) {
        // Off: no link, no cam1 object. A link that cannot be dropped yet (a take with cam1, a setter in flight) stays
        // until the next idle moment, and cam1 stays configured meanwhile.
        acceptedEver_ = false;
        unknown_.store(false, std::memory_order_release);
        if (fd_ >= 0 && idleToUnlink())
            dropLink(nullptr);
        configured_.store(fd_ >= 0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.sensorMissing = false;
        return;
    }

    const bool namesCam1 = options_.configNames && options_.configNames("cam1");
    const bool namesBoth = namesCam1 && options_.configNames("cam0");
    const bool ours = sameInodeAsOwn();

    if (fd_ < 0 && !ours) {
        const int fd = connectSocket();
        if (fd >= 0) {
            fd_ = fd;
            acceptedEver_ = true;
            rx_.clear();
            pollId_ = 0;
            settled_ = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.lost = false;
                if (state_.message == "link down")
                    state_.message.clear();
                state_.answered = false;
                state_.sensorMissing = false;
            }
            linked_.store(true, std::memory_order_release);
            std::cerr << "ApertarCore cam1 " << (everLinked_ ? "relinked" : "linked") << " (" << options_.socketPath << ")\n";
            everLinked_ = true;
        }
    }
    // §3.4: 2 s without a link, whatever kept it (refused, no socket file, our own socket), end the "unknown" hold.
    if (fd_ < 0 && unknown_.load(std::memory_order_acquire) && nowMs() - startMs_ >= kUnknownGraceMs)
        unknown_.store(false, std::memory_order_release);

    cam1state::ConfiguredInputs inputs;
    inputs.switchOn = true;
    inputs.configNamesBoth = namesBoth;
    inputs.socketAccepted = acceptedEver_;
    inputs.socketIsOurs = ours;
    configured_.store(cam1state::configured(inputs) || fd_ >= 0, std::memory_order_release);

    if (fd_ < 0) {
        const bool missing = namesCam1 && options_.sensorPresent && !options_.sensorPresent();
        std::lock_guard<std::mutex> lock(mutex_);
        state_.sensorMissing = missing;
        return;
    }
    if (pollId_ == 0) {
        pollId_ = kPollIdBase + (nextId_++ & 0xFFFFF);
        if (!sendLine("{\"cmd\":\"get_state\",\"id\":" + std::to_string(pollId_) + "}"))
            dropLink("send failed");
    }
}

void ControlClient::ioStep(int timeoutMs)
{
    pollfd fds[2];
    nfds_t count = 0;
    fds[count++] = pollfd{wakeFds_[0], POLLIN, 0};
    if (fd_ >= 0)
        fds[count++] = pollfd{fd_, POLLIN, 0};
    if (::poll(fds, count, timeoutMs) < 0 && errno != EINTR)
        return;
    if (fds[0].revents & POLLIN) {
        char drain[64];
        while (::read(wakeFds_[0], drain, sizeof(drain)) > 0) {}
    }
    if (fd_ >= 0 && count == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
        readSome();
    if (fd_ >= 0)
        sendNextForward();
}

bool ControlClient::sendLine(const std::string &line)
{
    if (fd_ < 0)
        return false;
    const std::string payload = line + "\n";
    size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t written = ::send(fd_, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        sent += static_cast<size_t>(written);
    }
    return true;
}

// recvmsg, so that a descriptor sent with a line is received and closed at once (only the buffer registration carries
// any; the relay has no use for them).
void ControlClient::readSome()
{
    char data[65536];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    iovec vec{data, sizeof(data)};
    msghdr message{};
    message.msg_iov = &vec;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t got = ::recvmsg(fd_, &message, MSG_CMSG_CLOEXEC);
    if (got < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        const size_t bytes = header->cmsg_len - CMSG_LEN(0);
        int received[16];
        std::memcpy(received, CMSG_DATA(header), std::min(bytes, sizeof(received)));
        for (size_t i = 0; i < std::min(bytes, sizeof(received)) / sizeof(int); ++i)
            ::close(received[i]);
    }
    if (got <= 0) {
        dropLink(got == 0 ? "closed by cam1" : std::strerror(errno));
        return;
    }
    rx_.append(data, static_cast<size_t>(got));
    size_t start = 0;
    for (size_t newline; fd_ >= 0 && (newline = rx_.find('\n', start)) != std::string::npos; start = newline + 1) {
        if (newline > start)
            handleLine(rx_.substr(start, newline - start));
    }
    if (fd_ < 0)
        return;
    rx_.erase(0, start);
    if (rx_.size() > kMaxLineBytes)
        dropLink("line too long");
}

void ControlClient::handleLine(const std::string &line)
{
    if (hasPrefix(line, "{\"ok\""))
        handleReply(line);
    else if (hasPrefix(line, "{\"event\""))
        handleEvent(line);
}

void ControlClient::handleReply(const std::string &line)
{
    const auto idValue = jsonkeys::numberOf(line, "id");
    if (!idValue)
        return;
    const int id = static_cast<int>(*idValue);
    if (pollId_ != 0 && id == pollId_) {
        pollId_ = 0;
        handleGetState(line);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inFlight_ || id != flight_.id)
        return;
    if (jsonkeys::boolOf(line, "ok").value_or(false)) {
        if (state_.message.rfind("CAM1: ", 0) == 0)
            state_.message.clear();
    } else {
        state_.message = "CAM1: " + jsonkeys::stringOf(line, "error").value_or("refused");
    }
    inFlight_ = false;
    if (--pending_ <= 0) {
        pending_ = 0;
        busySinceMs_ = 0;
    }
    idleCv_.notify_all();
}

// Keeps the recording, buffer, media_write_state and preview_frame events (timestamps, for the live skew of §7.6) and
// skips the rest (P4).
void ControlClient::handleEvent(const std::string &line)
{
    if (hasPrefix(line, "{\"event\":\"preview_frame\"")) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastPreviewFrameMs = nowMs();
    } else if (hasPrefix(line, "{\"event\":\"recording\"")) {
        const auto active = jsonkeys::boolOf(line, "active");
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastRecordingEventMs = nowMs();
        if (active)
            state_.recording = *active;
    } else if (hasPrefix(line, "{\"event\":\"buffer\"")) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastBufferEventMs = nowMs();
    } else if (hasPrefix(line, "{\"event\":\"media_write_state\"")) {
        const auto pending = jsonkeys::boolOf(line, "media_writes_pending");
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastMediaWriteEventMs = nowMs();
        if (pending)
            state_.writing = *pending;
    }
}

void ControlClient::handleGetState(const std::string &line)
{
    if (!jsonkeys::boolOf(line, "ok").value_or(false))
        return;
    const std::string camera = jsonkeys::stringOf(line, "state").value_or("");
    const bool standby = jsonkeys::boolOf(line, "standby").value_or(false);
    const bool recording = jsonkeys::boolOf(line, "recording").value_or(false);
    const bool writes = jsonkeys::boolOf(line, "media_writes_pending").value_or(false);
    const auto flag = [&line](const char *key) { return jsonkeys::boolOf(line, key).value_or(false); };
    const bool idle = camera == "ready" && !recording && !flag("storage_preroll_active") && !flag("timelapse_active") &&
                      !flag("still_exposure_active") && !writes && !flag("photo_mode_enabled");
    // cam1's settingsBody sits in the top level of the reply, from "resolution" to "clip_merge_env_off".
    const std::string run = jsonkeys::run(line, "resolution", "clip_merge_env_off");
    bool doSettle = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.standby = standby || camera == "standby";
        state_.recording = recording;
        state_.writing = writes;
        state_.idle = idle;
        state_.answered = true;
        state_.droppedFrames = static_cast<long long>(jsonkeys::numberOf(line, "recording_dropped_frames").value_or(0));
        state_.sensor = jsonkeys::stringOf(line, "sensor").value_or(state_.sensor);
        state_.sensorMono = jsonkeys::boolOf(line, "sensor_mono").value_or(state_.sensorMono);
        if (!run.empty())
            state_.settings = "{" + run + "}";
        doSettle = !settled_;
    }
    if (!doSettle)
        return;
    settled_ = true;
    unknown_.store(false, std::memory_order_release);
    // §4.6 Standby: on link or relink the primary's standby goes to cam1, and a cam1 left in standby while the primary is
    // awake is woken. Both count as setters (the busy window).
    const bool primaryStandby = options_.primaryInStandby && options_.primaryInStandby();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!primaryStandby && state_.standby)
        enqueueLocked("wake", {});
    else if (primaryStandby && !state_.standby)
        enqueueLocked("enter_standby", {});
}

void ControlClient::sendNextForward()
{
    std::string line;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inFlight_ || queue_.empty())
            return;
        flight_ = std::move(queue_.front());
        queue_.pop_front();
        inFlight_ = true;
        line = forwardLine(flight_.id, flight_.name, flight_.fields);
    }
    if (!sendLine(line))
        dropLink("send failed");
}

// EOF or a failed send. Setters queued or unanswered are dropped, so the pending count returns to 0 (§4.1); a take the
// relay knew about turns `lost` ("link down") until the relink (§4.5). `why` is null for the quiet unlink of a switch
// turned off.
void ControlClient::dropLink(const char *why)
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rx_.clear();
    pollId_ = 0;
    linked_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        inFlight_ = false;
        pending_ = 0;
        busySinceMs_ = 0;
        state_.answered = false;
        state_.idle = false;
        state_.standby = false;
        state_.writing = false;
        if (why && state_.recording) {
            state_.lost = true;
            state_.message = "link down";
        }
        if (!why)
            state_.recording = false;
        idleCv_.notify_all();
    }
    if (why)
        std::cerr << "ApertarCore cam1 link down (" << why << ")\n";
}

} // namespace apertar
