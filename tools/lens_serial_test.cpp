// Host test for src/hardware/Cef168Serial.cpp against a pseudo-terminal: a thread plays the CEF168 board (v, n, h, l, a, d; er for the
// rest; silent, nc and garbage modes) and the port's thread must connect, follow a zoom, drop and recover the link, pause, and stay
// quiet with no device. Build:
//   g++ -std=c++20 -Wall -I src tools/lens_serial_test.cpp src/hardware/Cef168Serial.cpp -o /tmp/lens_serial_test -lutil -lpthread

#include "hardware/Cef168Serial.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <poll.h>
#include <pty.h>
#include <unistd.h>

namespace ser = apertar::hardware::cef168::serial;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

namespace {

enum Mode { Normal = 0, Silent = 1, NotConnected = 2, Garbage = 3 };

struct Board {
    int master = -1;
    std::atomic<int> mode{Normal};
    std::atomic<int> focal{12};
    std::atomic<int> commands{0};
    std::mutex mutex;
    std::string aperture = "f/3.5-22.6";
    std::atomic<bool> stop{false};
    std::thread thread;

    void say(const std::string &text) { (void)!::write(master, text.data(), text.size()); }
    void setAperture(const std::string &text) { std::lock_guard<std::mutex> lock(mutex); aperture = text; }
    std::string getAperture() { std::lock_guard<std::mutex> lock(mutex); return aperture; }

    void reply(const std::string &command)
    {
        ++commands;
        const int m = mode;
        if (m == Silent) return;
        if (m == Garbage && command != "v" && command != "n") { say("\x01zz#$\r\n"); return; }
        if (command == "v") say("1.3\r\n");
        else if (command == "n") say("A1B2C3\r\n");
        else if (command == "h") {
            for (const char *line : {"Lens ID: 0xEB\r\n", "Focal length: 10-22 mm\r\n", "Zoom: 1x\r\n", "Aperture: f/3.5-22.6\r\n", "Focus distance: 0.23-0.23 m\r\n"}) {
                say(line);
                std::this_thread::sleep_for(5ms);
            }
        } else if (command == "l") say(m == NotConnected ? "nc\r\n" : std::to_string(focal.load()) + "\r\n");
        else if (command == "a") say(m == NotConnected ? "nc\r\n" : getAperture() + "\r\n");
        else if (command == "d") say(m == NotConnected ? "nc\r\n" : "0.23-0.23\r\n");
        else say("er\r\n");
    }

    void run()
    {
        std::string buffer;
        while (!stop) {
            pollfd pfd{master, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) continue;
            char chunk[64];
            const ssize_t got = ::read(master, chunk, sizeof(chunk));
            if (got <= 0) continue;
            buffer.append(chunk, static_cast<size_t>(got));
            size_t at;
            while ((at = buffer.find('\r')) != std::string::npos) {
                const std::string command = buffer.substr(0, at);
                buffer.erase(0, at + 1);
                reply(command);
            }
        }
    }
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds limit = 4000ms)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

} // namespace

int main()
{
    Board board;
    int slave = -1;
    char name[128] = {};
    if (::openpty(&board.master, &slave, name, nullptr, nullptr) != 0) {
        std::perror("openpty");
        return 2;
    }
    board.thread = std::thread([&] { board.run(); });

    ser::Timing timing;
    timing.replyTimeout = 120ms;
    timing.quietGap = 40ms;
    timing.poll = 30ms;
    timing.distancePoll = 60ms;
    timing.retry = 100ms;
    timing.failuresToDrop = 3;
    {
        ser::Cef168Serial port(timing);
        std::atomic<int> changes{0};
        port.setOnChange([&] { ++changes; });

        port.configure("");   // off: no thread, no traffic
        std::this_thread::sleep_for(100ms);
        CHECK(board.commands == 0 && !port.values().connected && port.values().device.empty());

        port.configure(name);
        CHECK(waitFor([&] { return port.values().connected && port.values().focalLengthMm; }));
        ser::Values v = port.values();
        CHECK(v.firmware == "1.3" && v.boardSerial == "A1B2C3" && v.lensConnected && v.lensId == 0xEB);
        CHECK(v.focalMinMm == 10.0 && v.focalMaxMm == 22.0 && v.focalLengthMm == 12.0);
        CHECK(v.apertureRange && v.apertureRange->first == 3.5 && v.apertureRange->second == 22.6);
        CHECK(waitFor([&] { return port.values().distance.has_value(); }));
        CHECK(port.values().distance && port.values().distance->minMetres == 0.23 && port.values().error.empty());
        CHECK(changes > 0);

        // the zoom moves: focal length and the aperture range follow; the ends are remembered
        board.focal = 22;
        board.setAperture("4.5-29");
        CHECK(waitFor([&] { return port.values().focalLengthMm == 22.0 && port.values().apertureRange && port.values().apertureRange->first == 4.5; }));
        CHECK(port.values().apertureAtMaxFocal == 4.5);
        board.focal = 10;
        board.setAperture("f/3.5-22.6");
        CHECK(waitFor([&] { return port.values().focalLengthMm == 10.0 && port.values().apertureAtMinFocal == 3.5; }));

        // a noisy line: connected stays, the old values stay
        board.mode = Garbage;
        std::this_thread::sleep_for(250ms);
        CHECK(port.values().connected && port.values().apertureRange && port.values().apertureRange->first == 3.5);

        // a lens that is not on the board: nc
        board.mode = NotConnected;
        CHECK(waitFor([&] { return !port.values().lensConnected; }));
        CHECK(port.values().connected && !port.values().focalLengthMm && !port.values().apertureRange);
        board.mode = Normal;
        CHECK(waitFor([&] { return port.values().lensConnected && port.values().focalLengthMm == 10.0; }));

        // the board stops answering: the link drops with an error, then recovers
        board.mode = Silent;
        CHECK(waitFor([&] { return !port.values().connected; }));
        CHECK(!port.values().error.empty());
        board.mode = Normal;
        CHECK(waitFor([&] { return port.values().connected && port.values().focalLengthMm == 10.0; }));

        // paused (a calibration runs): no traffic
        port.setPaused(true);
        std::this_thread::sleep_for(150ms);
        const int before = board.commands;
        std::this_thread::sleep_for(300ms);
        CHECK(board.commands == before);
        port.setPaused(false);
        CHECK(waitFor([&] { return board.commands > before; }));

        // another lens: `h` runs again
        port.setLensHint(1);
        const int hBefore = board.commands;
        port.setLensHint(2);
        CHECK(waitFor([&] { return board.commands > hBefore + 4; }));

        // a port that does not exist
        port.configure("/dev/does-not-exist-cef168");
        CHECK(waitFor([&] { return !port.values().connected && port.values().error.find("Cannot open") == 0; }));
        // and off again
        port.configure("");
        CHECK(waitFor([&] { return port.values().device.empty() && !port.values().connected; }));
    }   // the destructor joins the thread

    board.stop = true;
    board.thread.join();
    ::close(slave);
    ::close(board.master);
    if (failures == 0) std::puts("lens_serial_test: ok");
    return failures == 0 ? 0 : 1;
}
