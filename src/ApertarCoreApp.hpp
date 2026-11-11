#pragma once

#include "camera/CameraController.hpp"
#include "audio/AudioRecordingSession.hpp"
#include "hardware/GpioInput.hpp"
#include "hardware/StandbyPowerButtonInput.hpp"
#include "hardware/HardwareScriptRunner.hpp"
#include "hardware/GroveAnalogInput.hpp"
#include "hardware/NeoRotary4Input.hpp"
#include "hardware/Pcf8574Input.hpp"
#include "hardware/RecordingIndicatorOutput.hpp"
#include "hardware/TriggerOutput.hpp"
#include "ipc/ControlProtocol.hpp"
#include "ipc/ControlServer.hpp"
#include "web/WebServer.hpp"
#include "util/MediaProfile.hpp"
#include "util/StoragePreroll.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace apertar {

namespace hwconf { struct Document; }

class ApertarCoreApp {
public:
    struct Options {
        std::string socketPath = "/tmp/apertar-core.sock";
        std::string mediaRoot = "/media/RAW";
        std::string webBindAddress = "0.0.0.0";
        uint16_t webPort = 8000;
        std::string camera;     // --camera: "" or cam0 = the primary; cam1 or an I2C node (i2c@88000) = a secondary
        bool simulateCamera = false;
    };

    explicit ApertarCoreApp(Options options);
    ~ApertarCoreApp();

    bool start();
    void stop();
    void wait();
    void requestShutdown();

private:
    friend struct HardwareInputTestAccess;
    struct WebPreviewSlotMapping {
        int fd = -1;
        void *mapping = nullptr;
        size_t mappingLength = 0;
        uint8_t *data = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        int planeCount = 0;
        uint32_t planeOffsets[3] = { 0, 0, 0 };
        uint32_t planePitches[3] = { 0, 0, 0 };
        uint32_t planeLengths[3] = { 0, 0, 0 };
    };

    struct PendingWebPreviewFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t frameId = 0;
        int64_t timestampUs = 0;
        uint32_t planeOffsets[3] = { 0, 0, 0 };
        uint32_t planeStrides[3] = { 0, 0, 0 };
        std::vector<uint8_t> yuv;
    };

    std::string handleRequest(const std::string &line);
    std::string handleWebCommand(const std::string &payload);
    std::string handleCommand(const ControlCommand &command);
    std::optional<std::string> handleReadOnlyHardwareCommand(const ControlCommand &command);
    std::string testInputReply(const ControlCommand &command);
    std::string gpioPeripheralJson(const hwconf::Document &doc);
    std::string i2cPeripheralJson(const hwconf::Document &doc, const std::string &section, const std::string &title,
                                  int defaultAddress, bool coreOn, const std::string &error);
    WebServer::Response handleWebRequest(const WebServer::Request &request);
    std::string buildWebStateJson() const;
    std::string buildPresetLibraryJson() const;
    std::string buildMediaLibraryJson() const;
    void enqueuePreviewFrame(PreviewFrameDescriptor frame);
    void enqueueCurrentPreviewRegistrations();
    void updateWebPreviewRegistration(const PreviewFrameDescriptor &frame);
    void captureWebPreviewFrame(const PreviewFrameDescriptor &frame);
    void publishEvent(const std::string &eventJson);
    void handleTriggerEvent(const std::string &eventJson);
    void publishPreviewFrame(const PreviewFrameDescriptor &frame);
    void runPreviewPublisher();
    void stopPreviewPublisher();
    void runMjpegEncoder();
    void stopMjpegEncoder();
    void streamMjpeg(int clientFd);
    void syncRecordingTracking(bool active);
    double recordingElapsedSeconds() const;
    bool startSoundRecording(AudioRecordingOptions audioOptions, std::string *errorMessage);
    void stopSoundRecording();
    bool soundRecordingActive() const;
    bool startMediaSpeedTest(std::string *errorMessage);
    void runMediaSpeedTest(uint64_t testMiB, std::string mountPath);
    void joinMediaSpeedTest();
    void runMediaWatcher();
    void stopMediaWatcher();
    void runBufferEvents();
    void runBufferSafetyStop();
    // Storage pre-roll (CineMate's warm-up): requestStoragePreroll only requests; the thread runs it.
    storagepreroll::GuardDecision storagePrerollGuard(storagepreroll::Trigger trigger) const;
    void requestStoragePreroll(storagepreroll::Trigger trigger, std::optional<unsigned> seconds);
    void runStoragePreroll();
    void runStoragePrerollOnce(storagepreroll::Trigger trigger, unsigned seconds);
    void stopStoragePreroll();
    MediaSnapshot mediaSnapshot() const;
    std::string mediaGateError() const;
    bool mediaGateBypassed() const;
    std::string playbackHoldReason(bool withYield = true) const;   // "" or recording, writing, media_test, standby, media
    void applyMediaTuning(const MediaProfile &profile, bool apply);
    // Trim of the recording drive (root helper apertar-media-trim). trimMedia() only requests; one worker thread runs it.
    void trimMedia(const std::string &reason);
    void trimMediaAfterDelete(uint64_t freeBefore);
    void runMediaTrim();
    void stopMediaTrim();
    std::string mediaTrimStateJson() const;
    bool mediaTrimMayRun() const;
    void waitForMediaTrim(const std::string &commandName, int blockMs);
    // Merge worker for split clips ([storage] frames_per_folder, cdng/ClipMerge.hpp), on the trim worker's pattern:
    // clipMerge() only requests a scan; one thread (nice 10) renames part_*/ frames back into the clip root while the
    // camera is idle, re-checking clipMergeMayRun() before every rename; waitForClipMerge() pauses it for a take.
    void clipMerge(const std::string &reason);
    void runClipMerge();
    void stopClipMerge();
    bool clipMergeMayRun(std::string *why = nullptr) const;
    void waitForClipMerge(const std::string &commandName, int blockMs);
    std::string clipMergeStateJson() const;
    bool clipMergeIsActive(const std::string &clipName) const;   // the merge worker is moving this clip's frames now
    void endClipMergeRename();
    enum class ClipMergeOutcome { Done, Stopped, Shutdown };
    ClipMergeOutcome mergeSplitClip(const std::filesystem::path &clip);
    MediaProfile mediaProfile() const;
    void handleHardwareInputAction(const std::string &action, bool buttonPress = true);
    bool handlePowerButtonPress(bool doublePress);
    void handleSingleHardwareInputAction(const std::string &action);
    void handleHardwareAnalogInput(const GroveAnalogInput::AnalogEvent &event);
    std::string dispatchHardwareInputCommand(ControlCommand command);
    std::string toggleSharedUiBoolAction(const std::string &action,
                                         const std::string &commandName,
                                         bool currentValue);

    Options options_;
    CameraController camera_;
    ControlServer controlServer_;
    WebServer webServer_;
    GpioInput gpioInput_;
    StandbyPowerButtonInput standbyPowerButton_;
    GroveAnalogInput groveAnalogInput_;
    NeoRotary4Input neoRotary4Input_;
    Pcf8574Input pcf8574Input_;
    HardwareScriptRunner hardwareScriptRunner_;
    RecordingIndicatorOutput recordingIndicatorOutput_;
    TriggerOutput triggerOutput_;
    AudioRecordingSession soundRecorder_;
    std::atomic<bool> soundRecordingActive_{false};
    std::atomic<bool> storagePrerollFormatRunning_{false};   // a format runs; the watcher's mount change is not an insert
    std::atomic<int64_t> storagePrerollFormatDoneMs_{0};     // trimNowMs() when the last format ended (0 = none)
    std::atomic<bool> mediaDeleteActive_{false};   // T50, Q23: a media delete is running; record_start waits
    std::atomic<size_t> controlsErrors_{0};        // T37: what the validator found in the hardware file, shown in state
    std::atomic<size_t> controlsWarnings_{0};

    // T39: the controls worker applies a saved hardware file by restarting only the driver groups whose sections
    // changed, on its own thread (never inside a command), and waits for a take to end first.
    void runControlsWorker();
    void wakeControlsWorker();
    void applyHardwareText(const std::string &text);
    bool recordingGateActive();
    std::string controlsApplyJson();
    std::thread controlsThread_;
    std::mutex controlsWorkerMutex_;
    std::condition_variable controlsWake_;
    bool controlsWakeRequested_ = false;
    std::atomic<bool> controlsWorkerRunning_{false};
    std::string appliedHardwareText_;             // what the drivers were started from (controlsWorkerMutex_)
    std::string controlsApplyState_ = "applied";  // applied, waiting (a take runs), applying
    std::string controlsApplyDetail_;
    std::map<std::string, uint64_t> pendingUnitSince_;   // unit -> CLOCK_MONOTONIC us of the change it has not seen
    std::atomic<uint64_t> cfeHatGeneration_{0};

    // T42: the last control that moved (Listen), as section|key.
    void noteInputSeen(const std::string &sectionKey);
    std::mutex lastInputMutex_;
    std::string lastInput_;
    uint64_t lastInputSeq_ = 0;
    std::chrono::steady_clock::time_point lastInputAt_{};
    std::string soundRecordingFolder_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
    std::mutex hardwareActionMutex_;
    std::mutex hardwareInputSequenceMutex_;
    std::mutex commandMutex_;
    std::atomic<int64_t> lastUserActivityMs_{0};
    std::atomic<bool> standbyUsbPoweredOff_{false};

    std::mutex eventMutex_;
    bool collectingCommandEvents_ = false;
    std::vector<std::string> pendingCommandEvents_;

    std::mutex previewMutex_;
    std::condition_variable previewCv_;
    std::deque<PreviewFrameDescriptor> pendingPreviewRegistrations_;
    std::optional<PreviewFrameDescriptor> pendingPreviewFrame_;
    std::optional<PreviewFrameDescriptor> lastPreviewFrame_;
    std::unordered_map<uint32_t, PreviewFrameDescriptor> registeredPreviewFrames_;
    bool previewPublisherRunning_ = false;
    std::thread previewThread_;

    std::mutex mjpegMutex_;
    std::condition_variable mjpegCv_;
    std::unordered_map<uint32_t, WebPreviewSlotMapping> webPreviewSlots_;
    std::optional<PendingWebPreviewFrame> pendingMjpegFrame_;
    std::vector<uint8_t> recycledMjpegYuv_;
    std::vector<uint8_t> latestMjpegFrame_;
    uint64_t latestMjpegGeneration_ = 0;
    bool mjpegEncoderRunning_ = false;
    std::thread mjpegThread_;
    std::atomic<uint32_t> mjpegClientCount_{0};
    std::chrono::steady_clock::time_point lastWebPreviewCaptureAt_{};

    mutable std::mutex telemetryMutex_;
    mutable bool cpuSampleValid_ = false;
    mutable uint64_t lastCpuIdleTicks_ = 0;
    mutable uint64_t lastCpuTotalTicks_ = 0;
    // The media card's write rate and temperature (NVMe only), under telemetryMutex_.
    mutable std::string mediaStatDevice_;
    mutable bool mediaStatValid_ = false;
    mutable uint64_t lastMediaSectorsWritten_ = 0;
    mutable std::chrono::steady_clock::time_point lastMediaStatAt_{};
    mutable std::optional<double> mediaWriteMBps_;
    mutable std::chrono::steady_clock::time_point mediaTempAt_{};
    mutable std::optional<double> mediaTempC_;
    mutable std::optional<double> mediaTempMaxC_;
    mutable std::optional<double> mediaTempCritC_;
    mutable std::string mediaCardLabel_;
    bool recordingStartKnown_ = false;
    std::chrono::steady_clock::time_point recordingStartedAt_{};

    mutable std::mutex mediaSpeedMutex_;
    bool mediaSpeedTestRunning_ = false;
    bool mediaSpeedCurrentUsedFio_ = false;
    bool mediaSpeedLastUsedFio_ = false;
    uint64_t mediaSpeedCurrentBytes_ = 0;
    uint64_t mediaSpeedLastBytes_ = 0;
    double mediaSpeedLastWriteSpeedMBps_ = 0.0;
    std::string mediaSpeedError_;
    std::thread mediaSpeedThread_;

    // Media watcher: probes the mount root on its own thread (statvfs can block
    // on a hung device) and publishes the result; see runMediaWatcher().
    std::thread mediaWatcherThread_;
    std::atomic<bool> mediaWatcherRunning_{false};
    mutable std::mutex mediaStateMutex_;
    std::condition_variable mediaWatcherCv_;
    MediaSnapshot mediaSnapshot_;
    bool mediaSnapshotValid_ = false;
    MediaProfile mediaProfile_;

    // The "buffer" event thread (runBufferEvents): own thread, no controller lock; stopped in stopMediaWatcher.
    std::thread bufferEventThread_;
    std::atomic<bool> bufferEventsRunning_{false};
    std::mutex bufferEventMutex_;
    std::condition_variable bufferEventCv_;

    // The buffer safety stop thread (runBufferSafetyStop): started and joined with bufferEventThread_. The write queue's
    // hook only sets these atomics and notifies the cv; the thread performs the record_stop (may take the command path).
    std::thread bufferSafetyStopThread_;
    std::atomic<bool> bufferSafetyStopRunning_{false};
    std::atomic<bool> bufferSafetyStopPending_{false};
    std::atomic<uint64_t> bufferSafetyStopUsed_{0};
    std::atomic<uint64_t> bufferSafetyStopCapacity_{0};
    std::atomic<bool> bufferSafetyStopRam_{false};
    std::mutex bufferSafetyStopMutex_;
    std::condition_variable bufferSafetyStopCv_;

    // The storage pre-roll thread (runStoragePreroll): requests coalesce under the mutex; the stop flag ends a running
    // pre-roll early (record_stop, shutdown) and its clip is still deleted.
    std::thread storagePrerollThread_;
    std::atomic<bool> storagePrerollRunning_{false};
    std::atomic<bool> storagePrerollStop_{false};
    std::mutex storagePrerollMutex_;
    std::condition_variable storagePrerollCv_;
    bool storagePrerollPending_ = false;
    bool storagePrerollManual_ = false;
    storagepreroll::Trigger storagePrerollTrigger_ = storagepreroll::Trigger::Manual;
    unsigned storagePrerollSeconds_ = 2;
    int storagePrerollDelayMs_ = 0;

    // Media trim worker. mediaTrimMutex_ is only ever held for short state changes, never while the helper runs and
    // never together with commandMutex_ in the worker, so handleCommand may wait on it while holding commandMutex_.
    std::thread mediaTrimThread_;
    std::atomic<bool> mediaTrimWorkerRunning_{false};
    std::atomic<bool> mediaTrimSetting_{true};      // the persisted switch (CaptureSettings::mediaTrimEnabled); env beats it
    mutable std::mutex mediaTrimMutex_;
    std::condition_variable mediaTrimCv_;
    bool mediaTrimPending_ = false;
    std::string mediaTrimReason_;
    bool mediaTrimBusy_ = false;                    // the helper process is running
    bool mediaTrimWorking_ = false;                 // a request is being served (queued for an idle medium, or running)
    std::atomic<pid_t> mediaTrimPid_{0};
    std::atomic<bool> mediaTrimKilled_{false};
    std::atomic<int64_t> mediaTrimBlockUntilMs_{0}; // a take or media action began: start no new trim until then
    bool mediaTrimUnsupported_ = false;             // exit 3, remembered until the mount id changes
    int mediaTrimUnsupportedMountId_ = -1;
    int mediaTrimFailures_ = 0;
    // Merge worker state. clipMergeMutex_ guards the strings and the request; never held across a rename.
    std::thread clipMergeThread_;
    std::atomic<bool> clipMergeWorkerRunning_{false};
    mutable std::mutex clipMergeMutex_;
    std::condition_variable clipMergeCv_;
    bool clipMergeRequested_ = false;
    std::string clipMergeReason_;
    std::string clipMergeActive_;                   // the clip being merged ("" when none)
    std::string clipMergeLast_;                     // the last clip's result line
    std::atomic<unsigned> clipMergePending_{0};     // clips with a .parts marker at the last scan
    std::atomic<bool> clipMergeRenaming_{false};    // one rename is in flight
    std::atomic<int64_t> clipMergeBlockUntilMs_{0}; // a take or media action began: no rename until then
    // Progress (KB clip-merge-progress-plan.md), counted from the disk at each scan: a clip's frames in its root are
    // done, the ones in its parts are left, so it comes back after a power-off. Under clipMergeMutex_: the scan's
    // clips in order and the one being merged. Atomics: the clip being merged and its renames this run (time left).
    struct ClipMergeCount { std::string name; uint64_t inRoot = 0; uint64_t total = 0; };
    std::vector<ClipMergeCount> clipMergeQueue_;
    size_t clipMergeQueueIndex_ = 0;
    std::string clipMergePaused_;                   // why the merge waits ("" while it renames)
    std::atomic<uint64_t> clipMergeClipDone_{0};
    std::atomic<uint64_t> clipMergeClipTotal_{0};
    std::atomic<uint64_t> clipMergeRunRenames_{0};
    std::atomic<uint64_t> clipMergeRunRenameNs_{0};
    int mediaTrimBackoffRounds_ = 0;
    int64_t mediaTrimBackoffUntilMs_ = 0;
    std::string mediaTrimLastResult_ = "none";
    uint64_t mediaTrimLastBytes_ = 0;
    uint64_t mediaTrimLastMs_ = 0;
    int64_t mediaTrimLastUnix_ = 0;
};

} // namespace apertar
