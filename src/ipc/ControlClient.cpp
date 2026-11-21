#include "ipc/ControlClient.hpp"

#include "ipc/Cam1State.hpp"
#include "ipc/ControlProtocol.hpp"
#include "ipc/JsonKeys.hpp"
#include "ipc/Resync.hpp"
#include "audio/RecordingClock.hpp"
#include "util/TakeSlots.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
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
constexpr int kReadIdBase = 1 << 22;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// CLOCK_MONOTONIC in microseconds (steady_clock is CLOCK_MONOTONIC on Linux): the domain of start_us and stop_us.
int64_t monoUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A stop already sent is given this long past its stop_us before the orphan rule applies (design 4.6 rule 2:
// stop_us + max(3T, latency); 1 s covers both, and is the orphan stop's "within 1 s").
constexpr int64_t kStopGraceUs = 1000000;
constexpr int64_t kOrphanRepeatMs = 2000;
constexpr size_t kStoppedKeep = 16;

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

// One get_state answer, as far as the take rules read it.
struct ControlClient::TakePoll {
    bool recording = false;
    std::string clip;                  // recording_clip_name
    std::optional<double> firstSlot;
    std::optional<double> lastSlot;
    double fps = 0;
    int64_t sentMs = 0;                // when the get_state was sent
    bool relink = false;               // the first answer after a (re)link
};

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
    failReplies();
    queue_.clear();
    inFlight_ = false;
    pending_ = 0;
    takeIn_ = false;
    takeClip_.clear();
    observedRecording_ = false;
    refreshRecordingLocked();
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

bool ControlClient::Reply::wait(int ms, std::string *out)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return done; }) || failed)
        return false;
    if (out)
        *out = line;
    return true;
}

bool ControlClient::enqueue(const std::string &name, const std::map<std::string, std::string> &fields, Origin origin)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueueLocked(name, fields, origin);
}

std::shared_ptr<ControlClient::Reply> ControlClient::enqueueWithReply(const std::string &name,
                                                                      const std::map<std::string, std::string> &fields)
{
    auto reply = std::make_shared<Reply>();
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueueLocked(name, fields, Origin::Camera, 0, reply) ? reply : nullptr;
}

std::shared_ptr<ControlClient::Reply> ControlClient::readNow(const std::string &name, const std::map<std::string, std::string> &fields)
{
    auto reply = std::make_shared<Reply>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!linked_.load(std::memory_order_acquire))
        return nullptr;
    static int nextReadId = kReadIdBase;
    Forward read;
    read.id = reply->id = nextReadId++;
    read.name = name;
    read.fields = fields;
    read.reply = reply;
    reads_.push_back(std::move(read));   // not counted: a read never makes cam1 busy
    wake();
    return reply;
}

void ControlClient::forget(const std::shared_ptr<Reply> &reply)
{
    if (!reply)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    readsInFlight_.erase(reply->id);
    for (auto it = reads_.begin(); it != reads_.end(); ++it) {
        if (it->id == reply->id) {
            reads_.erase(it);
            break;
        }
    }
}

bool ControlClient::enqueueLocked(const std::string &name, const std::map<std::string, std::string> &fields, Origin origin, int step,
                                  std::shared_ptr<Reply> reply)
{
    if (!linked_.load(std::memory_order_acquire))
        return false;   // dropped: the resync on relink catches up
    static int nextForwardId = kForwardIdBase;
    Forward forward;
    forward.id = nextForwardId++;
    forward.name = name;
    forward.fields = fields;
    forward.origin = origin;
    forward.step = step;
    forward.fpsSent = resync::sentFps(name, fields);
    forward.reply = std::move(reply);
    if (forward.reply)
        forward.reply->id = forward.id;
    queue_.push_back(std::move(forward));
    if (pending_++ == 0)
        busySinceMs_ = nowMs();
    // A forwarded record format or preset makes each Core restore its own per-format profile (CC:11213-11239): a resync owed.
    if (origin == Origin::Forward && (name == "set_record_format" || name == "apply_preset"))
        tracker_.onForwardedReconfigure();
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
    takeRefused_ = refused;
    state_.message = message;
    updateRefusedLocked();
}

// The state word's `refused` input: a refused REC, or three resyncs that did not converge (design 4.4, 4.5).
void ControlClient::updateRefusedLocked()
{
    state_.refused = takeRefused_ || tracker_.refused();
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
        watchTake();
        if (takeChangeOwed_.exchange(false, std::memory_order_acq_rel) && options_.onTakeChange)
            options_.onTakeChange();
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
            relinkPoll_ = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.lost = false;
                if (state_.message == "link down")
                    state_.message.clear();
                state_.answered = false;
                state_.sensorMissing = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tracker_.onLink();   // a (re)link owes one resync (design 4.4)
                step1Failed_ = false;
                updateRefusedLocked();
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
    watchTies();
    measureSkew();
    if (pollId_ == 0) {
        pollId_ = kPollIdBase + (nextId_++ & 0xFFFFF);
        pollSentMs_ = nowMs();
        if (!sendLine("{\"cmd\":\"get_state\",\"id\":" + std::to_string(pollId_) + "}"))
            dropLink("send failed");
    }
    serviceResync();
}

// The live check (design 7.6): cam1's preview frames against the primary's, paired by gridPoint() slot. Unknown, and not
// locked, with no primary frames to pair with, at two frame rates, or with too few pairs.
void ControlClient::measureSkew()
{
    skew::Result result;
    if (options_.primaryFrames && options_.frameRate && fd_ >= 0) {
        const resync::PrimaryView view = options_.primaryView ? options_.primaryView() : resync::PrimaryView{};
        std::string cam1Settings;
        bool cam1Locked;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cam1Settings = state_.settings;
            cam1Locked = cam1PhaseLocked_ && state_.answered;
        }
        const auto primaryFps = jsonkeys::numberOf(view.settings, "fps");
        const auto cam1Fps = jsonkeys::numberOf(cam1Settings, "fps");
        const bool equalFps = ties_.fps || (primaryFps && cam1Fps && std::abs(*primaryFps - *cam1Fps) < 0.0005);
        const auto rate = options_.frameRate();
        result = skewMeter_.measure(options_.primaryFrames(), nowMs(), rate.first, rate.second, equalFps,
                                    cam1Locked && (!options_.primaryLocked || options_.primaryLocked()));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.locked = result.locked;
    state_.skewKnown = result.known;
    state_.skewUs = result.known ? result.skewUs : 0;
}

// A change of cam1Settings runs the transition resync (design 4.4): untying resolution alone sends cam1 the ladder off,
// tying anything back resyncs it. The first reading is the baseline.
void ControlClient::watchTies()
{
    if (!options_.ties)
        return;
    // cam1's last known sensor (kept across a relink, so the tie does not flip while cam1 has not answered yet).
    std::string cam1Sensor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cam1Sensor = state_.sensor;
    }
    const commandclasses::Ties now = commandclasses::effectiveTies(options_.ties(), options_.primaryView ? options_.primaryView().sensor : std::string(),
                                                                   cam1Sensor);
    if (tiesKnown_ && now != ties_) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracker_.onTieChange(ties_, now);
    }
    ties_ = now;
    tiesKnown_ = true;
}

// A take that ran on cam0 while cam1 did not record owes cam1 a resync after it (design 4.4, "a take cam1 sat out").
void ControlClient::watchTake()
{
    if (!options_.primaryTakeActive || fd_ < 0)
        return;
    const bool active = options_.primaryTakeActive();
    std::lock_guard<std::mutex> lock(mutex_);
    if (active) {
        if (!prevTakeActive_)
            cam1RecordedThisTake_ = false;
        if (state_.recording)
            cam1RecordedThisTake_ = true;
    } else if (prevTakeActive_ && !cam1RecordedThisTake_) {
        tracker_.onSatOutTake();
    }
    prevTakeActive_ = active;
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

void ControlClient::signalTakeChange()
{
    takeChangeOwed_.store(true, std::memory_order_release);
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
    // A relayed read: hand the line to whoever waits, outside the FIFO and the count.
    if (const auto read = readsInFlight_.find(id); read != readsInFlight_.end()) {
        {
            std::lock_guard<std::mutex> replyLock(read->second->mutex);
            read->second->line = line;
            read->second->done = true;
        }
        read->second->cv.notify_all();
        readsInFlight_.erase(read);
        return;
    }
    if (!inFlight_ || id != flight_.id)
        return;
    const bool ok = jsonkeys::boolOf(line, "ok").value_or(false);
    const Forward done = flight_;
    lastForwardReplyMs_ = nowMs();
    if (done.kind == Kind::Start) {
        // The take's record_start (design 6.2). cam1 sets takePending_ before it answers, so the time since the slot pick is
        // L_cam1, which the lead of the next take is judged against (T64 logs it, T69 measures it on the real cam1).
        const bool ours = takeIn_ && takeClip_ == done.clip;
        if (ok) {
            if (ours) {
                takeConfirmed_ = true;
                takeConfirmedMs_ = nowMs();
                observedRecording_ = true;
                std::cerr << "ApertarCore cam1 L_cam1 " << (nowMs() - takePickedMs_) << " ms (slot pick to cam1's reply, clip "
                          << done.clip << ")\n";
            }
        } else {
            const std::string why = jsonkeys::stringOf(line, "error").value_or("refused");
            state_.message = "CAM1: " + why;
            std::cerr << "ApertarCore cam1 refused the take: " << why << "\n";
            if (ours) {   // cam0 keeps rolling; cam1 never joins mid-take. The hold ends at the stop (takeFinished).
                takeRefused_ = true;
                takeIn_ = false;
                takeConfirmed_ = false;
                takeClip_.clear();
                stopSent_ = false;
            }
            updateRefusedLocked();
        }
        refreshRecordingLocked();
    } else if (done.kind == Kind::Stop) {
        if (!ok)
            state_.message = "CAM1: " + jsonkeys::stringOf(line, "error").value_or("refused");
        // cam1's stop returned: whatever it answered, the take is over for the primary (an orphan stop has no clip).
        observedRecording_ = false;
        if (!done.clip.empty() && takeIn_ && takeClip_ == done.clip)
            endTakeLocked(false, "");
        refreshRecordingLocked();
    } else if (ok) {
        // cam1 clamps an fps above its mode's ceiling and answers ok: say so, and schedule nothing (design 4.4). The compare
        // finds the difference and the 3-resync cap ends the loop.
        std::optional<std::string> clamp;
        if (done.fpsSent > 0) {
            const std::string_view settings = done.name == "apply_preset" ? jsonkeys::rawOf(line, "state").value_or("{}") : std::string_view(line);
            clamp = resync::clampMessage(done.fpsSent, settings);
        }
        if (clamp)
            state_.message = *clamp;
        else if (state_.message.rfind("CAM1: ", 0) == 0 || state_.message.rfind("CAM1 runs ", 0) == 0)
            state_.message.clear();
    } else {
        state_.message = "CAM1: " + jsonkeys::stringOf(line, "error").value_or("refused");
        // A refusal of a forwarded command owes a resync; one inside the resync, or of a camera=cam1 setter, only says why.
        if (done.origin == Origin::Forward)
            tracker_.onNonOkReply();
        if (done.step == 1)
            step1Failed_ = true;
    }
    if (done.reply) {
        {
            std::lock_guard<std::mutex> replyLock(done.reply->mutex);
            done.reply->line = line;
            done.reply->done = true;
        }
        done.reply->cv.notify_all();
    }
    inFlight_ = false;
    // Step 1 refused for another reason than the processed-format skip: it is sent once more after step 2 (design 4.4).
    if (done.step == 2 && step1Failed_) {
        step1Failed_ = false;
        enqueueLocked("set_mode_ladder", step1Fields_, Origin::Relay, 11);
    }
    if (done.kind == Kind::Setter && --pending_ <= 0) {
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
        const int64_t now = nowMs();
        if (const auto timestamp = jsonkeys::numberOf(line, "timestamp_us"))
            skewMeter_.addCam1({static_cast<int64_t>(*timestamp), now});   // relay thread only
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastPreviewFrameMs = now;
    } else if (hasPrefix(line, "{\"event\":\"recording\"")) {
        const auto active = jsonkeys::boolOf(line, "active");
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastRecordingEventMs = nowMs();
        if (active && *active) {
            observedRecording_ = true;
            // A take nobody asked for is judged by the relink rules a moment later, not at the next second's poll (the
            // orphan stop is due within 1 s). The short delay lets cam1 settle its state first.
            if (!(takeIn_ && takeConfirmed_))
                nextTickMs_ = std::min(nextTickMs_, nowMs() + 250);
        } else if (active) {
            observedRecording_ = false;
            // cam1 left a take the primary did not stop (its own buffer safety stop or failure stop): `lost`, design 4.5.
            if (takeIn_ && takeConfirmed_ && !stopSent_)
                endTakeLocked(true, "CAM1 left the take.");
        }
        refreshRecordingLocked();
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
    const int64_t sentMs = pollSentMs_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.standby = standby || camera == "standby";
        observedRecording_ = recording;
        state_.clipName = jsonkeys::stringOf(line, "recording_clip_name").value_or("");
        state_.writing = writes;
        state_.idle = idle;
        state_.answered = true;
        state_.droppedFrames = static_cast<long long>(jsonkeys::numberOf(line, "recording_dropped_frames").value_or(0));
        state_.sensor = jsonkeys::stringOf(line, "sensor").value_or(state_.sensor);
        state_.sensorMono = jsonkeys::boolOf(line, "sensor_mono").value_or(state_.sensorMono);
        cam1PhaseLocked_ = jsonkeys::boolOf(line, "fps_phase_lock_locked").value_or(false);
        if (!run.empty())
            state_.settings = "{" + run + "}";
        doSettle = !settled_;
        TakePoll poll;
        poll.recording = recording;
        poll.clip = state_.clipName;
        poll.firstSlot = jsonkeys::numberOf(line, "recording_first_slot");
        poll.lastSlot = jsonkeys::numberOf(line, "recording_last_slot");
        poll.fps = jsonkeys::numberOf(line, "fps").value_or(0);
        poll.sentMs = sentMs;
        poll.relink = relinkPoll_;
        relinkPoll_ = false;
        applyTakeRulesLocked(poll);
        refreshRecordingLocked();
    }
    if (options_.primaryView) {
        const resync::PrimaryView view = options_.primaryView();
        compareWith(view, sentMs);
    }
    if (!doSettle) {
        serviceResync();
        return;
    }
    settled_ = true;
    unknown_.store(false, std::memory_order_release);
    // §4.6 Standby: on link or relink the primary's standby goes to cam1, and a cam1 left in standby while the primary is
    // awake is woken. Both count as setters (the busy window).
    const bool primaryStandby = options_.primaryInStandby && options_.primaryInStandby();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!primaryStandby && state_.standby)
        enqueueLocked("wake", {}, Origin::Relay);
    else if (primaryStandby && !state_.standby)
        enqueueLocked("enter_standby", {}, Origin::Relay);
}

// The compare (design 4.4): cam1's settingsBody against the primary's as last published, on the tied fields only. It runs
// only while both Cores are idle and only against a get_state sent after the last forward's reply, and a resync follows
// two differing compares in a row.
void ControlClient::compareWith(const resync::PrimaryView &view, int64_t sentMs)
{
    const bool primaryIdle = !options_.primaryIdle || options_.primaryIdle();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!view.valid || !state_.answered || !state_.idle || pending_ > 0 || !primaryIdle || state_.settings.size() <= 2 ||
        !resync::compareEligible(sentMs, lastForwardReplyMs_))
        return;
    const std::vector<std::string> diffs = resync::differences(view.settings, state_.settings, ties_);
    if (!diffs.empty())
        lastDiffs_ = diffs;
    if (tracker_.onCompare(!diffs.empty())) {
        // A compare passed: cam1 follows again.
        if (state_.message.rfind("CAM1 did not follow", 0) == 0)
            state_.message.clear();
    }
    updateRefusedLocked();
}

// One resync at the next moment both Cores are idle (design 4.4). Its steps join the FIFO and the pending count; the
// count goes up first, then the published take is read again, and a take that is starting takes the steps back.
void ControlClient::serviceResync()
{
    if (!linked_.load(std::memory_order_acquire))
        return;
    const bool primaryIdle = !options_.primaryIdle || options_.primaryIdle();
    resync::Kind kind = resync::Kind::None;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.answered)
            return;
        const bool bothIdle = primaryIdle && state_.idle && pending_ == 0;
        kind = tracker_.take(bothIdle);
        if (tracker_.refused() && !state_.refused) {
            std::string why = "CAM1 did not follow CAM0";
            for (size_t i = 0; i < lastDiffs_.size(); ++i)
                why += (i ? ", " : ": ") + lastDiffs_[i];
            state_.message = why + ".";
            std::cerr << "ApertarCore cam1 refused: " << state_.message << "\n";
        }
        updateRefusedLocked();
        if (kind == resync::Kind::None)
            return;
    }
    const resync::PrimaryView view = options_.primaryView ? options_.primaryView() : resync::PrimaryView{};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!view.valid) {   // the primary has published nothing yet: the resync stays owed
        tracker_.giveBack(kind);
        return;
    }
    resync::PlanInputs plan;
    plan.ties = ties_;
    plan.kind = kind;
    plan.primary = view.settings;
    plan.cam1 = state_.settings;
    plan.primaryPreset = view.preset;
    plan.ladderResolutions = view.ladderResolutions;
    plan.ladderKnown = view.ladderKnown;
    plan.ladderEnabled = view.ladderEnabled;
    plan.ladderRampReady = view.ladderRampReady;
    const std::vector<resync::Step> steps = resync::planResync(plan);
    std::cerr << "ApertarCore cam1 resync (" << (kind == resync::Kind::Full ? "full" : "ladder off") << ":";
    for (const resync::Step &step : steps)
        std::cerr << ' ' << step.name;
    std::cerr << ")\n";
    const size_t before = queue_.size();
    const int pendingBefore = pending_;
    const int64_t busyBefore = busySinceMs_;
    step1Fields_.clear();
    for (const resync::Step &step : steps) {
        const int tag = step.name == "set_mode_ladder" ? 1 : step.name == "apply_preset" ? 2 : 3;
        if (tag == 1)
            step1Fields_ = step.fields;
        enqueueLocked(step.name, step.fields, Origin::Relay, tag);
    }
    // The count is up; read the published take once more. A record_start that published its take first wins (design 4.1).
    if (options_.primaryTakeStarting && options_.primaryTakeStarting()) {
        queue_.resize(before);
        pending_ = pendingBefore;
        busySinceMs_ = busyBefore;
        tracker_.giveBack(kind);
        updateRefusedLocked();
    }
}

void ControlClient::sendNextForward()
{
    // Relayed reads go out at once, whatever is in flight.
    while (true) {
        std::string readLine;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reads_.empty())
                break;
            Forward read = std::move(reads_.front());
            reads_.pop_front();
            readLine = forwardLine(read.id, read.name, read.fields);
            readsInFlight_[read.id] = read.reply;
        }
        if (!sendLine(readLine)) {
            dropLink("send failed");
            return;
        }
    }
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
    if (!sendLine(line)) {
        dropLink("send failed");
        return;
    }
    std::shared_ptr<Reply> written;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        written = flight_.written;
    }
    if (written) {
        {
            std::lock_guard<std::mutex> markLock(written->mutex);
            written->done = true;
        }
        written->cv.notify_all();
    }
}

// Everyone waiting for a reply gives up: the link is gone. Called under mutex_.
void ControlClient::failReplies()
{
    const auto fail = [](const std::shared_ptr<Reply> &reply) {
        if (!reply)
            return;
        {
            std::lock_guard<std::mutex> lock(reply->mutex);
            reply->failed = true;
            reply->done = true;
        }
        reply->cv.notify_all();
    };
    for (const Forward &forward : queue_) {
        fail(forward.reply);
        fail(forward.written);
    }
    if (inFlight_) {
        fail(flight_.reply);
        fail(flight_.written);
    }
    for (const Forward &read : reads_)
        fail(read.reply);
    reads_.clear();
    for (auto &entry : readsInFlight_)
        fail(entry.second);
    readsInFlight_.clear();
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
        // A record_stop queued or unanswered becomes a pending stop (design 4.1, 4.6): it is sent first at the relink.
        bool takeStopLost = false;
        const auto keepAsPending = [&](const Forward &forward) {
            if (forward.kind != Kind::Stop || forward.clip.empty())
                return;
            if (Stopped *entry = findStoppedLocked(forward.clip))
                entry->sentMonoUs = 0;
            if (takeIn_ && forward.clip == takeClip_)
                takeStopLost = true;
        };
        for (const Forward &forward : queue_)
            keepAsPending(forward);
        if (inFlight_)
            keepAsPending(flight_);
        failReplies();
        queue_.clear();
        inFlight_ = false;
        pending_ = 0;
        busySinceMs_ = 0;
        step1Failed_ = false;
        state_.answered = false;
        state_.locked = false;
        state_.skewKnown = false;
        state_.idle = false;
        state_.standby = false;
        state_.writing = false;
        if (why && takeStopLost) {
            endTakeLocked(true, "link down");   // the stop is kept; the take state ends at once (design 4.3)
        } else if (why && takeIn_) {
            state_.lost = true;                 // the take state stays `recording`, so the tally stays on (design 4.3)
            state_.message = "link down";
        }
        if (!why) {
            observedRecording_ = false;
            takeIn_ = false;
            takeClip_.clear();
        }
        refreshRecordingLocked();
        idleCv_.notify_all();
    }
    if (why)
        std::cerr << "ApertarCore cam1 link down (" << why << ")\n";
}

// ---- the take with cam1 (design 4.3, 4.6, 6.2) -------------------------------------------------------------------------

// state_.recording is the primary's take state for cam1 or what cam1 itself reports, whichever says recording: while
// either does, the card is in use. Called under mutex_ after any change to either.
void ControlClient::refreshRecordingLocked()
{
    state_.recording = takeIn_ || observedRecording_;
    state_.inTake = takeIn_;
    state_.takeCam1Only = takeIn_ && takeCam1Only_;
    state_.stopping = takeIn_ && stopSent_;
    recordingNow_.store(state_.recording, std::memory_order_release);
}

ControlClient::Stopped *ControlClient::findStoppedLocked(const std::string &clip)
{
    for (auto it = stopped_.rbegin(); it != stopped_.rend(); ++it)
        if (it->clip == clip)
            return &*it;
    return nullptr;
}

// The take ended for the primary: cam1 answered the stop, left on its own, or the link took the stop away. `lost` marks
// an end nobody asked for. A cam1-only take also owes the App its tally and `recording` event (takeEvents()).
void ControlClient::endTakeLocked(bool lost, const std::string &reason)
{
    const bool cam1Only = takeCam1Only_;
    takeIn_ = false;
    takeConfirmed_ = false;
    stopSent_ = false;
    takeClip_.clear();
    takeCam1Only_ = false;
    if (lost) {
        state_.lost = true;
        if (!reason.empty())
            state_.message = reason;
    }
    if (cam1Only) {
        events_.ended = true;
        events_.endReason = reason;
        signalTakeChange();
    }
    refreshRecordingLocked();
}

bool ControlClient::enqueueTakeLineLocked(Kind kind, const std::string &name, const std::map<std::string, std::string> &fields,
                                          bool front, const std::string &clip, std::shared_ptr<Reply> reply,
                                          std::shared_ptr<Reply> written)
{
    if (!linked_.load(std::memory_order_acquire))
        return false;
    static int nextTakeId = 1 << 23;
    Forward forward;
    forward.id = nextTakeId++;
    forward.kind = kind;
    forward.clip = clip;
    forward.name = name;
    forward.fields = fields;
    forward.origin = Origin::Relay;
    forward.reply = std::move(reply);
    forward.written = std::move(written);
    if (forward.reply)
        forward.reply->id = forward.id;
    if (front)
        queue_.push_front(std::move(forward));
    else
        queue_.push_back(std::move(forward));
    wake();
    return true;   // not counted: a take's own line never makes cam1 busy (design 4.3)
}

void ControlClient::publishTake(const std::string &clipName, bool cam1Only, int64_t startUs, int64_t pickedMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    takeClip_ = clipName;
    takeIn_ = true;
    takeCam1Only_ = cam1Only;
    takeConfirmed_ = false;
    takeConfirmedMs_ = 0;
    takeStartUs_ = startUs;
    takePickedMs_ = pickedMs;
    startJudged_ = false;
    stopSent_ = false;
    hadTake_ = true;
    state_.lost = false;
    refreshRecordingLocked();
}

void ControlClient::noteTake()
{
    std::lock_guard<std::mutex> lock(mutex_);
    hadTake_ = true;
}

std::shared_ptr<ControlClient::Reply> ControlClient::sendStart(const std::map<std::string, std::string> &fields)
{
    auto reply = std::make_shared<Reply>();
    const auto clip = fields.find("clip_name");
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueueTakeLineLocked(Kind::Start, "record_start", fields, false, clip == fields.end() ? std::string() : clip->second,
                                 reply, nullptr)
        ? reply
        : nullptr;
}

ControlClient::StopOutcome ControlClient::stopTake(int64_t stopUs, int waitMs)
{
    StopOutcome out;
    std::shared_ptr<Reply> written;
    std::string clip;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        takeRefused_ = false;
        updateRefusedLocked();
        if (!takeIn_) {
            // cam1 is not in the take (it left, or never joined): nothing to send, and a settled `lost` ends with the take.
            if (linked_.load(std::memory_order_acquire))
                state_.lost = false;
            return out;
        }
        out.inTake = true;
        clip = takeClip_;
        Stopped entry;
        entry.clip = clip;
        entry.startUs = takeStartUs_;
        entry.stopUs = stopUs;
        if (!linked_.load(std::memory_order_acquire)) {
            // A pending stop (design 4.6): sent first at the relink. The take state ends now; the word stays `lost`.
            stopped_.push_back(entry);
            while (stopped_.size() > kStoppedKeep)
                stopped_.pop_front();
            out.pending = true;
            const bool cam1Only = takeCam1Only_;
            takeIn_ = false;
            takeConfirmed_ = false;
            stopSent_ = false;
            takeClip_.clear();
            takeCam1Only_ = false;
            (void)cam1Only;
            refreshRecordingLocked();
            return out;
        }
        out.reply = std::make_shared<Reply>();
        written = std::make_shared<Reply>();
        // Ahead of anything queued, unless the take's own start is still queued: the stop must not pass it.
        const bool ahead = takeConfirmed_;
        enqueueTakeLineLocked(Kind::Stop, "record_stop", {{"stop_us", std::to_string(stopUs)}}, ahead, clip, out.reply, written);
        entry.sentMonoUs = monoUs();
        stopped_.push_back(entry);
        while (stopped_.size() > kStoppedKeep)
            stopped_.pop_front();
        stopSent_ = true;
        out.sent = true;
        refreshRecordingLocked();
    }
    // Bounded by the caller: the stop lead minus 2T (design 6.2). The relay thread signals the mark when the line is written.
    out.written = waitMs > 0 ? written->wait(waitMs, nullptr) : false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out.written)
        if (const Stopped *entry = findStoppedLocked(clip); entry && entry->sentMonoUs == 0) {
            out.pending = true;   // the link dropped while we waited: dropLink kept the stop
            out.sent = false;
        }
    return out;
}

void ControlClient::abandonTake(int64_t stopUs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (takeClip_.empty())
        return;
    Stopped entry;
    entry.clip = takeClip_;
    entry.startUs = takeStartUs_;
    entry.stopUs = stopUs;
    // Behind the take's own start, which may still be queued or in flight: cam1 then stops what it started.
    if (enqueueTakeLineLocked(Kind::Stop, "record_stop", {{"stop_us", std::to_string(stopUs)}}, false, entry.clip, nullptr, nullptr))
        entry.sentMonoUs = monoUs();
    stopped_.push_back(entry);
    while (stopped_.size() > kStoppedKeep)
        stopped_.pop_front();
    takeIn_ = false;
    takeConfirmed_ = false;
    stopSent_ = false;
    takeClip_.clear();
    takeCam1Only_ = false;
    refreshRecordingLocked();
}

void ControlClient::withdrawTake()
{
    std::lock_guard<std::mutex> lock(mutex_);
    takeIn_ = false;
    takeConfirmed_ = false;
    stopSent_ = false;
    takeClip_.clear();
    takeCam1Only_ = false;
    takeRefused_ = false;
    updateRefusedLocked();
    refreshRecordingLocked();
}

void ControlClient::takeFinished()
{
    std::lock_guard<std::mutex> lock(mutex_);
    takeRefused_ = false;
    updateRefusedLocked();
    if (linked_.load(std::memory_order_acquire) && !takeIn_)
        state_.lost = false;
}

ControlClient::TakeEvents ControlClient::takeEvents()
{
    std::lock_guard<std::mutex> lock(mutex_);
    TakeEvents out = std::move(events_);
    events_ = TakeEvents{};
    return out;
}

std::string ControlClient::takeClip() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return takeIn_ ? takeClip_ : std::string();
}

// The relink rules (design 4.6), at the first answer after a link and at every poll. `poll` is cam1's get_state.
void ControlClient::applyTakeRulesLocked(const TakePoll &poll)
{
    const int64_t nowMsValue = nowMs();
    const int64_t nowMono = monoUs();
    const std::pair<int, int> rate = recordingFrameRate(poll.fps > 0 ? poll.fps : 25.0);
    const takeslots::Rate slotRate{rate.first, rate.second};
    const auto lateWord = [](int64_t frames) { return std::to_string(frames) + (frames == 1 ? " frame late" : " frames late"); };

    // Stops the primary kept for a take cam1 is not recording any more are moot.
    if (!poll.recording) {
        for (Stopped &entry : stopped_) {
            if (entry.sentMonoUs == 0)
                entry.sentMonoUs = nowMono;
            // "CAM1 stopped N frames late": cam1's last slot against the one the stop_us named (design 6.2).
            if (!entry.reported && poll.lastSlot && entry.clip == poll.clip && slotRate.valid()) {
                entry.reported = true;
                const int64_t late = *poll.lastSlot > takeslots::expectedLastSlot(entry.stopUs, slotRate)
                                         ? static_cast<int64_t>(*poll.lastSlot) - takeslots::expectedLastSlot(entry.stopUs, slotRate)
                                         : 0;
                if (late > 0) {
                    state_.message = "CAM1 stopped " + lateWord(late);
                    std::cerr << "ApertarCore cam1 " << state_.message << " (clip " << entry.clip << ")\n";
                }
            }
        }
        // The take is in the primary's book but cam1 does not record it: it left unasked, or it restarted and its start wait
        // let it come up during a cam1-only take (design 3.4, 4.6 "cam1 back without its take"). Only a poll sent after the
        // start was answered can say so; a stop in flight ends the take by its reply.
        if (takeIn_ && takeConfirmed_ && !stopSent_ && poll.sentMs >= takeConfirmedMs_) {
            std::cerr << "ApertarCore cam1 " << (poll.relink ? "is back without its take" : "left the take") << " (clip " << takeClip_
                      << ")\n";
            endTakeLocked(true, poll.relink ? "CAM1 restarted and lost the take." : "CAM1 left the take.");
        }
        return;
    }

    // cam1 records. Whose take is it?
    if (takeIn_ && poll.clip == takeClip_) {
        // Rule 1: the running take. A rejoin after a link drop sends no event and no pulse; the gap is in the journal.
        if (poll.relink) {
            std::cerr << "ApertarCore cam1 rejoined the take (clip " << takeClip_ << ")\n";
        }
        state_.lost = false;
        if (state_.message == "link down")
            state_.message.clear();
        if (!startJudged_ && poll.firstSlot && slotRate.valid()) {
            startJudged_ = true;
            const int64_t late = takeslots::framesLate(static_cast<int64_t>(*poll.firstSlot),
                                                       takeslots::expectedFirstSlot(takeStartUs_, slotRate));
            if (late > 0) {
                state_.message = "CAM1 started " + lateWord(late);
                std::cerr << "ApertarCore cam1 " << state_.message << " (clip " << takeClip_ << ")\n";
            }
        }
        return;
    }
    if (Stopped *entry = findStoppedLocked(poll.clip)) {
        // Rule 2: a take the primary has stopped. A pending stop goes out now (a stop_us already past stops at once); a stop
        // already sent is given until its stop_us plus the grace before the orphan rule applies.
        if (entry->sentMonoUs == 0) {
            if (enqueueTakeLineLocked(Kind::Stop, "record_stop", {{"stop_us", std::to_string(entry->stopUs)}}, true, entry->clip, nullptr,
                                      nullptr)) {
                entry->sentMonoUs = nowMono;
                std::cerr << "ApertarCore cam1 sent the pending stop (clip " << entry->clip << ")\n";
            }
            return;
        }
        if (nowMono < entry->stopUs + kStopGraceUs || nowMsValue - orphanSentMs_ < kOrphanRepeatMs)
            return;
        // past the grace: the orphan rule below
    } else if (!takeIn_ && takeClip_.empty() && !hadTake_) {
        // Rule 3: no take running and this process has had none since it started: it restarted or crashed during a cam1-only
        // take. Adopt it (tally, record_cameras=cam1, a `recording` event with adopted:true and no pulse); STOP then ends it.
        takeIn_ = true;
        takeCam1Only_ = true;
        takeConfirmed_ = true;
        takeConfirmedMs_ = nowMsValue;
        takeClip_ = poll.clip;
        takeStartUs_ = 0;
        startJudged_ = true;
        stopSent_ = false;
        hadTake_ = true;
        state_.lost = false;
        events_.adopted = true;
        events_.adoptedClip = poll.clip;
        signalTakeChange();
        std::cerr << "ApertarCore cam1 adopted the take in progress (clip " << poll.clip << ")\n";
        return;
    }
    // Rule 4: the orphan stop, at once. Not repeated for 2 s while cam1 answers it.
    if (nowMsValue - orphanSentMs_ < kOrphanRepeatMs)
        return;
    orphanSentMs_ = nowMsValue;
    if (enqueueTakeLineLocked(Kind::Stop, "record_stop", {}, true, std::string(), nullptr, nullptr))
        std::cerr << "ApertarCore cam1 orphan stop (clip " << poll.clip << ")\n";
}

} // namespace apertar
