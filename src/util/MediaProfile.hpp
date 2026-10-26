#pragma once

#include "util/MediaProbe.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace apertar {

// The recording medium, classified two ways, as Cinemate does it: the
// *connection* (how the drive is attached) decides kernel-side tuning, the
// *filesystem* decides the writer topology. Core had neither: workers, queue
// depth and buffers depended on the recording format only, and the only thing
// Core knew about its drive was a volume label (s2 §1).
//
// Everything here is a pure function of strings except detectMediaClass(),
// which reads two sysfs files; the tables are starting points for the
// measurement in T29, not Cinemate's numbers copied.
enum class MediaClass { Unknown, PiNvme, UsbUas, UsbStorage, SdCard, Other };
enum class MediaFilesystem { None, Ext4, Exfat, Vfat, Ntfs, NtfsFuse, Other };

inline const char *mediaClassName(MediaClass c)
{
    switch (c) {
    case MediaClass::Unknown: return "unknown";
    case MediaClass::PiNvme: return "pi-nvme";
    case MediaClass::UsbUas: return "usb-uas";
    case MediaClass::UsbStorage: return "usb-storage";
    case MediaClass::SdCard: return "sd-card";
    case MediaClass::Other: return "other";
    }
    return "unknown";
}

inline const char *mediaFilesystemName(MediaFilesystem f)
{
    switch (f) {
    case MediaFilesystem::None: return "none";
    case MediaFilesystem::Ext4: return "ext4";
    case MediaFilesystem::Exfat: return "exfat";
    case MediaFilesystem::Vfat: return "vfat";
    case MediaFilesystem::Ntfs: return "ntfs";
    case MediaFilesystem::NtfsFuse: return "ntfs-fuse";
    case MediaFilesystem::Other: return "other";
    }
    return "none";
}

// rootDiskForDevice() lives in MediaProbe.hpp (the probe needs it too).

inline MediaFilesystem classifyFilesystem(std::string fstype)
{
    std::transform(fstype.begin(), fstype.end(), fstype.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (fstype.empty())
        return MediaFilesystem::None;
    if (fstype == "ext4" || fstype == "ext3" || fstype == "ext2")
        return MediaFilesystem::Ext4;
    if (fstype == "exfat")
        return MediaFilesystem::Exfat;
    if (fstype == "vfat" || fstype == "msdos" || fstype == "fat")
        return MediaFilesystem::Vfat;
    if (fstype == "ntfs" || fstype == "ntfs3")
        return MediaFilesystem::Ntfs;
    if (fstype == "fuseblk")
        return MediaFilesystem::NtfsFuse;  // ntfs-3g is the only FUSE block filesystem this camera mounts
    return MediaFilesystem::Other;
}

// Pure classification from the sysfs facts: the realpath of
// /sys/class/block/<root>/device (contains "/usb" for anything behind a USB
// bridge) and that directory's uevent text (DRIVER=uas|usb-storage|nvme).
// A fallback that cannot say no is not a fallback (Cinemate 888093c): nothing
// here returns a class on the strength of a node that exists on every Pi.
inline MediaClass classifyMediaClass(const std::string &rootDisk, const std::string &deviceRealpath,
                                     std::string uevent)
{
    if (rootDisk.rfind("mmcblk", 0) == 0)
        return MediaClass::SdCard;
    std::transform(uevent.begin(), uevent.end(), uevent.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool usb = deviceRealpath.find("/usb") != std::string::npos;
    if (usb)
        return uevent.find("driver=uas") != std::string::npos ? MediaClass::UsbUas : MediaClass::UsbStorage;
    if (rootDisk.rfind("nvme", 0) == 0 || uevent.find("driver=nvme") != std::string::npos)
        return MediaClass::PiNvme;
    if (rootDisk.empty())
        return MediaClass::Unknown;
    return MediaClass::Other;
}

inline std::string readSmallSysFile(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string text, line;
    while (std::getline(in, line)) {
        text += line;
        text += '\n';
    }
    return text;
}

inline MediaClass detectMediaClass(const std::string &devicePath)
{
    const std::string root = rootDiskForDevice(devicePath);
    if (root.empty())
        return MediaClass::Unknown;
    const std::filesystem::path device = std::filesystem::path("/sys/class/block") / root / "device";
    std::error_code ec;
    const std::filesystem::path real = std::filesystem::canonical(device, ec);
    const std::string realpath = ec ? std::string() : real.string();
    const std::string uevent = readSmallSysFile(device / "uevent");
    return classifyMediaClass(root, realpath, uevent);
}

// Writer topology per filesystem. ext4 has the flattest write latency and keeps
// Core's default 4 uncompressed workers; exFAT/FAT/NTFS allocate a directory
// entry per frame and get fewer concurrent writers, a deeper queue and two
// more camera buffers of cushion (Cinemate: 8 ext4 / 10 exFAT-NTFS). Direct I/O
// stays on where the kernel driver accepts O_DIRECT; the FUSE ntfs-3g path does
// not. Compressed formats are CPU-bound and keep their own worker counts.
struct MediaWriterSettings {
    unsigned workers = 2;
    unsigned queueFrames = 12;
    unsigned extraRawBuffers = 0;
    bool directIo = false;   // for T24's direct I/O, which this fork does not carry; unused
};

inline MediaWriterSettings writerSettingsFor(MediaFilesystem filesystem, MediaClass /*mediaClass*/)
{
    switch (filesystem) {
    case MediaFilesystem::Ext4: return {4, 12, 0, true};
    // exFAT: Core's pre-profile values. Cinemate's {2, 16, +2} dropped frames at
    // 16-bit 4K25 on the CFE NVMe (C0106, 2026-10-26); 4/12 held 10 min (C0101).
    case MediaFilesystem::Exfat: return {4, 12, 0, true};
    case MediaFilesystem::Vfat: return {2, 16, 2, true};
    case MediaFilesystem::Ntfs: return {2, 16, 2, true};
    case MediaFilesystem::NtfsFuse: return {2, 16, 2, false};
    case MediaFilesystem::None:
    case MediaFilesystem::Other: return {2, 12, 0, false};
    }
    return {2, 12, 0, false};
}

struct MediaProfile {
    MediaClass mediaClass = MediaClass::Unknown;
    MediaFilesystem filesystem = MediaFilesystem::None;
    std::string device;
    std::string rootDisk;
    std::string fstype;
    MediaWriterSettings writer{};

    bool known() const { return filesystem != MediaFilesystem::None; }

    std::string summary() const
    {
        return std::string("class=") + mediaClassName(mediaClass) + " fs=" + mediaFilesystemName(filesystem) +
               " workers=" + std::to_string(writer.workers) + " queue=" + std::to_string(writer.queueFrames) +
               " raw_buffers=+" + std::to_string(writer.extraRawBuffers);
    }
};

inline MediaProfile deriveMediaProfile(MediaClass mediaClass, const std::string &device, const std::string &fstype)
{
    MediaProfile profile;
    profile.mediaClass = mediaClass;
    profile.filesystem = classifyFilesystem(fstype);
    profile.device = device;
    profile.rootDisk = rootDiskForDevice(device);
    profile.fstype = fstype;
    profile.writer = writerSettingsFor(profile.filesystem, mediaClass);
    return profile;
}

inline MediaProfile detectMediaProfile(const MediaSnapshot &snapshot)
{
    if (!snapshot.mounted())
        return MediaProfile{};
    return deriveMediaProfile(detectMediaClass(snapshot.device), snapshot.device, snapshot.fstype);
}

// The media root the camera records to, for code that decides buffer counts at
// configure time and has no other way to the path. Set once at start-up.
inline std::string &configuredMediaRoot()
{
    static std::string root = "/media/RAW";
    return root;
}

// Uses the media watcher's cached snapshot: this runs at configure time and
// must not block on a dead device (critic Y8).
inline unsigned mediaExtraRawBuffersForConfiguredRoot()
{
    return detectMediaProfile(cachedMediaSnapshotOrProbe(configuredMediaRoot())).writer.extraRawBuffers;
}

} // namespace apertar
