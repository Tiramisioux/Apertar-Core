#include "hardware/Cef168Lens.hpp"

#include "hardware/HardwareConfigStore.hpp"
#include "hardware/HardwareConfigText.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

const char *kNoMoveMessage = "The lens did not move. Set the lens switch to AF (not MF) and try again. "
    "The adapter's switch self-test leaves the switch on the opposite side.";
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
    std::string summary = "{\"present\":false,\"state\":\"absent\",\"lens_name\":\"\",\"iris\":null,\"focus_position\":null}";

    Calibration calibration;
    Clock::time_point calibrationStart;
    bool calibrationSeen = false;   // the board raised `calibrating` during this run

    void close()
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    ~Impl() { close(); }

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
        return settings;
    }

    void finishCalibration(const Data &data, bool timedOut)
    {
        Calibration &cal = calibration;
        cal.finishedAt = nowIso();
        cal.focusMin = data.focusMin;
        cal.focusMax = data.focusMax;
        cal.distanceMinCm = data.distanceMinCm;
        cal.distanceMaxCm = data.distanceMaxCm;
        Entry &working = lib().working;
        const bool evidence = calibrationSeen || cal.movingTimeMs > 0;   // it moved
        const bool valid = data.rangeValid();
        if (timedOut) {
            cal.state = "failed";
            cal.message = "Calibration did not finish in 60 s. Check the lens switch is on AF and try again.";
        } else if (!evidence) {
            cal.state = "failed";
            cal.message = kNoMoveMessage;
            if (!valid) working.capFocus = false, working.capAutofocus = false;
        } else if (!valid) {
            cal.state = "failed";
            cal.message = "The lens moved, but the board holds no focus range for it (0, 32767 or 65535). Try again; if it stays, this lens does not work with focus control.";
            working.capFocus = false;
            working.capAutofocus = false;
        } else {
            cal.state = "done";
            cal.message = "Calibrated: focus " + std::to_string(data.focusMin) + " to " + std::to_string(data.focusMax) +
                (data.distanceMinCm == data.distanceMaxCm ? ". The lens reports no focus distance, so autofocus tuning is not possible." : ".");
            working.focusMin = data.focusMin;
            working.focusMax = data.focusMax;
            working.distanceMinCm = data.distanceMinCm;
            working.distanceMaxCm = data.distanceMaxCm;
            working.capFocus = true;
        }
        save();
    }

    // The sequence of Pinefeat's calibrate.cpp: press Calibrate, poll Data until `calibrating` clears (never before it was
    // seen, unless it never comes), then read the range the board now holds.
    void followCalibration(const Data &data)
    {
        if (calibration.state != "running") return;
        const auto elapsed = Clock::now() - calibrationStart;
        if (data.calibrating != 0) calibrationSeen = true;
        calibration.movingTimeMs = std::max(calibration.movingTimeMs, data.movingTime);
        const bool cleared = data.calibrating == 0 && (calibrationSeen || elapsed >= kCalibrationNoFlag);
        if (cleared || elapsed >= kCalibrationTimeout) finishCalibration(data, !cleared);
    }

    Status read(bool controls)
    {
        Status status;
        auto finish = [&]() {
            status.library = lib();
            status.calibration = calibration;
            status.settings = settings();
            status.iris = iris;
            summary = statusSummaryJson(status);
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
        followCalibration(data);
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

Cef168Lens::Cef168Lens() : impl_(std::make_unique<Impl>()) {}
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
    return impl_->save();
}

std::string Cef168Lens::setFocus(int position)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(false);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    if (status.data->calibrating != 0) return "The lens is calibrating.";
    return writeControl(impl_->fd, V4L2_CID_FOCUS_ABSOLUTE, position);
}

std::string Cef168Lens::setIris(double fNumber)
{
    const int value = irisControlValue(fNumber);
    if (value < 0) return "That is not an f-number.";
    std::lock_guard<std::mutex> lock(mutex_);
    const Status status = impl_->read(false);
    if (!status.data) return status.present ? status.error : "No Pinefeat CEF168 lens controller found.";
    if (status.data->calibrating != 0) return "The lens is calibrating.";
    const std::string error = writeControl(impl_->fd, V4L2_CID_IRIS_ABSOLUTE, value);
    if (error.empty()) {
        impl_->iris = fNumber;
        rememberIris(impl_->lib(), fNumber);
        impl_->save();
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
    if (const std::string error = writeControl(impl_->fd, kCidCalibrate, 1); !error.empty()) return error;
    Calibration run;
    run.state = "running";
    run.message = "Calibrating: the lens runs through its whole focus range.";
    run.startedAt = nowIso();
    impl_->calibration = run;
    impl_->calibrationStart = Clock::now();
    impl_->calibrationSeen = false;
    // Give the board a moment to raise `calibrating`, so a short run is not missed between two ticks.
    for (int i = 0; i < 10 && !impl_->calibrationSeen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (const auto data = readFrame(impl_->fd, nullptr); data && data->calibrating != 0) impl_->calibrationSeen = true;
    }
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
