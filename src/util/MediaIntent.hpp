#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace apertar {

// Eject intent: a flag file an automounter honours so a deliberate unmount is
// not undone. `umount` makes udev emit a "change" event for the device, and an
// automounter that treats "change" like "add" remounts the drive within a
// second (Cinemate f01b6cd). Core touches
//   $APERTAR_MEDIA_RUN_DIR/eject/<device basename>   (default /run/apertar-media)
// before unmounting and removes it when it mounts the device again or when the
// device node is gone. /run is a tmpfs, so no flag survives a reboot. The
// directory is created by the automounter or by a tmpfiles.d rule, owned by the
// user Core runs as; writing is best effort and failure only costs the
// protection, never the eject.
inline std::string mediaRunDirectory()
{
    if (const char *env = std::getenv("APERTAR_MEDIA_RUN_DIR"); env && *env)
        return env;
    return "/run/apertar-media";
}

inline std::filesystem::path ejectIntentPath(const std::string &devicePath)
{
    return std::filesystem::path(mediaRunDirectory()) / "eject" / std::filesystem::path(devicePath).filename();
}

inline bool writeEjectIntent(const std::string &devicePath, std::string *why)
{
    const std::filesystem::path flag = ejectIntentPath(devicePath);
    if (flag.filename().empty()) {
        if (why) *why = "no device name";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(flag.parent_path(), ec)) {
        if (why) *why = flag.parent_path().string() + " does not exist";
        return false;
    }
    std::ofstream out(flag, std::ios::trunc);
    if (!out) {
        if (why) *why = "cannot write " + flag.string();
        return false;
    }
    out << "ejected by apertar-core\n";
    return static_cast<bool>(out);
}

inline void clearEjectIntent(const std::string &devicePath)
{
    std::error_code ec;
    std::filesystem::remove(ejectIntentPath(devicePath), ec);
}

inline bool blockDeviceNodeExists(const std::string &devicePath)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("/sys/class/block") /
                                   std::filesystem::path(devicePath).filename(), ec);
}

} // namespace apertar
