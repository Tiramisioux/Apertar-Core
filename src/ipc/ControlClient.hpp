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
#include "ipc/CommandClasses.hpp"
#include "ipc/Resync.hpp"
#include "ipc/Skew.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        // The primary's side of the resync and the compare (design 4.4). All of them are read on the relay thread and
        // must not take commandMutex_ or the controller's mutex_: the command thread publishes what they return.
        std::function<commandclasses::Ties()> ties;            // UI.conf cam1Settings, parsed
        std::function<resync::PrimaryView()> primaryView;      // settingsBody, preset and ladder, as last published
        std::function<bool()> primaryIdle;                     // the 4.3 idle test on cam0, from published flags
        std::function<bool()> primaryTakeActive;               // a take (or pre-roll) runs on cam0
        std::function<bool()> primaryTakeStarting;             // a record_start is publishing its take (T62)
        // The live sync check (design 7.6): the primary's recent preview frames, its frame rate as a fraction and its
        // phase-lock verdict, all from published atomics. Unset: the check stays unknown.
        std::function<std::vector<skew::Stamp>()> primaryFrames;
        std::function<std::pair<int64_t, int64_t>()> frameRate;   // recordingFrameRate(fps)
        std::function<bool()> primaryLocked;
        // The relay's take state moved without a command (cam1 left the take, a take was adopted, a stop was answered). The
        // App drains takeEvents() on its own thread; this only wakes it. Called on the relay thread, with no lock held.
        std::function<void()> onTakeChange;
    };

    // Where a queued command came from: a "both" command forwarded after the primary's ok, a camera=cam1 setter, or one of
    // the relay's own steps. A non-ok reply to the first schedules a resync; the others only set cam1.message.
    enum class Origin { Forward, Camera, Relay };

    // A reply the command thread waits for (the ladder pair with camera=cam1, 1 s).
    struct Reply {
        int id = 0;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool failed = false;   // the link dropped first
        std::string line;
        // True with `line` set when the reply came.
        bool wait(int ms, std::string *out);
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
        bool stopping = false;              // a record_stop to cam1 is sent and unanswered (reads `writing`, §4.5)
        bool inTake = false;                // cam1 is in the primary's running take (the take state of §4.5)
        bool takeCam1Only = false;          // ... and the take is cam1's alone
        std::string clipName;               // cam1's recording_clip_name from its last get_state
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
        bool locked = false;                // both Cores locked at equal fps, skew within the bound (7.6)
        bool skewKnown = false;             // enough frame pairs to say; skew_us is null while false
        double skewUs = 0;                  // median cam1 - cam0 of the last second
    };

    ControlClient() = default;
    ~ControlClient();
    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;

    void start(Options options);
    void stop();

    // What the relay owes the App about the take, drained by takeEvents(): a take adopted after a primary restart (§4.6
    // rule 3), or a cam1-only take that ended without the command thread asking (a loss, cam1 back without its take, a
    // stop answered after the press returned).
    struct TakeEvents {
        bool adopted = false;
        std::string adoptedClip;
        bool ended = false;
        std::string endReason;
    };
    // The answer to a stop (design 6.2).
    struct StopOutcome {
        bool inTake = false;      // cam1 was in the take
        bool sent = false;        // the line is queued for cam1 (the link is up)
        bool pending = false;     // the link is down: kept as a pending stop for the relink
        bool written = false;     // the relay wrote the line to the socket within the wait
        std::shared_ptr<Reply> reply;   // cam1's answer to the stop
    };

    // Lock-free reads for the command thread's fast path.
    bool configured() const { return configured_.load(std::memory_order_acquire); }
    // The "cam1 unknown" flag of §3.4: set at start when config.txt names a cam1 sensor or its socket exists (with the
    // switch on); cleared by cam1's first get_state answer, or after 2 s of failed connects of any kind.
    bool unknown() const { return unknown_.load(std::memory_order_acquire); }
    bool linked() const { return linked_.load(std::memory_order_acquire); }

    Snapshot snapshot() const;

    // Queues a forward and counts it (the pending count of §4.3). False when there is no link: the forward is dropped,
    // and the resync on relink catches up (§4.1).
    bool enqueue(const std::string &name, const std::map<std::string, std::string> &fields, Origin origin = Origin::Forward);
    // The same for a command whose reply the caller reads (set_mode_ladder camera=cam1). Null when there is no link.
    std::shared_ptr<Reply> enqueueWithReply(const std::string &name, const std::map<std::string, std::string> &fields);
    // A relayed read (get_mode_ladder camera=cam1): sent at once, outside the FIFO and the pending count. Null when there
    // is no link. Call forget() after a timeout.
    std::shared_ptr<Reply> readNow(const std::string &name, const std::map<std::string, std::string> &fields);
    void forget(const std::shared_ptr<Reply> &reply);

    // The busy wait: blocks up to `ms` until the pending count is 0. True when it is.
    bool waitForIdle(int ms);

    // cam1.message, and the `refused` input of the state word for a refused REC (until the stop, T62). The resync's own
    // `refused` (three resyncs that did not converge) is kept apart and ORed in.
    void setMessage(const std::string &message);
    void setRefused(bool refused, const std::string &message);

    // ---- the take with cam1 (design 4.3, 4.6, 6.2) --------------------------------------------------------------------
    // All of these run on the command thread. The relay thread reads the same state under mutex_ and applies the relink
    // rules at every poll. None of them waits on the relay except stopTake(), bounded by its argument.
    //
    // True while cam1 is in the primary's running take, or reports a take of its own that no rule has dealt with yet:
    // `cam1RecordingActive()` (P7). Lock-free.
    bool recordingNow() const { return recordingNow_.load(std::memory_order_acquire); }
    // Publishes the take with cam1 in it (the clip_name sent cam1, verbatim), before the press reads the pending count
    // and before the relay sends the record_start (design 4.1 Locks). startUs/stopUs are the boundaries sent to cam1 (its
    // own grid); pickedMs is the steady-clock ms of the slot pick, for L_cam1.
    void publishTake(const std::string &clipName, bool cam1Only, int64_t startUs, int64_t pickedMs);
    // A take that has no cam1 in it has started: this process has had a take (design 4.6 rule 3).
    void noteTake();
    // Queues the take's record_start behind any setter. Not counted as pending. Null when there is no link.
    std::shared_ptr<Reply> sendStart(const std::map<std::string, std::string> &fields);
    // Stops cam1's side of the take on `stopUs` (design 6.2): the line goes ahead of the FIFO, and the call waits up to
    // `waitMs` until the relay has written it. With the link down the stop is kept as a pending stop and the take state
    // ends at once. Also ends a `refused`/`lost` hold of the take.
    StopOutcome stopTake(int64_t stopUs, int waitMs);
    // A start that got no answer, or cam0 failing to start after cam1 was told: cam1 is sent record_stop with `stopUs`
    // (the take's start_us, so a late start stops at once) and the take is remembered as stopped.
    void abandonTake(int64_t stopUs);
    // Nothing went to cam1: the published take is withdrawn (a refusal after the publish).
    void withdrawTake();
    // A take without cam1 in it, or any take, ended: the `refused` hold and a settled `lost` end with it.
    void takeFinished();
    TakeEvents takeEvents();
    // The clip_name of the running take cam1 is in, "" when none.
    std::string takeClip() const;

private:
    enum class Kind { Setter, Start, Stop };

    struct Forward {
        int id = 0;
        Kind kind = Kind::Setter;           // Start and Stop are the take's own lines: not counted as pending
        std::string clip;                   // Stop: the take it ends
        std::string name;
        std::map<std::string, std::string> fields;
        Origin origin = Origin::Forward;
        int step = 0;                       // a resync step: 1 ladder, 2 preset, 3 Clear HDR, 11 the ladder again
        double fpsSent = 0;                 // for the clamp rule
        std::shared_ptr<Reply> reply;       // someone waits for it
        std::shared_ptr<Reply> written;     // Stop: the caller waits until the line is on the socket
    };

    // A take the primary has stopped, with the stop cam1 owes (design 4.6, "What the primary remembers").
    struct Stopped {
        std::string clip;
        int64_t startUs = 0;
        int64_t stopUs = 0;
        int64_t sentMonoUs = 0;             // when the stop was sent; 0 = not sent yet (a pending stop)
        bool reported = false;              // "CAM1 stopped N frames late" has been judged
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
    bool enqueueLocked(const std::string &name, const std::map<std::string, std::string> &fields, Origin origin = Origin::Forward,
                       int step = 0, std::shared_ptr<Reply> reply = nullptr);
    void watchTake();
    void watchTies();
    void serviceResync();
    void compareWith(const resync::PrimaryView &view, int64_t sentMs);
    void updateRefusedLocked();
    void refreshRecordingLocked();
    bool enqueueTakeLineLocked(Kind kind, const std::string &name, const std::map<std::string, std::string> &fields, bool front,
                               const std::string &clip, std::shared_ptr<Reply> reply, std::shared_ptr<Reply> written);
    struct TakePoll;
    void applyTakeRulesLocked(const TakePoll &poll);
    void endTakeLocked(bool lost, const std::string &reason);
    Stopped *findStoppedLocked(const std::string &clip);
    void signalTakeChange();
    void failReplies();
    void measureSkew();

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
    int64_t pollSentMs_ = 0;        // when the get_state in flight was sent
    commandclasses::Ties ties_;
    bool tiesKnown_ = false;
    bool prevTakeActive_ = false;
    bool cam1RecordedThisTake_ = false;
    std::vector<std::string> lastDiffs_;   // the fields the last differing compare found
    skew::Meter skewMeter_;                // relay thread only
    bool cam1PhaseLocked_ = false;         // cam1's get_state: fps_phase_lock_locked (under mutex_)
    int64_t nextTickMs_ = 0;
    bool settled_ = false;          // the relink actions (standby, wake) have run for this link

    std::atomic<bool> configured_{false};
    std::atomic<bool> unknown_{false};
    std::atomic<bool> linked_{false};
    std::atomic<bool> recordingNow_{false};
    std::atomic<bool> takeChangeOwed_{false};

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::deque<Forward> queue_;
    bool inFlight_ = false;
    Forward flight_;
    int pending_ = 0;
    int64_t busySinceMs_ = 0;
    Snapshot state_;                // everything but configured, linked, pending, busyForMs, which are derived
    resync::Tracker tracker_;       // under mutex_
    bool takeRefused_ = false;      // setRefused(), for a refused REC
    int64_t lastForwardReplyMs_ = 0;
    bool step1Failed_ = false;      // resync step 1 was refused: it is sent once more after step 2
    std::map<std::string, std::string> step1Fields_;
    std::deque<Forward> reads_;     // relayed reads waiting to be sent
    std::map<int, std::shared_ptr<Reply>> readsInFlight_;

    // The take with cam1, under mutex_ (design 4.6). The primary's take state for cam1 is `takeIn_`; `observedRecording_`
    // is what cam1 itself reports. `state_.recording` is their OR.
    std::string takeClip_;
    bool takeIn_ = false;
    bool takeCam1Only_ = false;
    bool takeConfirmed_ = false;            // cam1 answered the record_start ok
    int64_t takeConfirmedMs_ = 0;
    int64_t takeStartUs_ = 0;
    int64_t takePickedMs_ = 0;
    bool startJudged_ = false;              // "CAM1 started N frames late" has been judged for this take
    bool stopSent_ = false;
    bool hadTake_ = false;                  // this process has had a take since it started
    bool observedRecording_ = false;
    bool relinkPoll_ = false;               // the next get_state is the first after a (re)link (relay thread)
    int64_t orphanSentMs_ = 0;
    std::deque<Stopped> stopped_;
    TakeEvents events_;
};

} // namespace apertar
