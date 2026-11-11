#pragma once
// Camera by port, subdev by of_node (dual-sensor design §3.2). Pure: std and BootConfigText.hpp only, no libcamera.
//
// A Core names its camera by CSI port: cam0 and cam1 resolve through /proc/device-tree/__symbols__/i2c_csi_dsi0|1 to
// the I2C node the sensor hangs on (rp1/i2c@88000|70000), which is part of the libcamera camera id and the prefix of
// the sensor subdev's of_node. Any other value names the I2C node itself (`i2c@88000`). Every function takes a root
// prefix for /proc, /sys and /boot ("" on the Pi) so a host test can run it on a fake tree.
// Host-tested on a fake tree (camera_port_test.cpp).
#include "hardware/BootConfigText.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace apertar::cameraport {

namespace fs = std::filesystem;

// Where a subdev's device/of_node canonicalises to, below the root.
inline constexpr const char *kDeviceTreeBase = "/sys/firmware/devicetree/base";

// Set by the secondary's session when its acquire fails with EBUSY (a dead cam0 let the primary take cam1):
// main() then exits 0 instead of 1, so Restart=on-failure does not loop.
inline std::atomic<bool> &busyExitRequested()
{
    static std::atomic<bool> flag{false};
    return flag;
}

inline fs::path rooted(const fs::path &root, const char *absolute)
{
    return root.empty() ? fs::path(absolute) : fs::path(root.string() + absolute);
}

// A text file up to its first NUL, trailing whitespace cut (device-tree strings end in a NUL).
inline std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t nul = text.find('\0');
    if (nul != std::string::npos) text.erase(nul);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

inline bool endsWith(const std::string &text, const std::string &tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

inline bool startsWith(const std::string &text, const std::string &head)
{
    return text.compare(0, head.size(), head) == 0;
}

inline const char *portSymbol(const std::string &port)
{
    if (port == "cam0") return "i2c_csi_dsi0";
    if (port == "cam1") return "i2c_csi_dsi1";
    return "";
}

// The I2C node a Core's camera hangs on.
struct Node {
    bool valid = false;
    std::string port;   // "cam0", "cam1", or "" for a node no __symbols__ entry names
    std::string path;   // device-tree path, e.g. /axi/pcie@1000120000/rp1/i2c@88000
};

inline std::string symbolPath(const fs::path &root, const std::string &symbol)
{
    if (symbol.empty()) return {};
    return readText(rooted(root, "/proc/device-tree/__symbols__") / symbol);
}

inline std::string portOfNodePath(const fs::path &root, const std::string &path)
{
    if (path.empty()) return {};
    for (const char *port : {"cam0", "cam1"})
        if (symbolPath(root, portSymbol(port)) == path) return port;
    return {};
}

namespace detail {
// First node under `dir` (device-tree path `rel`) whose path equals `name` or ends in "/name". Symlinks are not
// followed below the root; the __symbols__, __fixups__ and similar subtrees are skipped.
inline std::string walkForNode(const fs::path &dir, const std::string &rel, const std::string &name, int depth)
{
    if (depth > 12) return {};
    std::error_code ec;
    std::vector<fs::path> children;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code stat;
        if (entry.is_symlink(stat) || !entry.is_directory(stat)) continue;
        if (entry.path().filename().string().rfind("__", 0) == 0) continue;
        children.push_back(entry.path());
    }
    std::sort(children.begin(), children.end());
    for (const auto &child : children) {
        const std::string childRel = rel + "/" + child.filename().string();
        if (childRel == name || endsWith(childRel, "/" + name)) return childRel;
        const std::string found = walkForNode(child, childRel, name, depth + 1);
        if (!found.empty()) return found;
    }
    return {};
}
} // namespace detail

// "cam0" / "cam1" resolve through __symbols__; any other value is the I2C node itself, its port the symbol that
// names it. valid is false when nothing matches (no device tree, no such node).
inline Node resolve(const fs::path &root, const std::string &value)
{
    Node node;
    if (value == "cam0" || value == "cam1") {
        node.path = symbolPath(root, portSymbol(value));
        node.port = value;
    } else if (!value.empty()) {
        node.path = detail::walkForNode(rooted(root, "/proc/device-tree"), "", value, 0);
        node.port = portOfNodePath(root, node.path);
    }
    node.valid = !node.path.empty();
    if (!node.valid) node.port.clear();
    return node;
}

// True when a libcamera camera id (/base/axi/.../rp1/i2c@88000/imx585@1a) lies under the node.
inline bool cameraIdUnder(const std::string &cameraId, const std::string &nodePath)
{
    return !nodePath.empty() && cameraId.find(nodePath + "/") != std::string::npos;
}

// The port a camera id belongs to, "" when it is under neither cam0's nor cam1's node.
inline std::string portOfCameraId(const fs::path &root, const std::string &cameraId)
{
    for (const char *port : {"cam0", "cam1"})
        if (cameraIdUnder(cameraId, symbolPath(root, portSymbol(port)))) return port;
    return {};
}

// `ofNode` is a subdev's canonical device/of_node with the root and /sys/firmware/devicetree/base cut off
// (/axi/.../i2c@88000/imx585@1a); true when it lies below nodePath.
inline bool ofNodeUnder(const std::string &ofNode, const std::string &nodePath)
{
    return !nodePath.empty() && startsWith(ofNode, nodePath + "/");
}

struct Subdev {
    std::string node;     // v4l-subdev5
    std::string name;     // "imx585 10-001a"
    std::string ofNode;   // see ofNodeUnder; "" when the subdev has no of_node
};

// A subdev's of_node below the device-tree base, "" when it has none. The base is canonicalised too, so a root that
// sits behind a symlink (macOS /tmp) compares true.
inline std::string ofNodeOf(const fs::path &root, const fs::path &subdevDir)
{
    std::error_code ec;
    const fs::path ofNode = fs::canonical(subdevDir / "device/of_node", ec);
    if (ec) return {};
    const fs::path base = fs::canonical(rooted(root, kDeviceTreeBase), ec);
    if (ec) return {};
    const std::string full = ofNode.string();
    const std::string prefix = base.string();
    return startsWith(full, prefix + "/") ? full.substr(prefix.size()) : std::string();
}

// Every v4l-subdev, ordered by its number.
inline std::vector<Subdev> scanSubdevs(const fs::path &root)
{
    std::vector<Subdev> found;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(rooted(root, "/sys/class/video4linux"), ec)) {
        if (ec) break;
        Subdev subdev;
        subdev.node = entry.path().filename().string();
        if (subdev.node.rfind("v4l-subdev", 0) != 0) continue;
        std::ifstream nameFile(entry.path() / "name");
        std::getline(nameFile, subdev.name);
        subdev.ofNode = ofNodeOf(root, entry.path());
        found.push_back(std::move(subdev));
    }
    std::sort(found.begin(), found.end(), [](const Subdev &a, const Subdev &b) {
        return a.node.size() != b.node.size() ? a.node.size() < b.node.size() : a.node < b.node;
    });
    return found;
}

// The subdevs below nodePath; every subdev when nodePath is empty (no scope).
inline std::vector<Subdev> subdevsUnder(const fs::path &root, const std::string &nodePath)
{
    std::vector<Subdev> all = scanSubdevs(root);
    if (nodePath.empty()) return all;
    all.erase(std::remove_if(all.begin(), all.end(), [&](const Subdev &s) { return !ofNodeUnder(s.ofNode, nodePath); }),
              all.end());
    return all;
}

// A sensor, as opposed to the lens driver (VCM) that shares its I2C bus: the focus drivers' names start with these.
inline bool isSensorName(const std::string &name)
{
    for (const char *lens : {"dw9", "ak7", "lc89", "ad5", "bu64", "vcm"})
        if (name.rfind(lens, 0) == 0) return false;
    return !name.empty();
}

inline bool sensorSubdevUnder(const fs::path &root, const std::string &nodePath)
{
    if (nodePath.empty()) return false;
    for (const Subdev &subdev : subdevsUnder(root, nodePath))
        if (isSensorName(subdev.name)) return true;
    return false;
}

// The first subdev below nodePath (all when empty) whose name contains `needle`: "v4l-subdev5", or "".
inline std::string findSubdevNode(const fs::path &root, const std::string &nodePath, const std::string &needle)
{
    for (const Subdev &subdev : subdevsUnder(root, nodePath))
        if (subdev.name.find(needle) != std::string::npos) return subdev.node;
    return {};
}

// ---- config.txt ----

// config.txt, or the copy APERTAR_BOOT_CONFIG names (a dev machine, a test), as ApertarCoreApp's bootConfigPath().
inline std::string bootConfigText(const fs::path &root)
{
    if (const char *path = std::getenv("APERTAR_BOOT_CONFIG"); path && *path) return readText(path);
    std::ifstream in(rooted(root, "/boot/firmware/config.txt"), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline bootconf::Board boardOf(const fs::path &root)
{
    const fs::path dir = rooted(root, "/proc/device-tree");
    std::ifstream compatible(dir / "compatible", std::ios::binary);
    std::ifstream model(dir / "model", std::ios::binary);
    return bootconf::boardFromDeviceTree(std::string((std::istreambuf_iterator<char>(compatible)), std::istreambuf_iterator<char>()),
                                         std::string((std::istreambuf_iterator<char>(model)), std::istreambuf_iterator<char>()));
}

// True when an active dtoverlay line for a camera sensor loads on the port (cam0 / cam1).
inline bool configTextNamesPort(const std::string &text, const bootconf::Board &board, const std::string &port)
{
    if (port != "cam0" && port != "cam1") return false;
    const bootconf::State state = bootconf::readState(bootconf::parseDocument(text, board), board);
    const auto it = state.cameras.find(port);
    return it != state.cameras.end() && it->second.line != 0;
}

inline bool configNamesPort(const fs::path &root, const std::string &port)
{
    return configTextNamesPort(bootConfigText(root), boardOf(root), port);
}

// ---- waiting ----

// Looks for a sensor subdev below nodePath every pollMs until timeoutMs has passed (once when timeoutMs is 0).
inline bool waitForSensorSubdev(const fs::path &root, const std::string &nodePath, int timeoutMs, int pollMs = 500)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (sensorSubdevUnder(root, nodePath)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
}

// The secondary's check in main(), before app.start(): is there a sensor on its port? A boot may not have probed one
// that config.txt names, so with `configNamesIt` it looks again every pollMs up to timeoutMs; without, at once.
// Fills *why with the one log line when there is none.
inline bool secondaryHasSensor(const fs::path &root, const std::string &value, bool configNamesIt, int timeoutMs,
                               int pollMs, std::string *why)
{
    const Node node = resolve(root, value);
    if (!node.valid) {
        if (why) *why = "ApertarCore camera " + value + ": no such I2C node or port; nothing to run, exiting.";
        return false;
    }
    if (waitForSensorSubdev(root, node.path, configNamesIt ? timeoutMs : 0, pollMs)) return true;
    if (why)
        *why = "ApertarCore camera " + value + ": no sensor on " + node.path + (configNamesIt ? " after the wait" : "") +
               "; nothing to run, exiting.";
    return false;
}

} // namespace apertar::cameraport
