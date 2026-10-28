#pragma once

#include "camera/FramePhaseLock.hpp"
#include "camera/ModeLadder.hpp"
#include <apertar/core/Types.hpp>
#include "util/RecordingDropWarning.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace apertar {

class CameraController {
public:
    using EventCallback = std::function<void(const std::string &)>;
    using PreviewCallback = std::function<void(PreviewFrameDescriptor)>;
    using AudioLevelCallback = std::function<void(int)>;
    struct StillExposureStatus {
        bool active = false;
        double remainingSeconds = 0;
    };
    struct MediaWriteStatus {
        bool pending = false;
        uint64_t revision = 0;
    };
    // One sensor mode as libcamera configured it in the startup probe.
    struct SensorMode {
        std::string format;         // e.g. "SRGGB12_CSI2P"
        uint32_t bitDepth = 0;
        Resolution size{};
        bool clearHdr = false;      // listed with wide_dynamic_range on
        double maxFps = 0;          // from FrameDurationLimits after configure()
        std::string crop;           // ScalerCropMaximum, sensor pixels
        uint32_t binning = 0;       // 0 = unknown; see SensorBinning
        std::string binningSource;  // "driver", "inferred" or "unknown"
    };
    // Binning of a configured mode. "driver": the driver's crop rectangle says it (crop width
    // over output width). "inferred": the driver gives no window (crop = output size, or none),
    // so it is guessed from the active area's width. "unknown": nothing to go on.
    struct SensorBinning {
        uint32_t factor = 0;
        std::string source = "unknown";
        std::string crop;           // ScalerCropMaximum, empty if the pipeline has none
    };

    CameraController(EventCallback eventCallback, PreviewCallback previewCallback);
    ~CameraController();

    bool start(const std::string &mediaRoot, bool simulateCamera);
    void stop();
    bool enterStandby();
    bool wakeFromStandby();

    bool setResolution(Resolution resolution);
    bool setFps(double fps);
    bool setIso(uint32_t iso);
    bool setIsoAuto(bool enabled = true); // false: lock at the gain in use
    bool setShutterUs(uint32_t shutterUs);
    bool setShutterAuto();
    bool setExposureCompensation(double ev);
    bool setExposureMetering(const std::string &mode);
    bool setStillExposureMode(bool photoMode, bool longExposure, double timelapseIntervalSeconds = 0);
    bool setWhiteBalanceKelvin(uint32_t kelvin);
    bool setWhiteBalanceAuto();
    bool setRecordingFormat(RecordingFormat format);
    bool setCdngCompression(CdngCompression compression);
    bool setLossyDngRatio(LossyDngRatio ratio);
    bool setProresProfile(ProresProfile profile);
    bool setProresColorProfile(ProresColorProfile profile);
    bool setCdngPreviewColorProfile(ProresColorProfile profile);
    bool setTuningFile(const std::string &path);
    bool setRawBitDepth(uint32_t bitDepth);
    bool setClearHdr(bool enabled);
    bool setHdrThresholdHigh(uint32_t value);
    bool setHdrThresholdLow(uint32_t value);
    bool setHdrBlend(uint32_t value);
    bool setHdrGainAdder(uint32_t value);
    bool setH264Resolution(Resolution resolution);
    bool setTimecodeMode(TimecodeMode mode);
    bool setConformFps(double fps);
    bool setFpsPhaseLock(bool enabled);
    bool setFpsSpeedRamp(bool enabled, bool keepExposure, bool stretchAudio);
    // T34: the mode ladder. Any of rungs / enabled / rampReady; dryRun only validates. Fills the
    // get_mode_ladder JSON in *json; false with *error when refused (in a take, or errors).
    bool setModeLadder(const std::optional<std::string> &rungs, std::optional<bool> enabled,
                       std::optional<bool> rampReady, bool dryRun, std::string *json, std::string *error);
    std::string modeLadderJson() const;
    // A preset that names a mode (namesMode) must be the one the ladder would pick for its FPS;
    // one that names none follows the ladder. False with *error otherwise.
    bool applyLadderToPreset(CaptureSettings &settings, bool namesMode, std::string *error) const;
    bool setAutofocusMode(AutofocusMode mode);
    bool setAutofocusRange(AutofocusRange range);
    bool setAutofocusSpeed(AutofocusSpeed speed);
    bool setAutofocusWindow(double x, double y, double width, double height);
    bool applyCapturePreset(const CaptureSettings &settings, const std::function<bool()> &commitUi = {});
    bool checkRecordingFailure();
    bool flagMediaFault(const std::string &message, bool mediumGone);
    std::string recordingFaultName() const;
    bool mediaWritesPending() const;
    MediaWriteStatus mediaWriteStatus() const;
    StillExposureStatus stillExposureStatus() const;
    std::string photoWriteError() const;

    bool capturePhoto(PhotoCaptureFormat format = PhotoCaptureFormat::Dng, unsigned count = 1);
    bool startTimelapse(PhotoCaptureFormat format, double intervalSeconds);
    bool stopTimelapse();
    bool timelapseActive() const;
    double timelapseIntervalSeconds() const;
    uint64_t timelapseShotCount() const;
    bool startRecording(const AudioRecordingOptions &audioOptions = {},
                        const GyroRecordingOptions &gyroOptions = {});
    bool stopRecording();

    CameraState state() const;
    CaptureSettings settings() const;
    std::string sensorName() const;
    std::vector<SensorMode> sensorModes() const;
    SensorBinning sensorBinning() const;
    bool isRecording() const;
    std::string lastError() const;
    std::string recordingError() const;
    PhaseLockStatus phaseLockStatus() const;
    bool recordingHasDroppedFrames() const { return recordingDropWarning_.dropped(); }

private:
    class CameraSession;

    bool applySettingsLocked(CaptureSettings nextSettings, bool requiresReconfigure, bool commit = true);
    bool applyFpsStepLocked(double effectiveFps, bool rampingTake, bool commit, const CaptureSettings *rung = nullptr);
    // T34: what the ladder knows of each rung in base's format, the active test, the rewrite of a
    // request into the rung its FPS picks, the derived state fields, the file.
    std::vector<ladder::RungState> ladderStatesLocked(const ladder::Ladder &ladder, const CaptureSettings &base) const;
    bool ladderActiveForLocked(const CaptureSettings &base) const;
    bool ladderRewriteLocked(CaptureSettings &next) const;
    void fillLadderFieldsLocked(CaptureSettings &out) const;
    void saveModeLadderLocked() const;
    std::string modeLadderJsonLocked(const ladder::Ladder &ladder, const std::vector<std::string> &errors,
                                     const std::vector<std::string> &warnings) const;
    void restoreFpsAfterRampLocked();
    bool commitHdrKnobsLocked(const CaptureSettings &next);
    void persistSettingsLocked();
    void restoreModeGeometryLocked(CaptureSettings &next) const;
    bool validateResolution(Resolution resolution);
    void setErrorLocked(std::string error);
    void emitSettingsEventLocked();
    void emitRecordingEventLocked();
    bool queuePhotoCaptureLocked(PhotoCaptureFormat format, const std::string &basePath,
                                 std::function<void()> onSaved = {}, unsigned count = 1);
    void completeTimelapseShot(uint64_t sequence);
    void emitTimelapseProgressLocked();
    void timelapseLoop();
    void emitTimelapseEvent(bool active, double intervalSeconds, const std::string &folderPath);
    void emitTimelapseFrameEvent(uint64_t frameIndex);

    mutable std::mutex mutex_;
    mutable std::mutex timelapseMutex_;
    std::condition_variable timelapseCv_;
    std::thread timelapseThread_;
    EventCallback eventCallback_;
    PreviewCallback previewCallback_;
    RecordingDropWarning recordingDropWarning_;
    CameraState state_ = CameraState::Offline;
    CaptureSettings settings_{};
    std::map<RecordingFormat, CaptureSettings> formatSettings_;
    std::optional<CaptureSettings> stillSettings_;
    std::string mediaRoot_;
    std::string lastError_;
    bool simulateCamera_ = false;
    bool recording_ = false;
    bool recordingFailureReported_ = false;
    std::string recordingFailureMessage_;
    std::string externalFailureMessage_;
    bool externalMediumGone_ = false;
    std::string recordingFaultName_;
    bool timelapseStopRequested_ = false;
    bool timelapseActive_ = false;
    double timelapseIntervalSeconds_ = 0;
    uint64_t timelapseFrameIndex_ = 1;
    uint64_t timelapseShotCount_ = 0;
    uint64_t timelapseSequence_ = 0;
    PhotoCaptureFormat timelapseFormat_ = PhotoCaptureFormat::Dng;
    std::string timelapseFolderPath_;
    // Speed ramping (T31): the exposure the operator set, kept across clamps while
    // keep-exposure ramps shorten it; the take's start rate, restored at stop.
    uint32_t rampNominalShutterUs_ = 0;
    double rampTakeBaseFps_ = 0.0;
    bool rampTakeChanged_ = false;
    ladder::Ladder modeLadder_;   // T34, from mode_ladder.conf
    std::unique_ptr<CameraSession> session_;
};

} // namespace apertar
