#include "hardware/HardwareConfigStore.hpp"

#include "util/SettingsFile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

#include <sys/stat.h>

namespace apertar::hardware {

namespace {

constexpr size_t kHistoryKept = 20;
constexpr size_t kSeenKept = 8;
constexpr int kImuCoalesceSeconds = 600;

std::string readTextFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// The script paths a press would run: the non-empty path keys of [run_script]. An empty slot runs nothing.
std::map<std::string, std::string> scriptPaths(const hwconf::Document &doc)
{
    std::map<std::string, std::string> out;
    for (const auto &line : doc.lines)
        if (line.kind == hwconf::Kind::Key && line.section == "run_script" && line.key.rfind("path", 0) == 0 && !line.value.empty())
            out[line.key] = line.value;
    return out;
}

std::string timestamp(const char *format, std::time_t when)
{
    std::tm local {};
    localtime_r(&when, &local);
    char out[32];
    std::strftime(out, sizeof(out), format, &local);
    return out;
}

// The name part of a history entry: [a-z0-9_] only, so a caller cannot put a path in it.
std::string safeName(const std::string &by)
{
    std::string out;
    for (const char ch : by)
        out += (std::isalnum(static_cast<unsigned char>(ch)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) : '_');
    return out.empty() ? "unknown" : out;
}

bool isHistoryEntry(const std::string &name)
{
    // <YYYYmmdd-HHMMSS>-h<16 hex>-<by>.conf
    return name.size() > 35 && name[8] == '-' && name[15] == '-' && name[16] == 'h' && name[33] == '-' &&
           name.compare(name.size() - 5, 5, ".conf") == 0;
}

std::vector<std::string> historyEntries(const std::filesystem::path &directory)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file(error) && isHistoryEntry(entry.path().filename().string()))
            names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::time_t entryTime(const std::string &name)
{
    std::tm local {};
    local.tm_isdst = -1;
    if (strptime(name.substr(0, 15).c_str(), "%Y%m%d-%H%M%S", &local) == nullptr)
        return 0;
    return std::mktime(&local);
}

} // namespace

HardwareConfigStore::HardwareConfigStore(std::filesystem::path file, std::filesystem::path notesDirectory)
    : file_(std::move(file)), notes_(std::move(notesDirectory))
{
    std::ifstream input(notes_ / "hardware-state.conf");
    for (std::string line; std::getline(input, line);) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "written_revision")
            state_.writtenRevision = value;
        else if (key == "written_at")
            state_.writtenAt = value;
        else if (key == "written_by")
            state_.writtenBy = value;
    }
}

std::filesystem::path HardwareConfigStore::defaultPath()
{
    if (const char *envPath = std::getenv("APERTAR_HARDWARE_CONFIG"); envPath && *envPath)
        return std::filesystem::path(envPath);
    return "/home/pi/apertar-hardware.conf";
}

std::filesystem::path HardwareConfigStore::defaultNotesDirectory()
{
    if (const char *xdgConfig = std::getenv("XDG_CONFIG_HOME"); xdgConfig && *xdgConfig)
        return std::filesystem::path(xdgConfig) / "apertar-core";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "apertar-core";
    return std::filesystem::temp_directory_path() / "apertar-core";
}

HardwareConfigStore::Stamp HardwareConfigStore::statFile() const
{
    Stamp stamp;
    struct stat info {};
    if (::stat(file_.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return stamp;
    stamp.exists = true;
    stamp.size = info.st_size;
#ifdef __APPLE__
    stamp.mtimeNs = int64_t(info.st_mtimespec.tv_sec) * 1000000000 + info.st_mtimespec.tv_nsec;
    stamp.ctimeNs = int64_t(info.st_ctimespec.tv_sec) * 1000000000 + info.st_ctimespec.tv_nsec;
#else
    stamp.mtimeNs = int64_t(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
    stamp.ctimeNs = int64_t(info.st_ctim.tv_sec) * 1000000000 + info.st_ctim.tv_nsec;
#endif
    stamp.inode = info.st_ino;
    return stamp;
}

// Reads the file again only when its stat key (size, mtime, ctime, inode) moved. A rename changes the inode.
void HardwareConfigStore::refreshLocked() const
{
    const Stamp now = statFile();
    if (now == stamp_)
        return;
    stamp_ = now;
    if (!now.exists) {
        text_.clear();
        revision_ = std::string(hwconf::kAbsentRevision);
        return;
    }
    text_ = readTextFile(file_);
    revision_ = hwconf::revision(text_);
    rememberLocked(text_);
}

void HardwareConfigStore::rememberLocked(const std::string &text) const
{
    const std::string revision = hwconf::revision(text);
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const auto &item) { return item.first == revision; }), seen_.end());
    seen_.insert(seen_.begin(), {revision, text});
    if (seen_.size() > kSeenKept)
        seen_.resize(kSeenKept);
}

// The text of a revision a caller says it started from: what Core served or wrote, then the history.
std::optional<std::string> HardwareConfigStore::baseTextLocked(const std::string &revision) const
{
    if (revision == hwconf::kAbsentRevision)
        return std::string();
    for (const auto &item : seen_)
        if (item.first == revision)
            return item.second;
    const std::filesystem::path directory = notes_ / "hardware-history";
    const std::filesystem::path adopted = directory / ("adopted-" + revision + ".conf");
    if (std::filesystem::exists(adopted))
        return readTextFile(adopted);
    for (const auto &name : historyEntries(directory))
        if (name.compare(16, 17, revision) == 0)
            return readTextFile(directory / name);
    return std::nullopt;
}

// A file Core did not write must not be edited while an editor may still be saving it (nano rewrites in place).
bool HardwareConfigStore::settledLocked() const
{
    if (!stamp_.exists || revision_ == state_.writtenRevision)
        return true;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t age = now - stamp_.mtimeNs;
    if (age >= 0 && age < 1000000000)
        return false;
    const Stamp first = statFile();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return first == statFile();
}

std::string HardwareConfigStore::currentText() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    refreshLocked();
    return text_;
}

std::string HardwareConfigStore::currentRevision() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    refreshLocked();
    return revision_;
}

StoreInfo HardwareConfigStore::info() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    refreshLocked();
    StoreInfo out = state_;
    out.exists = stamp_.exists;
    out.revision = revision_;
    out.text = text_;
    return out;
}

StoreResult HardwareConfigStore::edit(const std::string &base, const std::vector<hwconf::Edit> &edits, const std::string &by,
                                      const StoreFlags &flags, const hwconf::Facts &facts)
{
    return saveLocked(base, &edits, nullptr, by, flags, facts);
}

StoreResult HardwareConfigStore::replace(const std::string &base, const std::string &text, const std::string &by,
                                         const StoreFlags &flags, const hwconf::Facts &facts)
{
    return saveLocked(base, nullptr, &text, by, flags, facts);
}

StoreResult HardwareConfigStore::saveLocked(const std::string &base, const std::vector<hwconf::Edit> *edits,
                                            const std::string *text, const std::string &by, const StoreFlags &flags,
                                            const hwconf::Facts &facts)
{
    // The mutex first, then the file lock: the writers in this process queue here, other programs on the lock.
    std::lock_guard<std::mutex> guard(mutex_);
    StoreResult result;
    std::error_code error;
    std::filesystem::create_directories(file_.parent_path(), error);
    std::unique_ptr<settingsFile::QtIniLock> lock;
    if (!flags.dryRun) {
        lock = std::make_unique<settingsFile::QtIniLock>(file_);
        if (!*lock) {
            result.code = "locked";
            result.error = "Another program is writing the file; try again.";
            return result;
        }
    }

    refreshLocked();
    result.revision = revision_;
    result.text = text_;
    if (!settledLocked()) {
        result.code = "being_written";
        result.error = "The file is being written (an editor is saving it); try again in a second.";
        return result;
    }

    const std::string current = text_;
    const std::string currentRevision = revision_;
    const hwconf::Document currentDoc = hwconf::parse(current);
    std::vector<hwconf::Edit> merged;
    std::string next;

    if (base != currentRevision) {
        const auto baseText = baseTextLocked(base);
        if (!baseText) {
            result.code = "stale";
            result.error = "The file changed since you loaded it. Reload, then change it again.";
            return result;
        }
        if (edits)
            merged = *edits;
        else if (const auto fromText = hwconf::textToEdits(*baseText, *text))
            merged = *fromText;
        else {
            result.code = "stale";
            result.error = "The file changed since you loaded it, in more than values. Reload, then change it again.";
            return result;
        }
        if (const std::string problem = flags.trusted ? std::string() : hwconf::checkEdits(currentDoc, merged); !problem.empty()) {
            result.code = edits ? "edits" : "stale";
            result.error = problem;
            return result;
        }
        result.conflicts = hwconf::conflicts(*baseText, current, merged);
        if (!result.conflicts.empty()) {
            result.code = "conflict";
            result.error = "The file changed in the same place since you loaded it.";
            return result;
        }
        next = hwconf::applyEdits(current, merged);
        result.merged = text != nullptr;
    } else if (edits) {
        if (const std::string problem = flags.trusted ? std::string() : hwconf::checkEdits(currentDoc, *edits); !problem.empty()) {
            result.code = "edits";
            result.error = problem;
            return result;
        }
        next = hwconf::applyEdits(current, *edits);
    } else {
        next = *text;
    }

    result.text = next;
    result.diff = hwconf::diff(current, next);
    result.issues = hwconf::validate(next, facts);
    if (next == current) {
        result.ok = true;
        return result;
    }

    const hwconf::Document nextDoc = hwconf::parse(next);
    if (scriptPaths(nextDoc) != scriptPaths(currentDoc)) {
        result.code = "script_paths";
        result.error = "Script paths are changed over ssh, not here.";
        return result;
    }
    result.newErrors = hwconf::newErrors(hwconf::validate(current, facts), result.issues);
    if (!result.newErrors.empty() && !flags.force) {
        result.code = "errors";
        result.error = "Not saved: " + std::to_string(result.newErrors.size()) + " new error" +
                       (result.newErrors.size() == 1 ? "" : "s") + ". " + result.newErrors.front().message;
        return result;
    }

    if (result.merged && !flags.confirmMerge) {
        result.ok = true;
        result.needsReview = true;
        return result;
    }
    if (flags.dryRun) {
        result.ok = true;
        return result;
    }

    const bool existed = stamp_.exists;
    if (existed) {
        addHistoryLocked(current, currentRevision, by);
        if (flags.backup && !settingsFile::atomicWrite(file_.string() + ".bak", current)) {
            result.code = "backup";
            result.error = "Could not keep the backup of the file; nothing was saved.";
            return result;
        }
    }
    const std::string nextRevision = hwconf::revision(next);
    const std::string previousWritten = state_.writtenRevision;
    state_.writtenRevision = nextRevision;   // before the rename, so nobody calls Core's own write "outside"
    if (!settingsFile::atomicWrite(file_, next)) {
        state_.writtenRevision = previousWritten;
        result.code = "write";
        result.error = "Could not write the file; nothing was saved.";
        return result;
    }
    rememberLocked(current);
    stamp_ = Stamp {};
    refreshLocked();
    writeStateLocked(by);
    result.ok = true;
    result.written = true;
    result.revision = revision_;
    return result;
}

// The version being replaced goes to the history. IMU writes within 10 minutes of an IMU entry share it.
void HardwareConfigStore::addHistoryLocked(const std::string &text, const std::string &revision, const std::string &by) const
{
    const std::filesystem::path directory = notes_ / "hardware-history";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::string who = safeName(by);
    auto entries = historyEntries(directory);
    const std::time_t now = std::time(nullptr);
    if (who == "imu" && !entries.empty()) {
        const std::string &newest = entries.back();
        if (newest.compare(newest.size() - 9, 9, "-imu.conf") == 0 && std::difftime(now, entryTime(newest)) < kImuCoalesceSeconds)
            return;
    }
    const std::string name = timestamp("%Y%m%d-%H%M%S", now) + "-" + revision + "-" + who + ".conf";
    settingsFile::atomicWrite(directory / name, text);
    entries = historyEntries(directory);
    for (size_t i = 0; entries.size() > kHistoryKept && i < entries.size() - kHistoryKept; ++i)
        std::filesystem::remove(directory / entries[i], error);
}

void HardwareConfigStore::writeStateLocked(const std::string &by)
{
    state_.writtenAt = timestamp("%Y-%m-%d %H:%M:%S", std::time(nullptr));
    state_.writtenBy = by;
    settingsFile::atomicWrite(notes_ / "hardware-state.conf",
                              "written_revision=" + state_.writtenRevision + "\n" +
                              "written_at=" + state_.writtenAt + "\n" +
                              "written_by=" + state_.writtenBy + "\n");
}

bool HardwareConfigStore::adopt()
{
    std::lock_guard<std::mutex> guard(mutex_);
    refreshLocked();
    if (!stamp_.exists)
        return false;
    const std::filesystem::path directory = notes_ / "hardware-history";
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
        if (entry.path().filename().string().rfind("adopted-", 0) == 0)
            return false;
    return settingsFile::atomicWrite(directory / ("adopted-" + revision_ + ".conf"), text_);
}

HardwareConfigStore &configStore()
{
    static HardwareConfigStore store(HardwareConfigStore::defaultPath(), HardwareConfigStore::defaultNotesDirectory());
    return store;
}

} // namespace apertar::hardware
