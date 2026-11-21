#pragma once
// The texts the relay answers with when cam1 cannot be reached (dual-sensor design §4.1, §6.2). Pure: std only.
#include <optional>
#include <string>
#include <string_view>

namespace apertar::cam1state {

// A `camera=cam1` setter with no link is refused with record_start's state texts (§4.1). `word` is the state word
// (§4.5); a linked cam1 is never refused here, whatever its word.
inline std::optional<std::string> linkDownRefusal(std::string_view word, bool linked)
{
    if (linked) return std::nullopt;
    if (word == "not_found") return std::string("CAM1 is not found.");
    if (word == "lost") return std::string("CAM1 link is lost.");
    return std::string("CAM1 is offline.");
}

inline constexpr const char *kCam1DidNotAnswer = "CAM1 did not answer.";
inline constexpr const char *kCam0RecordingRefusal = "CAM0 is recording.";
inline constexpr const char *kLadderCam0Only = "The Mode Ladder runs on CAM0 only.";

} // namespace apertar::cam1state
