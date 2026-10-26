#pragma once

#include <cerrno>
#include <exception>
#include <string>
#include <system_error>

namespace apertar {

// What a failed write to the recording medium means. The errno decides; the
// classes are the ones a recorder has to treat differently: a medium that is
// gone must not be touched again, a full medium is an orderly stop, a permission
// problem is a configuration error. Logic follows Cinemate's yank errno set
// {EIO, ENOENT, ENODEV, ENOTCONN, ESTALE} plus ENXIO and ENOMEDIUM.
enum class MediaFault {
    None,
    Removed,     // device or mount gone: do not reopen, rename or fsync anything on it
    Full,        // ENOSPC/EDQUOT: keep what was written, stop cleanly
    ReadOnly,    // EROFS: the kernel remounted read-only after an error, or a ro mount
    Permission,  // EACCES/EPERM
    Other
};

inline MediaFault classifyMediaErrno(int err)
{
    switch (err) {
    case 0:
        return MediaFault::None;
    case EIO:
    case ENODEV:
    case ENXIO:
    case ENOTCONN:
    case ESTALE:
    case ENOENT:
#ifdef ENOMEDIUM
    case ENOMEDIUM:
#endif
        return MediaFault::Removed;
    case ENOSPC:
    case EDQUOT:
        return MediaFault::Full;
    case EROFS:
        return MediaFault::ReadOnly;
    case EACCES:
    case EPERM:
        return MediaFault::Permission;
    default:
        return MediaFault::Other;
    }
}

// The errno behind an exception, when there is one. std::system_error from the
// generic or system category carries it; anything else is unknown (0).
inline int errnoFromException(const std::exception &e)
{
    if (const auto *se = dynamic_cast<const std::system_error *>(&e)) {
        const auto &code = se->code();
        if (code.category() == std::generic_category() || code.category() == std::system_category())
            return code.value();
    }
    return 0;
}

inline MediaFault classifyMediaException(const std::exception &e)
{
    const int err = errnoFromException(e);
    return err == 0 ? MediaFault::Other : classifyMediaErrno(err);
}

// Short machine-readable name for state fields and logs.
inline const char *mediaFaultName(MediaFault fault)
{
    switch (fault) {
    case MediaFault::None: return "";
    case MediaFault::Removed: return "media_removed";
    case MediaFault::Full: return "media_full";
    case MediaFault::ReadOnly: return "media_read_only";
    case MediaFault::Permission: return "media_permission";
    case MediaFault::Other: return "write_error";
    }
    return "write_error";
}

// Operator-facing text. `detail` is the low-level message (path + strerror)
// and is appended so the journal keeps the evidence.
inline std::string mediaFaultMessage(MediaFault fault, const std::string &detail)
{
    std::string text;
    switch (fault) {
    case MediaFault::None: return detail;
    case MediaFault::Removed: text = "Media removed during recording."; break;
    case MediaFault::Full: text = "Media is full."; break;
    case MediaFault::ReadOnly: text = "Media became read-only."; break;
    case MediaFault::Permission: text = "No permission to write to the media."; break;
    case MediaFault::Other: return detail.empty() ? std::string("Recording write failed.") : detail;
    }
    if (!detail.empty())
        text += " (" + detail + ")";
    return text;
}

// A medium in this state must not be reopened, renamed, fsynced or unlinked on.
inline bool mediaFaultMeansGone(MediaFault fault)
{
    return fault == MediaFault::Removed;
}

} // namespace apertar
