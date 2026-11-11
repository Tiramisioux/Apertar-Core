#pragma once
// The relay's link to cam1 (dual-sensor design §4.1): the client half of ControlServer, on its own thread.
//
// It connects to cam1's control socket, sends the primary's "both" commands through a FIFO (one in flight, replies
// matched by id), polls cam1's get_state every second and reads the events it keeps. The thread never takes the
// primary's commandMutex_ and never calls a CameraController accessor that takes the controller's mutex_: everything it
// needs from the primary comes through the std::function options below, which read published flags and files.
//
// With the Dual sensor switch off (UI.conf dualSensorEnabled, amendment A1) the thread does one stat() of UI.conf per
// second and nothing else: no socket, no log line, configured() stays false.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace apertar {

// cam1's control socket (design 3.4). The secondary's unit sets APERTAR_SOCKET_PATH to it.
inline constexpr const char *kCam1SocketPath = "/tmp/apertar-core-cam1.sock";

class ControlClient {
public:
    struct Options {
        std::string socketPath;                                // cam1's control socket
        std::string ownSocketPath;                             // this Core's own, for the same-inode check
        std::function<bool()> switchOn;                        // UI.conf dualSensorEnabled
        std::function<bool(const std::string &)> configNames;  // config.txt names a sensor on this port ("cam0", "cam1")
        std::function<bool()> sensorPresent;                   // sysfs shows a sensor on cam1's port
        std::function<bool()> primaryInStandby;                // published by the command thread
    };

    // What the command thread and the state builders read. A copy, taken under the client's own mutex.
    struct Snapshot {
        bool configured = false;
        bool linked = false;
        bool lost = false;
        bool sensorMissing = false;
        bool recording = false;
        bool writing = false;
        bool refused = false;
        bool standby = false;
        bool idle = false;                  // cam1's last get_state met the §4.3 idle test
        bool answered = false;              // cam1 has answered a get_state since the link
        int pending = 0;
        int64_t busyForMs = 0;
        std::string message;
        long long droppedFrames = 0;
        std::string settings = "{}";
        std::string sensor;
        bool sensorMono = false;
        int64_t lastRecordingEventMs = 0;   // steady clock, ms; 0 = never
        int64_t lastBufferEventMs = 0;
        int64_t lastMediaWriteEventMs = 0;
        int64_t lastPreviewFrameMs = 0;
    };

    ControlClient() = default;
    ~ControlClient();
    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;

    void start(Options options);
    void stop();

    // Lock-free reads for the command thread's fast path.
    bool configured() const { return configured_.load(std::memory_order_acquire); }
    // The "cam1 unknown" flag of §3.4: set at start when config.txt names a cam1 sensor or its socket exists (with the
    // switch on); cleared by cam1's first get_state answer, or after 2 s of failed connects of any kind.
    bool unknown() const { return unknown_.load(std::memory_order_acquire); }
    bool linked() const { return linked_.load(std::memory_order_acquire); }

    Snapshot snapshot() const;

    // Queues a forward and counts it (the pending count of §4.3). False when there is no link: the forward is dropped,
    // and the resync on relink catches up (§4.1).
    bool enqueue(const std::string &name, const std::map<std::string, std::string> &fields);

    // The busy wait: blocks up to `ms` until the pending count is 0. True when it is.
    bool waitForIdle(int ms);

    // The next two are for part 2's resync: cam1.message and the `refused` input of the state word.
    void setMessage(const std::string &message);
    void setRefused(bool refused, const std::string &message);

private:
    struct Forward {
        int id = 0;
        std::string name;
        std::map<std::string, std::string> fields;
    };

    void run();
    void tick();
    void ioStep(int timeoutMs);
    int connectSocket() const;
    bool sameInodeAsOwn() const;
    bool sendLine(const std::string &line);
    void readSome();
    void handleLine(const std::string &line);
    void handleReply(const std::string &line);
    void handleEvent(const std::string &line);
    void handleGetState(const std::string &line);
    void sendNextForward();
    void dropLink(const char *why);
    void wake();
    bool idleToUnlink() const;
    bool enqueueLocked(const std::string &name, const std::map<std::string, std::string> &fields);

    Options options_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int wakeFds_[2] = {-1, -1};

    // Owned by the relay thread.
    int fd_ = -1;
    std::string rx_;
    bool everLinked_ = false;
    bool acceptedEver_ = false;     // cam1's socket accepted a connection since the switch went on (A1)
    int pollId_ = 0;                // the get_state in flight, 0 = none
    int nextId_ = 1 << 20;          // ids for what the relay sends; cam1 echoes them
    int64_t startMs_ = 0;
    int64_t nextTickMs_ = 0;
    bool settled_ = false;          // the relink actions (standby, wake) have run for this link

    std::atomic<bool> configured_{false};
    std::atomic<bool> unknown_{false};
    std::atomic<bool> linked_{false};

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::deque<Forward> queue_;
    bool inFlight_ = false;
    Forward flight_;
    int pending_ = 0;
    int64_t busySinceMs_ = 0;
    Snapshot state_;                // everything but configured, linked, pending, busyForMs, which are derived
};

} // namespace apertar
