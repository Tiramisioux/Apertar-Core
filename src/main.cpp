#include "ApertarCoreApp.hpp"
#include "camera/CameraPort.hpp"
#include "ipc/PrimaryWait.hpp"

#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

volatile sig_atomic_t shutdownSignalFd = -1;

void handleSignal(int)
{
    const int savedErrno = errno;
    const int fd = shutdownSignalFd;
    if (fd >= 0) {
        const char wake = 1;
        const auto ignored = ::write(fd, &wake, 1);
        (void)ignored;
    }
    errno = savedErrno;
}

void printUsage(const char *argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --socket PATH       Control socket path (default: /tmp/apertar-core.sock)\n"
        << "  --media PATH        Recording media root (default: /media/RAW)\n"
        << "  --web-bind ADDR     Web control bind address (default: 0.0.0.0)\n"
        << "  --web-port PORT     Web control port, 0 disables it (default: 8000)\n"
        << "  --camera PORT       Camera by port: cam0 (default), cam1, or an I2C node such as i2c@88000\n"
        << "  --simulate          Run without opening the camera yet\n"
        << "  --help              Show this help\n";
}

} // namespace

int main(int argc, char **argv)
{
    apertar::ApertarCoreApp::Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--simulate") {
            options.simulateCamera = true;
            continue;
        }
        if (arg == "--socket" && i + 1 < argc) {
            options.socketPath = argv[++i];
            continue;
        }
        if (arg == "--media" && i + 1 < argc) {
            options.mediaRoot = argv[++i];
            continue;
        }
        if (arg == "--web-bind" && i + 1 < argc) {
            options.webBindAddress = argv[++i];
            continue;
        }
        if (arg == "--camera" && i + 1 < argc) {
            options.camera = argv[++i];
            continue;
        }
        if (arg == "--web-port" && i + 1 < argc) {
            const std::string value = argv[++i];
            try {
                const unsigned long parsed = std::stoul(value);
                if (parsed > 65535) {
                    std::cerr << "Invalid web port: " << value << "\n";
                    return 2;
                }
                options.webPort = static_cast<uint16_t>(parsed);
            } catch (...) {
                std::cerr << "Invalid web port: " << value << "\n";
                return 2;
            }
            continue;
        }

        std::cerr << "Unknown or incomplete option: " << arg << "\n";
        printUsage(argv[0]);
        return 2;
    }

    // Camera shutdown takes locks and joins threads: never do it inside a
    // signal handler, which may interrupt a thread already holding those locks.
    int signalPipe[2];
    if (::pipe2(signalPipe, O_CLOEXEC) != 0 ||
        ::fcntl(signalPipe[1], F_SETFL, O_NONBLOCK) < 0) {
        std::cerr << "ApertarCore could not configure shutdown signals.\n";
        return 1;
    }

    shutdownSignalFd = signalPipe[1];
    struct sigaction action{};
    action.sa_handler = handleSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);

    // A secondary with no sensor on its port exits 0 before app.start(): start() restores the low-power state and
    // starts threads before the camera, and a failed start exits 1, which Restart=on-failure would loop. sysfs only,
    // no libcamera call, so the primary's camera is never touched.
    if (!options.simulateCamera && !options.camera.empty() && options.camera != "cam0") {
        const auto node = apertar::cameraport::resolve({}, options.camera);
        std::string why;
        if (!apertar::cameraport::secondaryHasSensor({}, options.camera,
                                                     node.valid && apertar::cameraport::configNamesPort({}, node.port),
                                                     20000, 500, &why)) {
            std::cerr << why << "\n";
            return 0;
        }
    }

    // Start order (design 3.4, P10): a secondary waits for the primary's socket to answer (30 s), then for the primary
    // to be idle, before its first camera start; a signal ends the wait. Without the primary's socket it starts after
    // 30 s without the idle wait. The primary does not wait.
    if (!options.simulateCamera && apertar::cameraport::isSecondary(options.camera)) {
        std::cerr << "ApertarCore secondary wait: up to " << apertar::primarywait::kSocketTimeoutMs / 1000
                  << " s for the primary's socket " << apertar::primarywait::kPrimarySocket
                  << ", then until the primary is idle\n";
        long waitedMs = 0;
        const auto outcome = apertar::primarywait::waitForPrimaryIdle(
            apertar::primarywait::kPrimarySocket, apertar::primarywait::kSocketTimeoutMs, apertar::primarywait::kPollMs,
            [&](int ms) {
                pollfd stop{signalPipe[0], POLLIN, 0};
                return ::poll(&stop, 1, ms) > 0;
            }, &waitedMs);
        if (outcome == apertar::primarywait::Outcome::Cancelled) {
            std::cerr << "ApertarCore secondary wait: stopped by a signal\n";
            return 0;
        }
        if (outcome == apertar::primarywait::Outcome::TimedOut)
            std::cerr << "ApertarCore secondary wait: the primary's socket did not answer within "
                      << apertar::primarywait::kSocketTimeoutMs / 1000 << " s; starting without the idle wait\n";
        else
            std::cerr << "ApertarCore secondary wait: the primary is idle after " << waitedMs << " ms; starting\n";
    }

    apertar::ApertarCoreApp app(options);

    if (!app.start()) {
        if (apertar::cameraport::busyExitRequested().load()) {
            std::cerr << "ApertarCore camera " << options.camera << " is held by another Core; exiting.\n";
            return 0;
        }
        std::cerr << "ApertarCore failed to start.\n";
        return 1;
    }

    std::thread signalThread([&] {
        char wake;
        ssize_t result;
        do {
            result = ::read(signalPipe[0], &wake, 1);
        } while (result < 0 && errno == EINTR);
        app.requestShutdown();
    });

    std::cout << "ApertarCore running on " << options.socketPath << "\n";
    if (options.webPort != 0) {
        std::cout << "ApertarCore web control available on http://"
                  << options.webBindAddress
                  << ":" << options.webPort
                  << "\n";
    }
    app.wait();
    const char wake = 1;
    const auto ignored = ::write(signalPipe[1], &wake, 1);
    (void)ignored;
    signalThread.join();
    app.stop();
    shutdownSignalFd = -1;
    ::close(signalPipe[0]);
    ::close(signalPipe[1]);

    std::cout << "ApertarCore stopped.\n";
    return 0;
}
