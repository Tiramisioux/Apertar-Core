// Host test for src/camera/CameraPort.hpp: --check-dual (dualSensorsPresent) on a fake /proc, /sys and /boot tree.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/camera_port_test.cpp -o /tmp/cpt && /tmp/cpt
#include "camera/CameraPort.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace apertar::cameraport;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static const std::string kCam0Node = "/axi/pcie@1000120000/rp1/i2c@88000";
static const std::string kCam1Node = "/axi/pcie@1000120000/rp1/i2c@70000";

static void writeFile(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A Pi 5 with both CSI ports in its device tree, config.txt `config`, and no sensor subdev yet.
static fs::path makeRoot(const std::string &name, const std::string &config)
{
    const char *tmp = std::getenv("TMPDIR");
    const fs::path root = fs::path(tmp && *tmp ? tmp : "/tmp") / ("camera_port_test-" + name);
    fs::remove_all(root);
    writeFile(root / "proc/device-tree/compatible", std::string("raspberrypi,5-model-b\0brcm,bcm2712\0", 35));
    writeFile(root / "proc/device-tree/model", std::string("Raspberry Pi 5 Model B Rev 1.0\0", 31));
    writeFile(root / "proc/device-tree/__symbols__/i2c_csi_dsi0", kCam0Node + std::string(1, '\0'));
    writeFile(root / "proc/device-tree/__symbols__/i2c_csi_dsi1", kCam1Node + std::string(1, '\0'));
    fs::create_directories(root / ("proc/device-tree" + kCam0Node));
    fs::create_directories(root / ("proc/device-tree" + kCam1Node));
    fs::create_directories(root / "sys/class/video4linux");
    writeFile(root / "boot/firmware/config.txt", config);
    return root;
}

// A sensor subdev (or a lens driver, by name) whose of_node lies under `node`.
static void addSubdev(const fs::path &root, int number, const std::string &name, const std::string &node)
{
    const std::string chip = name.substr(0, name.find(' '));
    const fs::path dtNode = root / ("sys/firmware/devicetree/base" + node + "/" + chip + "@1a");
    fs::create_directories(dtNode);
    const fs::path device = root / ("sys/devices/i2c-dev" + std::to_string(number));
    fs::create_directories(device);
    fs::create_directory_symlink(dtNode, device / "of_node");
    const fs::path subdev = root / ("sys/class/video4linux/v4l-subdev" + std::to_string(number));
    writeFile(subdev / "name", name + "\n");
    fs::create_directory_symlink(device, subdev / "device");
}

static const std::string kBoth = "[pi5]\ndtoverlay=imx585,cam0,link-frequency=891000000\n"
                                 "dtoverlay=imx585,link-frequency=891000000\n";
static const std::string kCam0Line = "[pi5]\ndtoverlay=imx585,cam0\n";
static const std::string kCam1Line = "[pi5]\ndtoverlay=imx585\n";

int main()
{
    ::unsetenv("APERTAR_BOOT_CONFIG");
    std::string why;

    {
        const fs::path root = makeRoot("both", kBoth);
        addSubdev(root, 2, "imx585 4-001a", kCam0Node);
        addSubdev(root, 5, "imx585 6-001a", kCam1Node);
        check(dualSensorsPresent(root, "cam1", 2000, 50, &why), "two sensors: cam1 runs");
        check(why == "ApertarCore dual check: a sensor on cam0 and on cam1; two cameras.", "two sensors: the log line");
        check(!dualSensorsPresent(root, "cam0", 0, 50, &why), "two sensors, --camera cam0: cam0 is the primary's port");
        check(!dualSensorsPresent(root, "", 0, 50, &why), "two sensors, no --camera: no port to check");
        check(dualSensorsPresent(root, "i2c@70000", 0, 50, &why), "two sensors, cam1 by its I2C node");
        check(!dualSensorsPresent(root, "i2c@88000", 0, 50, &why), "two sensors, cam0 by its I2C node: one camera");
    }
    {
        const fs::path root = makeRoot("cam0-only", kBoth);
        addSubdev(root, 2, "imx585 4-001a", kCam0Node);
        const auto start = std::chrono::steady_clock::now();
        check(!dualSensorsPresent(root, "cam1", 300, 50, &why), "cam0 only, cam1 in config.txt: skipped");
        const auto waited = std::chrono::steady_clock::now() - start;
        check(waited >= std::chrono::milliseconds(300), "cam0 only, cam1 in config.txt: waits the bound for a late probe");
        check(why == "ApertarCore dual check: no sensor on cam1 (" + kCam1Node + ") after the wait; one camera.",
              "cam0 only: the log line names cam1's node");
    }
    {
        const fs::path root = makeRoot("cam0-only-noline", kCam0Line);
        addSubdev(root, 2, "imx585 4-001a", kCam0Node);
        const auto start = std::chrono::steady_clock::now();
        check(!dualSensorsPresent(root, "cam1", 5000, 50, &why), "cam0 only, no cam1 line in config.txt: skipped");
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000),
              "no cam1 line in config.txt: no wait");
        check(why == "ApertarCore dual check: no sensor on cam1 (" + kCam1Node + "); one camera.",
              "no cam1 line: the log line has no wait");
    }
    {
        const fs::path root = makeRoot("cam1-only", kBoth);
        addSubdev(root, 5, "imx585 6-001a", kCam1Node);
        check(!dualSensorsPresent(root, "cam1", 300, 50, &why), "cam1 only: the primary takes it, cam1 is skipped");
        check(why == "ApertarCore dual check: no sensor on cam0 (" + kCam0Node + ") after the wait; one camera.",
              "cam1 only: the log line names cam0's node");
    }
    {
        const fs::path root = makeRoot("cam1-only-noline", kCam1Line);
        addSubdev(root, 5, "imx585 6-001a", kCam1Node);
        const auto start = std::chrono::steady_clock::now();
        check(!dualSensorsPresent(root, "cam1", 5000, 50, &why), "cam1 only, no cam0 line: skipped at once");
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000), "no cam0 line: no wait");
    }
    {
        const fs::path root = makeRoot("none", kBoth);
        check(!dualSensorsPresent(root, "cam1", 200, 50, &why), "no sensor: skipped");
        const fs::path empty = makeRoot("none-noline", "");
        check(!dualSensorsPresent(empty, "cam1", 5000, 50, &why), "no sensor, no lines: skipped");
    }
    {
        const fs::path root = makeRoot("lens", kBoth);
        addSubdev(root, 2, "imx585 4-001a", kCam0Node);
        addSubdev(root, 6, "dw9807 6-000c", kCam1Node);
        check(!dualSensorsPresent(root, "cam1", 0, 50, &why), "a lens driver on cam1 is no sensor");
    }
    {
        const fs::path root = makeRoot("late", kBoth);
        addSubdev(root, 2, "imx585 4-001a", kCam0Node);
        std::thread late([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            addSubdev(root, 5, "imx585 6-001a", kCam1Node);
        });
        check(dualSensorsPresent(root, "cam1", 3000, 50, &why), "cam1 probed late, within the bound: cam1 runs");
        late.join();
    }
    {
        const fs::path root = makeRoot("no-dt", kBoth);
        fs::remove_all(root / "proc/device-tree/__symbols__");
        check(!dualSensorsPresent(root, "cam1", 0, 50, &why), "no device tree symbols: one camera");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
