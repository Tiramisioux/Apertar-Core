#pragma once
// The one writer of ~/apertar-hardware.conf. Everything in Core that changes the file goes through here: the
// WebUI and core.py saves, and the IMU code. It serialises the writers, refuses a stale or half-written file,
// validates before it writes, keeps the versions it replaced, and writes atomically.
#include "hardware/HardwareConfigText.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace apertar::hardware {

struct StoreFlags {
    bool dryRun = false;        // work out everything, write nothing
    bool force = false;         // save although the new text has errors the current one does not
    bool backup = false;        // also keep <file>.bak: WebUI and core.py saves only
    bool confirmMerge = false;  // a stale text save that merged onto the current file: write it now
    bool trusted = false;       // Core's own code (the IMU): the easy-mode key checks are skipped
};

struct StoreResult {
    bool ok = false;            // saved, or (dry run, review) would save
    bool written = false;
    bool merged = false;        // a stale text save was merged key by key onto the current file
    bool needsReview = false;   // merged but not written: show text and diff, then send again with confirmMerge
    std::string code;           // refusals: locked, being_written, stale, conflict, edits, errors, script_paths, backup, write
    std::string error;          // the refusal in plain words
    std::string revision;       // of the file after the call (the current one on a refusal)
    std::string text;           // the text the file has, or would have
    std::string diff;           // current -> text
    std::vector<hwconf::Issue> issues;      // of text
    std::vector<hwconf::Issue> newErrors;   // what blocked the save, if that was it
    std::vector<hwconf::Conflict> conflicts;
};

struct StoreInfo {
    bool exists = false;
    std::string revision;
    std::string text;              // the file's text at that revision
    std::string writtenRevision;   // the last revision Core wrote, from hardware-state.conf
    std::string writtenAt;
    std::string writtenBy;
};

class HardwareConfigStore {
public:
    HardwareConfigStore(std::filesystem::path file, std::filesystem::path notesDirectory);

    static std::filesystem::path defaultPath();             // APERTAR_HARDWARE_CONFIG, else /home/pi/apertar-hardware.conf
    static std::filesystem::path defaultNotesDirectory();   // ~/.config/apertar-core

    const std::filesystem::path &path() const { return file_; }
    std::string currentText() const;                        // "" when the file does not exist
    std::string currentRevision() const;                    // hwconf::kAbsentRevision when it does not exist
    StoreInfo info() const;

    // Key edits (easy mode, the IMU code). base = the revision the caller saw; edits that touch other keys than the
    // ones changed since then still merge.
    StoreResult edit(const std::string &base, const std::vector<hwconf::Edit> &edits, const std::string &by,
                     const StoreFlags &flags = {}, const hwconf::Facts &facts = {});
    // The whole text (the Config page). A stale base whose changes are only key values merges like key edits.
    StoreResult replace(const std::string &base, const std::string &text, const std::string &by,
                        const StoreFlags &flags = {}, const hwconf::Facts &facts = {});

    // Pins the file as found, once, in the history as adopted-<revision>.conf ("Before Controls"). True when it did.
    bool adopt();

private:
    struct Stamp {
        bool exists = false;
        int64_t size = 0;
        int64_t mtimeNs = 0;
        int64_t ctimeNs = 0;
        uint64_t inode = 0;
        bool operator==(const Stamp &other) const
        {
            return exists == other.exists && size == other.size && mtimeNs == other.mtimeNs &&
                   ctimeNs == other.ctimeNs && inode == other.inode;
        }
    };

    Stamp statFile() const;
    void refreshLocked() const;
    bool settledLocked() const;
    std::optional<std::string> baseTextLocked(const std::string &revision) const;
    void rememberLocked(const std::string &text) const;
    StoreResult saveLocked(const std::string &base, const std::vector<hwconf::Edit> *edits, const std::string *text,
                           const std::string &by, const StoreFlags &flags, const hwconf::Facts &facts);
    void addHistoryLocked(const std::string &text, const std::string &revision, const std::string &by) const;
    void writeStateLocked(const std::string &by);

    std::filesystem::path file_;
    std::filesystem::path notes_;
    mutable std::mutex mutex_;
    mutable Stamp stamp_;
    mutable std::string text_;
    mutable std::string revision_ = std::string(hwconf::kAbsentRevision);
    mutable std::vector<std::pair<std::string, std::string>> seen_;   // revision, text: the last 8 Core served or wrote
    StoreInfo state_;
};

HardwareConfigStore &configStore();

} // namespace apertar::hardware
