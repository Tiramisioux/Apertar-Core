#include "hardware/Cef168Lens.hpp"

#include "hardware/HardwareConfigStore.hpp"
#include "hardware/HardwareConfigText.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <fstream>
#include <map>
#include <thread>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace apertar::hardware::cef168 {

namespace {

constexpr uint32_t kCidData = V4L2_CID_USER_BASE | 169;   // cef168.h: custom_data (lens_id 168, data 169, calibrate 170)
constexpr uint32_t kCidCalibrate = V4L2_CID_USER_BASE | 170;

std::string errnoText(const std::string &what, int error)
{
    return what + ": " + std::strerror(error);
}

const char *typeName(uint32_t type)
{
    switch (type) {
    case V4L2_CTRL_TYPE_INTEGER: return "integer";
    case V4L2_CTRL_TYPE_BOOLEAN: return "boolean";
    case V4L2_CTRL_TYPE_MENU: return "menu";
    case V4L2_CTRL_TYPE_BUTTON: return "button";
    case V4L2_CTRL_TYPE_INTEGER64: return "integer64";
    case V4L2_CTRL_TYPE_STRING: return "string";
    case V4L2_CTRL_TYPE_BITMASK: return "bitmask";
    case V4L2_CTRL_TYPE_INTEGER_MENU: return "integer_menu";
    case V4L2_CTRL_TYPE_U8: return "u8";
    case V4L2_CTRL_TYPE_U16: return "u16";
    case V4L2_CTRL_TYPE_U32: return "u32";
    default: return "other";
    }
}

// "Focus, Absolute" -> "focus_absolute"
std::string nameKey(const std::string &text)
{
    std::string key;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!key.empty() && key.back() != '_') key += '_';
    }
    while (!key.empty() && key.back() == '_') key.pop_back();
    return key;
}

} // namespace

namespace {

struct Found {
    std::string path;
    int bus = -1;
};

std::optional<Found> findSubdev()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/video4linux", ec)) {
        const std::string base = entry.path().filename().string();
        if (base.rfind("v4l-subdev", 0) != 0) continue;
        std::ifstream in(entry.path() / "name");
        std::string name;
        std::getline(in, name);
        if (const auto bus = busOfSubdevName(name)) return Found{"/dev/" + base, *bus};
    }
    return std::nullopt;
}

bool extScalar(int fd, uint32_t id, long long *value, bool write, int *error)
{
    v4l2_ext_control control{};
    control.id = id;
    if (write) control.value = static_cast<__s32>(*value);
    v4l2_ext_controls controls{};
    controls.which = V4L2_CTRL_ID2WHICH(id);
    controls.count = 1;
    controls.controls = &control;
    if (::ioctl(fd, write ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &controls) < 0) {
        if (error) *error = errno;
        return false;
    }
    if (!write) *value = control.value;
    return true;
}

std::optional<Data> readFrame(int fd, std::string *error)
{
    uint8_t buffer[kFrameLength] = {};
    v4l2_ext_control control{};
    control.id = kCidData;
    control.size = kFrameLength;
    control.ptr = buffer;
    v4l2_ext_controls controls{};
    controls.which = V4L2_CTRL_ID2WHICH(kCidData);
    controls.count = 1;
    controls.controls = &control;
    // EIO: the board does not answer or failed the driver's own CRC check (nothing fitted).
    if (::ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) < 0) {
        if (error) *error = errnoText("Reading the lens data failed", errno);
        return std::nullopt;
    }
    return decode(buffer, kFrameLength, error);
}

Control describe(const v4l2_query_ext_ctrl &query, int fd)
{
    Control control;
    control.id = query.id;
    control.name = query.name;
    control.type = typeName(query.type);
    control.minimum = query.minimum;
    control.maximum = query.maximum;
    control.step = static_cast<long long>(query.step);
    control.defaultValue = query.default_value;
    control.readOnly = (query.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0;
    control.writeOnly = (query.flags & V4L2_CTRL_FLAG_WRITE_ONLY) != 0;
    control.isVolatile = (query.flags & V4L2_CTRL_FLAG_VOLATILE) != 0;
    control.executeOnWrite = (query.flags & V4L2_CTRL_FLAG_EXECUTE_ON_WRITE) != 0;
    const bool scalar = !(query.flags & V4L2_CTRL_FLAG_HAS_PAYLOAD) && query.type != V4L2_CTRL_TYPE_INTEGER64 &&
        query.type != V4L2_CTRL_TYPE_STRING && query.type != V4L2_CTRL_TYPE_BUTTON;
    if (scalar && !control.writeOnly) {
        long long value = 0;
        if (extScalar(fd, query.id, &value, false, nullptr)) {
            control.value = value;
            // A volatile read can move the control's range (the focus range comes from the lens): ask again.
            v4l2_query_ext_ctrl again{};
            again.id = query.id;
            if (::ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &again) == 0) {
                control.minimum = again.minimum;
                control.maximum = again.maximum;
            }
        }
    }
    return control;
}

std::vector<Control> listControls(int fd)
{
    std::vector<Control> controls;
    v4l2_query_ext_ctrl query{};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (::ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &query) == 0) {
        if (query.type != V4L2_CTRL_TYPE_CTRL_CLASS && !(query.flags & V4L2_CTRL_FLAG_DISABLED))
            controls.push_back(describe(query, fd));
        const uint32_t next = query.id;
        query = {};
        query.id = next | V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    return controls;
}

} // namespace

namespace {
// Writes one scalar control; "" or what went wrong.
std::string writeControl(int fd, uint32_t id, long long value)
{
    int error = 0;
    if (!extScalar(fd, id, &value, true, &error)) return errnoText("Writing the lens control failed", error);
    return {};
}

using Clock = std::chrono::steady_clock;
constexpr auto kCalibrationTimeout = std::chrono::seconds(60);   // a full sweep takes a few seconds; this is generous
constexpr auto kCalibrationNoFlag = std::chrono::seconds(3);     // the board never raised `calibrating`: nothing ran
constexpr auto kCalibrationMinimum = std::chrono::milliseconds(100);   // calibrate.cpp: never stop sooner than this
constexpr auto kCalibrationPoll = std::chrono::microseconds(500);

const char *kNoStartMessage = "The board did not start a calibration. Set the lens switch to AF and try again.";
const char *kNoSweepMessage = "The lens did not sweep its focus. Set the lens switch to AF (three AF/MF flips start the "
    "board's self-test) and try again.";

// What one run of Pinefeat's poll loop found.
struct CalibrationResult {
    bool timedOut = false;
    bool readFailed = false;
    bool sawFlag = false;           // the board raised `calibrating` at least once
    std::string readError;
    std::map<double, int> points;   // inverse distance (m^-1) -> position; the smaller position wins a duplicate distance
    int positionMin = INT_MAX, positionMax = INT_MIN;   // the positions seen while calibrating == 2
    unsigned distanceMinCm = UINT_MAX, distanceMaxCm = 0;
    unsigned movingTimeMs = 0;      // from the last frame, as calibrate.cpp
    std::optional<Data> last;       // the last frame: its range is the board's own after the run
};

std::string distanceText(unsigned cm)
{
    return cm == kInfiniteCm ? std::string("\xe2\x88\x9e") : jsonNum(cm / 100.0) + " m";
}

double roundInverse(double value) { return std::round(value * 100000.0) / 100000.0; }

// calibrate.cpp's loop: poll Data every 0.5 ms until `calibrating` is 0 and at least 100 ms went by. `fd` is the worker's own.
CalibrationResult runPoll(int fd, const std::atomic<bool> &stop)
{
    CalibrationResult result;
    const auto start = Clock::now();
    int failures = 0;
    while (!stop) {
        std::this_thread::sleep_for(kCalibrationPoll);
        const auto elapsed = Clock::now() - start;
        std::string error;
        const auto data = readFrame(fd, &error);
        if (!data) {
            if (++failures >= 400) {   // a board that stopped answering (~ a second of nothing but errors)
                result.readFailed = true;
                result.readError = error;
                break;
            }
            if (elapsed >= kCalibrationTimeout) { result.timedOut = true; break; }
            continue;
        }
        failures = 0;
        result.movingTimeMs = data->movingTime;
        result.last = data;
        if (data->calibrating != 0) result.sawFlag = true;
        if (data->calibrating == 2) {
            result.positionMin = std::min(result.positionMin, data->focusCurrent);
            result.positionMax = std::max(result.positionMax, data->focusCurrent);
            if (data->distanceMinCm != 0) {
                result.distanceMinCm = std::min(result.distanceMinCm, data->distanceMinCm);
                result.distanceMaxCm = std::max(result.distanceMaxCm, data->distanceMinCm);
                const double inverse = 100.0 / static_cast<double>(data->distanceMinCm);
                const auto [it, added] = result.points.try_emplace(inverse, data->focusCurrent);
                if (!added && data->focusCurrent < it->second) it->second = data->focusCurrent;
            }
        }
        if (data->calibrating == 0 && elapsed >= kCalibrationMinimum && (result.sawFlag || elapsed >= kCalibrationNoFlag)) break;
        if (elapsed >= kCalibrationTimeout) { result.timedOut = true; break; }
    }
    return result;
}
} // namespace

struct Cef168Lens::Impl {
    int fd = -1;
    std::string path;

    bool loaded = false;
    Library library;
    std::filesystem::path libraryPath;

    bool detected = false;      // a lens (id != 0) answered and the library has chosen for it
    int lastLensId = -1;
    bool irisApplied = false;   // the working lens's iris went to the lens
    std::optional<double> iris; // the last f-number sent to the lens
    std::string summary = statusSummaryJson(Status{});

    serial::Cef168Serial serial;    // the board's UART: focal length and aperture range; its own thread
    Status last;                    // the last look, kept fresh by refresh(); summary and the recording's snapshot come from it
    LensMetadata published;
    bool dirty = false;             // the summary changed since the quick tick last told the caller
    std::function<bool()> gate;     // the camera is idle: the quick tick may touch the I2C bus
    std::function<void()> notify;
    std::thread fast;
    std::mutex fastMutex;
    std::condition_variable fastWake;
    bool fastStop = false;

    Calibration calibration;
    std::mutex *outer = nullptr;    // Cef168Lens::mutex_: the worker takes it only to store its result
    std::thread worker;             // the calibration poll loop; joined before a new run and in the destructor
    std::atomic<bool> stopWorker{false};

    void close()
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(fastMutex);
            fastStop = true;
        }
        fastWake.notify_all();
        if (fast.joinable()) fast.join();
        stopWorker = true;
        if (worker.joinable()) worker.join();
        close();
    }

    Library &lib()
    {
        if (!loaded) {
            const char *override = std::getenv("APERTAR_LENS_LIBRARY");   // tests
            libraryPath = override && *override ? std::filesystem::path(override)
                                                : HardwareConfigStore::defaultNotesDirectory() / "lenses.conf";
            library = loadFile(libraryPath);
            loaded = true;
        }
        return library;
    }
    std::string save() { return saveFile(libraryPath, library); }

    Settings settings()
    {
        Settings settings = loadStepSettings();
        const Entry &working = lib().working;
        settings.apertureMin = working.apertureMin;
        settings.apertureMax = working.apertureMax;
        settings.iris = working.lastIris;
        const serial::Values live = serial.values();
        if (live.connected && live.lensConnected) settings.liveAperture = live.apertureRange;
        return settings;
    }

    // Called with the outer mutex held: the summary and the recording's lens snapshot follow `last`.
    void commit()
    {
        const std::string text = statusSummaryJson(last);
        if (text != summary) {
            summary = text;
            dirty = true;
        }
        const LensMetadata meta = lensMetadata(last);
        if (!(meta == published)) {
            published = meta;
            publishLensMetadata(meta);
        }
    }

    // After something changed outside a look (the iris, the library, the serial port's values).
    void recompute()
    {
        last.iris = iris;
        last.library = lib();
        last.settings = settings();
        serial.configure(last.settings.serialDevice);
        last.serial = serial.values();
        commit();
    }

    // The quick tick: with `i2c` the data frame is read again (nothing else touches the bus), then the rest is recomputed.
    void refresh(bool i2c)
    {
        if (!last.present) return;
        if (i2c && fd >= 0 && last.data && calibration.state != "running" && last.data->calibrating == 0)
            if (const auto data = readFrame(fd, nullptr)) last.data = data;
        serial.setPaused(calibration.state == "running" || (last.data && last.data->calibrating != 0));
        recompute();
    }

    void fastLoop()
    {
        std::unique_lock<std::mutex> wait(fastMutex);
        while (!fastStop) {
            fastWake.wait_for(wait, std::chrono::milliseconds(250), [this] { return fastStop; });
            if (fastStop) break;
            wait.unlock();
            const bool idle = gate ? gate() : false;
            bool fire = false;
            {
                std::unique_lock<std::mutex> lock(*outer, std::try_to_lock);   // never queue behind a long call
                if (lock.owns_lock()) {
                    refresh(idle);
                    fire = dirty;
                    dirty = false;
                }
            }
            if (fire && notify) notify();
            wait.lock();
        }
    }

    // Worker thread: owns `workerFd` (a second open of the subdev, so the 0.3 s sweep never holds the main fd or mutex_),
    // polls, then stores the verdict under the lock.
    void runCalibration(int workerFd)
    {
        const CalibrationResult result = runPoll(workerFd, stopWorker);
        ::close(workerFd);
        if (stopWorker) return;
        std::lock_guard<std::mutex> lock(*outer);
        finishCalibration(result);
    }

    // Success = at least 2 PWL points and positions that moved. Core's own verdict: the board's min/max (0..65535 on a
    // Canon EF-S 10-22, always) says nothing about it. Called with the outer mutex held.
    void finishCalibration(const CalibrationResult &r)
    {
        Calibration &cal = calibration;
        cal.finishedAt = nowIso();
        cal.movingTimeMs = r.movingTimeMs;
        Entry &working = lib().working;
        const bool swept = r.points.size() >= 2 && r.positionMax > r.positionMin;
        // The board holds the real range after a run (0..1192 on a Canon EF-S 10-22; 0..65535 before it): that counts too.
        const bool boardRange = r.last && r.last->rangeValid();
        if (r.timedOut) {
            cal.state = "failed";
            cal.message = "Calibration did not finish in 60 s. Check the lens switch is on AF and try again.";
        } else if (r.readFailed) {
            cal.state = "failed";
            cal.message = "The board stopped answering during the calibration" + (r.readError.empty() ? std::string() : ": " + r.readError);
        } else if (!r.sawFlag) {
            cal.state = "failed";
            cal.message = kNoStartMessage;
        } else if (!swept && !boardRange) {
            cal.state = "failed";
            cal.message = kNoSweepMessage;
            working.capFocus = false;
            working.capAutofocus = false;
        } else {
            cal.state = "done";
            const int lowest = swept ? r.positionMin : r.last->focusMin;
            const int highest = swept ? r.positionMax : r.last->focusMax;
            const unsigned nearest = swept ? r.distanceMinCm : r.last->distanceMinCm;
            const unsigned farthest = swept ? r.distanceMaxCm : r.last->distanceMaxCm;
            cal.focusMin = lowest;
            cal.focusMax = highest;
            cal.distanceMinCm = nearest;
            cal.distanceMaxCm = farthest;
            cal.pwlMap.clear();
            for (const auto &[inverse, position] : r.points) {
                cal.pwlMap.push_back(roundInverse(inverse));
                cal.pwlMap.push_back(position);
            }
            cal.stepFrames = r.movingTimeMs < 300 ? 4u : r.movingTimeMs < 350 ? 5u : 6u;
            cal.message = "Calibrated: focus " + std::to_string(lowest) + " to " + std::to_string(highest) + ", " +
                distanceText(nearest) + " to " + distanceText(farthest) + ", " + std::to_string(r.points.size()) + " points.";
            working.focusMin = lowest;
            working.focusMax = highest;
            working.distanceMinCm = nearest;
            working.distanceMaxCm = farthest;
            working.pwlMap = cal.pwlMap;
            working.stepFrames = cal.stepFrames;
            working.capFocus = true;
        }
        save();
    }

    Status read(bool controls)
    {
        Status status;
        auto finish = [&]() {
            status.library = lib();
            status.calibration = calibration;
            status.settings = settings();
            status.iris = iris;
            serial.configure(status.settings.serialDevice);
            status.serial = serial.values();
            serial.setPaused(calibration.state == "running" || (status.data && status.data->calibrating != 0));
            if (status.data && status.data->lensId != 0) serial.setLensHint(status.data->lensId);
            last = status;
            commit();
            return status;
        };
        const auto found = findSubdev();
        if (!found) {
            close();
            detected = false;
            irisApplied = false;
            return finish();
        }
        status.present = true;
        status.subdev = found->path;
        status.bus = found->bus;
        if (fd < 0 || path != found->path) {
            close();
            path = found->path;
            fd = ::open(found->path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                status.error = errnoText("Cannot open " + found->path, errno);
                return finish();
            }
        }
        status.data = readFrame(fd, &status.error);
        if (!status.data) {
            detected = false;   // nothing answering: whatever is mounted next is looked up again
            irisApplied = false;
            return finish();
        }
        const Data &data = *status.data;
        if (data.lensId == 0) {   // no lens mounted (assumed)
            detected = false;
            irisApplied = false;
        } else if (!detected || data.lensId != lastLensId) {
            detected = true;
            lastLensId = data.lensId;
            irisApplied = false;
            if (adopt(lib(), data.lensId, nowIso())) save();
        }
        if (!irisApplied && data.lensId != 0) {   // the lens forgot the iris when it was unplugged or powered
            irisApplied = true;
            const double wanted = lib().working.lastIris;
            if (wanted > 0.0 && irisControlValue(wanted) > 0) {
                const std::string error = writeControl(fd, V4L2_CID_IRIS_ABSOLUTE, irisControlValue(wanted));
                if (error.empty()) iris = wanted;
                else status.error = error;
            }
        }
        if (controls) status.controls = listControls(fd);
        return finish();
    }
};

Cef168Lens::Cef168Lens() : impl_(std::make_unique<Impl>()) { impl_->outer = &mutex_; }
Cef168Lens::~Cef168Lens() = default;

Status Cef168Lens::read(bool controls)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->read(controls);
}

void Cef168Lens::poll()
{
    read(false);
}

void Cef168Lens::stop()
{
    {
        std::lock_guard<std::mutex> lock(impl_->fastMutex);
        impl_->fastStop = true;
    }
    impl_->fastWake.notify_all();
    if (impl_->fast.joinable()) impl_->fast.join();
}

void Cef168Lens::start(std::function<bool()> gate, std::function<void()> notify)
{
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->gate = std::move(gate);
    impl_->notify = std::move(notify);
    if (!impl_->fast.joinable()) impl_->fast = std::thread([impl = impl_.get()] { impl->fastLoop(); });
}

std::string Cef168Lens::summaryJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->summary;
}

std::optional<double> Cef168Lens::iris() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->iris;
}

Settings Cef168Lens::settings()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->settings();
}

std::string Cef168Lens::modify(const std::function<std::string(Library &, const std::string &now)> &change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Library &current = impl_->lib();
    Library edited = current;   // all or nothing
    if (const std::string error = change(edited, nowIso()); !error.empty()) return error;
    const bool irisChanged = edited.selected != current.selected || edited.working.lastIris != current.working.lastIris;
    current = std::move(edited);
    if (irisChanged) impl_->irisApplied = false;   // the next look sends the new working lens's iris
    const std::string saved = impl_->save();
    impl_->recompute();
    return saved;
}

std::string Cef168Lens::setFocus(int position)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(false);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    if (status.data->calibrating != 0 || impl_->calibration.state == "running") return "The lens is calibrating.";
    return writeControl(impl_->fd, V4L2_CID_FOCUS_ABSOLUTE, position);
}

std::string Cef168Lens::setIris(double fNumber)
{
    const int value = irisControlValue(fNumber);
    if (value < 0) return "That is not an f-number.";
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(false);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    if (status.data->calibrating != 0 || impl_->calibration.state == "running") return "The lens is calibrating.";
    const std::string error = writeControl(impl_->fd, V4L2_CID_IRIS_ABSOLUTE, value);
    if (error.empty()) {
        impl_->iris = fNumber;
        rememberIris(impl_->lib(), fNumber);
        impl_->save();
        impl_->recompute();
    }
    return error;
}

std::string Cef168Lens::calibrate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(false);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    if (status.data->lensId == 0) return "No lens is mounted.";
    if (impl_->calibration.state == "running" || status.data->calibrating != 0) return "The lens is already calibrating.";
    if (impl_->worker.joinable()) impl_->worker.join();   // the last run stored its result before it let go of the lock
    // The worker polls on its own fd, so the sweep never holds the main one. Press Calibrate on it first: the run starts
    // there and then, and the poll loop begins at once.
    const int workerFd = ::open(impl_->path.c_str(), O_RDWR | O_CLOEXEC);
    if (workerFd < 0) return errnoText("Cannot open " + impl_->path, errno);
    if (const std::string error = writeControl(workerFd, kCidCalibrate, 1); !error.empty()) {
        ::close(workerFd);
        return error;
    }
    Calibration run;
    run.state = "running";
    run.message = "Calibrating: the lens runs through its whole focus range.";
    run.startedAt = nowIso();
    impl_->calibration = run;
    impl_->stopWorker = false;
    Impl *impl = impl_.get();
    impl->worker = std::thread([impl, workerFd] { impl->runCalibration(workerFd); });
    return {};
}

std::string Cef168Lens::setControl(const std::string &idOrName, long long value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(true);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    const Control *match = nullptr;
    char *end = nullptr;
    const unsigned long numeric = std::strtoul(idOrName.c_str(), &end, 0);
    const bool isNumber = !idOrName.empty() && *end == '\0';
    const std::string key = nameKey(idOrName);
    for (const Control &control : status.controls)
        if ((isNumber && control.id == numeric) || (!isNumber && nameKey(control.name) == key)) match = &control;
    if (!match) return "The lens has no control '" + idOrName + "'.";
    if (match->readOnly) return match->name + " is read-only.";
    if (match->type == "string" || match->type == "integer64" || match->type[0] == 'u' || match->type == "other")
        return match->name + " is not a single number control.";
    if (match->type == "button") value = 1;
    else if (value < match->minimum || value > match->maximum)
        return match->name + " takes " + std::to_string(match->minimum) + " to " + std::to_string(match->maximum) + ".";
    if (match->id == kCidCalibrate) return "Use calibrate for the Calibrate button: it follows the run.";
    const std::string error = writeControl(impl_->fd, match->id, value);
    if (error.empty() && match->id == V4L2_CID_IRIS_ABSOLUTE) {
        impl_->iris = static_cast<double>(value) / 100.0;
        rememberIris(impl_->lib(), *impl_->iris);
        impl_->save();
        impl_->recompute();
    }
    return error;
}

// ---- [lens] in apertar-hardware.conf: the two global step sizes ----------------------------------------------------------

Settings loadStepSettings()
{
    const hwconf::Document doc = hwconf::parse(configStore().currentText());
    auto number = [&](const char *key) -> std::optional<double> {
        const auto text = hwconf::effectiveValue(doc, "lens", key);
        if (!text) return std::nullopt;
        char *end = nullptr;
        const std::string trimmed = hwconf::trim(*text);
        const double value = std::strtod(trimmed.c_str(), &end);
        if (trimmed.empty() || *end != '\0' || !std::isfinite(value)) return std::nullopt;
        return value;
    };
    Settings settings;
    if (const auto v = number("focus_step_percent"); v && *v > 0.0 && *v <= 100.0) settings.focusStepPercent = *v;
    if (const auto v = number("iris_step_thirds"); v && *v >= 1.0 && *v <= 9.0) settings.irisStepThirds = static_cast<int>(std::lround(*v));
    if (const auto text = hwconf::effectiveValue(doc, "lens", "serial_device")) settings.serialDevice = hwconf::trim(*text);
    if (const auto text = hwconf::effectiveValue(doc, "lens", "ui_focus_distance")) {
        const std::string value = hwconf::trim(*text);
        settings.uiFocusDistance = value == "true" || value == "1" || value == "on" || value == "yes";
    }
    return settings;
}

std::string saveStepSettings(const std::vector<std::pair<std::string, std::string>> &values)
{
    std::vector<hwconf::Edit> edits;
    for (const auto &[key, value] : values)
        edits.push_back({value.empty() ? hwconf::Edit::Op::Unset : hwconf::Edit::Op::Set, "lens", key, value});
    HardwareConfigStore &store = configStore();
    StoreFlags flags;
    flags.trusted = true;   // [lens] is Core's own section: set_controls does not edit it key by key
    const StoreResult result = store.edit(store.currentRevision(), edits, "lens", flags);
    return result.ok ? std::string() : "Could not update " + store.path().string() + ": " + result.error;
}

} // namespace apertar::hardware::cef168
