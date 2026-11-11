#pragma once
// The secondary's start wait (dual-sensor design §3.4, P10). POSIX and std only, no libcamera; host-tested
// (primary_wait_test.cpp).
//
// Before its first camera start the secondary waits up to 30 s for the primary's control socket to answer, then until
// the primary is idle (§4.3), then starts. A blocking request over the socket: connect, send one get_state line,
// read lines until the reply (every client also receives the primary's events, which are skipped).
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace apertar::primarywait {

inline constexpr const char *kPrimarySocket = "/tmp/apertar-core.sock";
inline constexpr int kSocketTimeoutMs = 30000;
inline constexpr int kPollMs = 500;

namespace detail {
// The value text after `"key":` in a get_state reply: "true", "false" or the string between quotes. Empty when absent.
// The quote in front of the key keeps "recording" apart from "sound_recording".
inline std::string valueOf(std::string_view reply, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\":";
    size_t at = reply.find(needle);
    if (at == std::string_view::npos)
        return {};
    at += needle.size();
    while (at < reply.size() && reply[at] == ' ')
        ++at;
    if (at < reply.size() && reply[at] == '"') {
        const size_t end = reply.find('"', at + 1);
        return end == std::string_view::npos ? std::string() : std::string(reply.substr(at + 1, end - at - 1));
    }
    size_t end = at;
    while (end < reply.size() && reply[end] != ',' && reply[end] != '}')
        ++end;
    return std::string(reply.substr(at, end - at));
}
} // namespace detail

// §4.3: idle means Ready and none of recording, storage_preroll_active, timelapse_active, still_exposure_active,
// media_writes_pending, photo mode. A reply that is not an ok reply, or has no state, is not idle.
inline bool stateIsIdle(std::string_view reply)
{
    if (reply.rfind("{\"ok\":true", 0) != 0)
        return false;
    if (detail::valueOf(reply, "state") != "ready")
        return false;
    for (const char *busy : {"recording", "storage_preroll_active", "timelapse_active", "still_exposure_active",
                             "media_writes_pending", "photo_mode_enabled"})
        if (detail::valueOf(reply, busy) == "true")
            return false;
    return true;
}

// One request: connect, send `line`, read until a line that starts with {"ok" (the reply). False on any failure or when
// timeoutMs passes.
inline bool request(const std::string &socketPath, const std::string &line, int timeoutMs, std::string *reply)
{
    sockaddr_un address{};
    if (socketPath.size() >= sizeof(address.sun_path))
        return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
        const std::string payload = line + "\n";
        size_t sent = 0;
        while (sent < payload.size()) {
            const ssize_t written = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
            if (written <= 0)
                break;
            sent += static_cast<size_t>(written);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::string buffer;
        while (sent == payload.size() && !ok && buffer.size() < (1u << 22)) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                break;
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0)
                break;
            char chunk[4096];
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0)
                break;
            buffer.append(chunk, static_cast<size_t>(got));
            size_t start = 0;
            for (size_t nl; (nl = buffer.find('\n', start)) != std::string::npos; start = nl + 1) {
                if (buffer.compare(start, 5, "{\"ok\"") == 0) {
                    if (reply)
                        *reply = buffer.substr(start, nl - start);
                    ok = true;
                    break;
                }
            }
            buffer.erase(0, start);
        }
    }
    ::close(fd);
    return ok;
}

enum class Outcome { Idle, TimedOut, Cancelled };

// Asks the primary for get_state every pollMs until it is idle. If it has not answered for socketTimeoutMs (never
// answered, or stopped answering), TimedOut: the caller starts without the idle wait. `sleep(ms)` waits and returns
// true when the caller was told to stop (a signal); `elapsedMs`, when set, gets the time spent.
inline Outcome waitForPrimaryIdle(const std::string &socketPath, int socketTimeoutMs, int pollMs,
                                  const std::function<bool(int)> &sleep, long *elapsedMs = nullptr)
{
    const auto begin = std::chrono::steady_clock::now();
    auto lastAnswer = begin;
    const auto since = [](auto from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
    };
    for (;;) {
        std::string reply;
        if (request(socketPath, "{\"cmd\":\"get_state\"}", 2000, &reply)) {
            lastAnswer = std::chrono::steady_clock::now();
            if (stateIsIdle(reply)) {
                if (elapsedMs) *elapsedMs = static_cast<long>(since(begin));
                return Outcome::Idle;
            }
        } else if (since(lastAnswer) >= socketTimeoutMs) {
            if (elapsedMs) *elapsedMs = static_cast<long>(since(begin));
            return Outcome::TimedOut;
        }
        if (sleep(pollMs)) {
            if (elapsedMs) *elapsedMs = static_cast<long>(since(begin));
            return Outcome::Cancelled;
        }
    }
}

} // namespace apertar::primarywait
