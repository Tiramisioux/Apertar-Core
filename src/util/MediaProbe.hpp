#pragma once

#include "util/MediaFault.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/statvfs.h>

namespace apertar {

// Is the recording medium alive? Cinemate's two independent answers are "is the
// path still a mount point" (/proc/mounts) and "does statvfs on the mount root
// answer" (a yanked device stays in /proc/mounts until something unmounts it,
// and statvfs then fails with EIO/ENODEV/...). Errors on paths *inside* the
// volume are deliberately not consulted here: a corrupt clip directory is not a
// yank (Cinemate 58fc7c2).
//
// The decision is a pure function (evaluateMediaProbe) so it can be tested on
// the host; probeMediaRoot() is the thin syscall wrapper around it.
// What the CFexpress HAT's status byte says about the card slot (CfeHat.hpp).
// Unknown when there is no HAT, it is disabled, or its reads disagree.
enum class CardPresence { Unknown, Present, Absent };

inline const char *cardPresenceName(CardPresence card)
{
    switch (card) {
    case CardPresence::Present: return "in";
    case CardPresence::Absent: return "out";
    case CardPresence::Unknown: break;
    }
    return "-";
}

struct MediaSnapshot {
    enum class State { None, Mounted, Removed, Error };
    State state = State::None;
    std::string device;      // /dev/... column of /proc/mounts, empty when not mounted
    std::string fstype;      // ext4, exfat, ntfs3, fuseblk, vfat, ...
    std::string options;     // mount options as /proc/mounts shows them
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    int statvfsErrno = 0;    // the errno statvfs returned, 0 when it succeeded
    bool deviceNodePresent = true;   // /sys/class/block/<root> still exists (USB: gone at once on a pull)
    std::string nvmeState;           // /sys/block/<root>/device/state for NVMe ("live", "dead", ...), else empty
    CardPresence card = CardPresence::Unknown;   // CFexpress HAT slot, for an NVMe medium

    bool mounted() const { return state == State::Mounted; }
    const char *stateName() const
    {
        switch (state) {
        case State::None: return "none";
        case State::Mounted: return "mounted";
        case State::Removed: return "removed";
        case State::Error: return "error";
        }
        return "none";
    }
    bool sameMedium(const MediaSnapshot &other) const
    {
        return state == other.state && device == other.device && fstype == other.fstype;
    }
};

struct MountTableEntry {
    bool found = false;
    std::string device;
    std::string fstype;
    std::string options;
};

// /proc/mounts escapes space, tab, newline and backslash as \040 \011 \012 \134.
inline std::string decodeMountField(const std::string &field)
{
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const char a = field[i + 1], b = field[i + 2], c = field[i + 3];
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                out.push_back(static_cast<char>((a - '0') * 64 + (b - '0') * 8 + (c - '0')));
                i += 3;
                continue;
            }
        }
        out.push_back(field[i]);
    }
    return out;
}

inline std::string normalizeMountPathForProbe(std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

// Exact match on the mount-point column; "/media/RAW1" never matches "/media/RAW"
// (Cinemate's cinepi-raw used a substring test and a standby drive kept it true).
inline MountTableEntry findMountEntry(std::istream &mounts, const std::string &mountPath)
{
    const std::string wanted = normalizeMountPathForProbe(mountPath);
    MountTableEntry entry;
    std::string device, mountedAt, type, options, rest;
    while (mounts >> device >> mountedAt >> type >> options) {
        std::getline(mounts, rest);
        if (normalizeMountPathForProbe(decodeMountField(mountedAt)) != wanted)
            continue;
        // The last entry for a path is the one on top.
        entry.found = true;
        entry.device = decodeMountField(device);
        entry.fstype = type;
        entry.options = options;
    }
    return entry;
}

inline MountTableEntry findMountEntry(const std::string &mountPath)
{
    std::ifstream mounts("/proc/mounts");
    return findMountEntry(mounts, mountPath);
}

// /dev/nvme0n1p1 -> nvme0n1, /dev/mmcblk0p2 -> mmcblk0, /dev/sda1 -> sda, /dev/sda -> sda.
inline std::string rootDiskForDevice(const std::string &devicePath)
{
    std::string name = std::filesystem::path(devicePath).filename().string();
    if (name.empty())
        return name;
    const bool partitionedWithP = name.rfind("nvme", 0) == 0 || name.rfind("mmcblk", 0) == 0;
    if (partitionedWithP) {
        const size_t p = name.rfind('p');
        if (p != std::string::npos && p > 0 && p + 1 < name.size() &&
            std::isdigit(static_cast<unsigned char>(name[p - 1]))) {
            bool digits = true;
            for (size_t i = p + 1; i < name.size(); ++i)
                digits = digits && std::isdigit(static_cast<unsigned char>(name[i]));
            if (digits)
                name.erase(p);
        }
        return name;
    }
    while (name.size() > 1 && std::isdigit(static_cast<unsigned char>(name.back())))
        name.pop_back();
    return name;
}

// The decision. statvfs alone is not enough: ext4_statfs answers from in-memory
// counters and exFAT/FAT cache theirs after the first call, so a pulled drive can
// keep "answering" (critic Y1). Two device-level facts settle it the way
// Cinemate's udev-remove and NVMe-state watchdogs do: the block device node is
// gone (USB: immediately), or the NVMe controller reports "dead" (after its
// I/O timeout). A CFexpress card pulled from the HAT keeps both for that whole
// timeout (the controller stays "live" until an I/O times out), so for an NVMe
// medium the HAT's slot bit is asked first. Order: not mounted -> None; NVMe and
// HAT slot empty, node gone or NVMe dead -> Removed; statvfs yank errno ->
// Removed; other errno -> Error; else Mounted.
inline MediaSnapshot evaluateMediaProbe(const MountTableEntry &entry, int statvfsErrno,
                                        uint64_t freeBytes, uint64_t totalBytes,
                                        bool deviceNodePresent = true, const std::string &nvmeState = {},
                                        CardPresence card = CardPresence::Unknown)
{
    MediaSnapshot snapshot;
    if (!entry.found) {
        snapshot.state = MediaSnapshot::State::None;
        return snapshot;
    }
    snapshot.device = entry.device;
    snapshot.fstype = entry.fstype;
    snapshot.options = entry.options;
    snapshot.statvfsErrno = statvfsErrno;
    snapshot.deviceNodePresent = deviceNodePresent;
    snapshot.nvmeState = nvmeState;
    const bool nvme = rootDiskForDevice(entry.device).rfind("nvme", 0) == 0;
    snapshot.card = nvme ? card : CardPresence::Unknown;
    if ((nvme && card == CardPresence::Absent) || !deviceNodePresent || nvmeState == "dead") {
        snapshot.state = MediaSnapshot::State::Removed;
        return snapshot;
    }
    if (statvfsErrno == 0) {
        snapshot.state = MediaSnapshot::State::Mounted;
        snapshot.freeBytes = freeBytes;
        snapshot.totalBytes = totalBytes;
        return snapshot;
    }
    snapshot.state = classifyMediaErrno(statvfsErrno) == MediaFault::Removed
        ? MediaSnapshot::State::Removed : MediaSnapshot::State::Error;
    return snapshot;
}

// statvfs on the mount root. It can block for a long time on a hung device, so
// call this from a thread that holds no lock anyone else waits for.
inline std::string readSysfsWord(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string word;
    in >> word;
    return word;
}

inline MediaSnapshot probeMediaRoot(const std::string &mountPath, CardPresence card = CardPresence::Unknown)
{
    const MountTableEntry entry = findMountEntry(mountPath);
    if (!entry.found)
        return evaluateMediaProbe(entry, 0, 0, 0);
    // An empty CFexpress slot settles it before statvfs, which can block on the
    // vanished device.
    if (card == CardPresence::Absent && rootDiskForDevice(entry.device).rfind("nvme", 0) == 0)
        return evaluateMediaProbe(entry, 0, 0, 0, true, {}, card);
    bool nodePresent = true;
    std::string nvmeState;
    if (entry.device.rfind("/dev/", 0) == 0) {
        std::error_code ec;
        const std::string root = rootDiskForDevice(entry.device);
        const std::filesystem::path partition = std::filesystem::path("/sys/class/block") / std::filesystem::path(entry.device).filename();
        nodePresent = std::filesystem::exists(partition, ec);
        if (root.rfind("nvme", 0) == 0)
            nvmeState = readSysfsWord(std::filesystem::path("/sys/block") / root / "device" / "state");
    }
    struct statvfs vfs {};
    if (::statvfs(mountPath.c_str(), &vfs) != 0)
        return evaluateMediaProbe(entry, errno ? errno : EIO, 0, 0, nodePresent, nvmeState, card);
    const uint64_t frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    return evaluateMediaProbe(entry, 0, uint64_t(vfs.f_bavail) * frsize, uint64_t(vfs.f_blocks) * frsize,
                              nodePresent, nvmeState, card);
}

// The watcher's latest result, for code that must not block on a dead device
// (configure-time buffer counts, record_start under the controller lock). Empty
// until the watcher has run once.
inline std::mutex &cachedMediaSnapshotMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::optional<MediaSnapshot> &cachedMediaSnapshotStorage()
{
    static std::optional<MediaSnapshot> snapshot;
    return snapshot;
}

inline void updateCachedMediaSnapshot(const MediaSnapshot &snapshot)
{
    std::lock_guard<std::mutex> lock(cachedMediaSnapshotMutex());
    cachedMediaSnapshotStorage() = snapshot;
}

inline std::optional<MediaSnapshot> cachedMediaSnapshot()
{
    std::lock_guard<std::mutex> lock(cachedMediaSnapshotMutex());
    return cachedMediaSnapshotStorage();
}

// Cached when the watcher has one, a direct probe only before its first cycle.
inline MediaSnapshot cachedMediaSnapshotOrProbe(const std::string &mountPath)
{
    if (const auto cached = cachedMediaSnapshot())
        return *cached;
    return probeMediaRoot(mountPath);
}

} // namespace apertar
