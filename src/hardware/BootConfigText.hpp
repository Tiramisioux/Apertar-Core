#pragma once
// /boot/firmware/config.txt for the WebUI's System -> Boot config card. Pure: no I/O.
//
// Ported from Cinemate's settings-editor config.txt tab (cinemate src/module/app/boot_config.py): read the file into
// a state the page can show, write a state back by touching only the lines that carry it, refuse a save that would
// not do what was asked, and never synthesize a file from a template on a live Pi.
//
// What differs is the file. Cinemate's installer writes a fenced block (# >>> cinemate-install >>>) and the editor
// rewrites inside it. Apertar has no installer for config.txt: its image ships a sectioned file ([pi4] [cm4] [cm5]
// [pi5] [all], then a DPI display list), whose own comments say how to change it: "uncomment the dtoverlay for your
// sensor", "uncomment some or all of these to enable the optional hardware interfaces", "uncomment the dtoverlays for
// the one you are currently using". This code does exactly that and nothing else, so a saved file is still the Apertar
// layout, line for line:
//
//   camera      dtoverlay=<sensor>[,cam0][,mono][,link-frequency=<Hz>]   board section ([pi5] on a Pi 5, [pi4] on a Pi 4)
//   interfaces  dtparam=i2c_arm=on, dtparam=i2s=on, dtparam=spi=on       board section
//               dtparam=audio=on                                         [all]
//   RP1 clock   dtoverlay=rp1-clk-333mhz                                 [all]
//   display     the DPI overlays (vc4-kms-DPI-4inch + waveshare-4dpi + waveshare-touch-4dpi, or
//               vc4-kms-dpi-hyperpixel4sq)                               [all], the DPI list at the end
//
// A choice is made by commenting or uncommenting the line that is already there, or by changing its parameters.
// Only when no such line exists is one added, next to its kind. Every other byte, the fan block
// (# BEGIN APERTAR FAN MODE) included, is kept. The new text is then read back, and a result that differs from the
// request (a line this code does not edit still decides it) is refused with that line's number instead of written.
//
// Port names follow the Raspberry Pi overlays: cam0 is the "cam0" parameter, cam1 is the overlay's default and is
// written without a port word. camera_auto_detect stays 0, as the Apertar file has it: every sensor is named.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace apertar::bootconf {

// ---- small text helpers -------------------------------------------------------------------------

inline std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline bool blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

inline std::string trimmed(std::string_view text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

inline bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

inline std::optional<uint64_t> parseHz(std::string_view text)
{
    if (text.empty() || text.size() > 12) return std::nullopt;
    uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(ch - '0');
    }
    return value;
}

// ---- the board ----------------------------------------------------------------------------------

struct Board {
    std::string family;          // "pi5" (BCM2712), "pi4" (BCM2711), "pi3" (BCM2837/BCM2710)
    std::string model;           // /proc/device-tree/model, shown on the page
    bool computeModule = false;
    bool detected = false;       // false: no device tree (a dev machine). Read as a Pi 5, Apertar's main board.
};

// compatible and model as read from /proc/device-tree (NUL-separated, NUL-terminated).
inline Board boardFromDeviceTree(std::string compatible, std::string model)
{
    std::replace(compatible.begin(), compatible.end(), '\0', ',');
    model.erase(std::remove(model.begin(), model.end(), '\0'), model.end());
    Board board;
    board.model = trimmed(model);
    const std::string c = lower(compatible);
    if (contains(c, "bcm2712")) board.family = "pi5";
    else if (contains(c, "bcm2711")) board.family = "pi4";
    else if (contains(c, "bcm2837") || contains(c, "bcm2710")) board.family = "pi3";
    board.detected = !board.family.empty();
    if (!board.detected) board.family = "pi5";
    board.computeModule = contains(lower(board.model), "compute module");
    return board;
}

// The firmware's conditional filters, for the boards Apertar runs on. A filter this does not know ([gpio4=1],
// [edid=...], [pi0], ...) is read as not applying: the lines under it are left alone and do not count.
inline bool filterApplies(const std::string &filter, const Board &board)
{
    if (filter.empty() || filter == "all") return true;
    const std::string model = lower(board.model);
    const bool pi5 = board.family == "pi5", pi4 = board.family == "pi4", pi3 = board.family == "pi3";
    if (filter == "pi5") return pi5;
    if (filter == "cm5") return pi5 && board.computeModule;
    if (filter == "pi500") return pi5 && contains(model, "500");
    if (filter == "pi4") return pi4;
    if (filter == "cm4") return pi4 && board.computeModule;
    if (filter == "cm4s") return pi4 && contains(model, "compute module 4s");
    if (filter == "pi400") return pi4 && contains(model, "400");
    if (filter == "pi3") return pi3;
    if (filter == "pi3+") return pi3 && contains(model, "plus");
    if (filter == "pi02") return pi3 && contains(model, "zero 2");
    if (filter == "cm3") return pi3 && board.computeModule;
    return false;
}

// The section the Apertar file keeps the board's camera and interface lines in.
inline std::string boardSection(const Board &board) { return board.family; }

// cam0 exists on boards with two camera connectors (Pi 5, the compute modules); a Pi 4 or Pi 3 has one, cam1.
inline std::vector<std::string> boardPorts(const Board &board)
{
    if (board.family == "pi5" || board.computeModule) return {"cam0", "cam1"};
    return {"cam1"};
}

// ---- what the card offers -----------------------------------------------------------------------

struct LinkOption {
    uint64_t hz;
    unsigned mbpsPerLane;
    double fps4k;                 // published 4K 12-bit 4-lane frame rate, 0 = none published
    bool overRp1Spec;
    bool experimental;
};

struct Sensor {
    std::string_view key;          // the page's value
    std::string_view overlay;      // dtoverlay name
    std::string_view label;
    bool mono;
    uint64_t defaultHz;            // the overlay's own link frequency; 0 = no link-frequency menu
    uint64_t pi4MaxHz;             // options above this are left off on a Pi 4; 0 = no limit
    std::vector<LinkOption> options;
};

// The sensors Core has handling for (CameraController detectSensorFamily: imx577, imx477, imx294, imx283, imx585).
// Link-frequency values are Cinemate's resources/sensors.json: the will127534 IMX585 driver table (1188 MHz left
// out: frame drops on a Pi 5), the IMX477 presets (above 750 MHz: experimental, over the RP1 spec) and the IMX283's
// two (the driver's only register sequences; 720 MHz is its ceiling, and 360 MHz needs the overlay's link-frequency
// parameter, which Tiramisioux/imx283-v4l2-driver has from 257c9cf: an older overlay does not enumerate the camera).
inline const std::vector<Sensor> &sensors()
{
    static const std::vector<LinkOption> imx585Options = {
        {297000000, 594, 20.8, false, false},  {360000000, 720, 25.0, false, false},
        {445500000, 891, 30.0, false, false},  {594000000, 1188, 41.7, false, false},
        {720000000, 1440, 50.0, false, false}, {891000000, 1782, 60.0, true, false},
        {1039500000, 2079, 75.0, true, false},
    };
    static const std::vector<Sensor> list = {
        {"imx477", "imx477", "Raspberry Pi HQ Camera (IMX477)", false, 450000000, 500000000, {
            {450000000, 900, 0, false, false}, {720000000, 1440, 0, false, false}, {750000000, 1500, 0, false, false},
            {891000000, 1782, 0, true, true},  {909000000, 1818, 0, true, true},  {918000000, 1836, 0, true, true},
            {945000000, 1890, 0, true, true},  {972000000, 1944, 0, true, true}}},
        {"imx577", "imx577", "IMX577", false, 0, 0, {}},
        {"imx283", "imx283", "OneInchEye (IMX283)", false, 720000000, 0, {
            {360000000, 720, 0, false, false}, {720000000, 1440, 0, false, false}}},
        {"imx585", "imx585", "StarlightEye (IMX585, colour)", false, 720000000, 891000000, imx585Options},
        {"imx585_mono", "imx585", "StarlightEye (IMX585, mono)", true, 720000000, 891000000, imx585Options},
        {"imx294", "imx294", "IMX294", false, 0, 0, {}},
    };
    return list;
}

inline const Sensor *findSensor(std::string_view key)
{
    for (const auto &sensor : sensors())
        if (sensor.key == key) return &sensor;
    return nullptr;
}

inline std::vector<LinkOption> linkOptionsFor(const Sensor &sensor, const Board &board)
{
    std::vector<LinkOption> out;
    for (const auto &option : sensor.options)
        if (board.family != "pi4" || !sensor.pi4MaxHz || option.hz <= sensor.pi4MaxHz) out.push_back(option);
    return out;
}

// "720 MHz · 1440 Mbps/lane · up to 50 fps at 4K (default)"
inline std::string linkOptionLabel(const LinkOption &option, uint64_t defaultHz)
{
    char mhz[32];
    if (option.hz % 1000000 == 0) std::snprintf(mhz, sizeof(mhz), "%llu MHz", static_cast<unsigned long long>(option.hz / 1000000));
    else std::snprintf(mhz, sizeof(mhz), "%.1f MHz", static_cast<double>(option.hz) / 1e6);
    std::string label = mhz;
    label += " · " + std::to_string(option.mbpsPerLane) + " Mbps/lane";
    if (option.fps4k > 0) {
        char fps[32];
        std::snprintf(fps, sizeof(fps), "%g", option.fps4k);
        label += std::string(" · up to ") + fps + " fps at 4K";
    }
    if (option.hz == defaultHz) label += " (default)";
    if (option.experimental) label += " — experimental";
    if (option.overRp1Spec) label += " — over RP1 spec";
    return label;
}

struct Display {
    std::string_view key;
    std::string_view label;
    std::vector<std::string_view> overlays;   // as the Apertar file spells them
};

inline const std::vector<Display> &displays()
{
    static const std::vector<Display> list = {
        {"hdmi", "HDMI only (no DPI panel)", {}},
        {"waveshare4", "Waveshare 4\" square DPI", {"vc4-kms-DPI-4inch", "waveshare-4dpi", "waveshare-touch-4dpi"}},
        {"hyperpixel4sq", "Hyperpixel 4\" square DPI", {"vc4-kms-dpi-hyperpixel4sq"}},
    };
    return list;
}

inline const Display *findDisplay(std::string_view key)
{
    for (const auto &display : displays())
        if (display.key == key) return &display;
    return nullptr;
}

constexpr std::string_view kRp1ClockOverlay = "rp1-clk-333mhz";

inline bool isCameraOverlay(std::string_view name)
{
    const std::string n = lower(name);
    for (const std::string_view prefix : {"imx", "ov", "ar"})
        if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0 &&
            std::isdigit(static_cast<unsigned char>(n[prefix.size()])))
            return true;
    return false;
}
inline bool isRp1ClockOverlay(std::string_view name)
{
    const std::string n = lower(name);
    return n.rfind("rp1-clk", 0) == 0 || n == "rp1-overclock";
}
// Same rule as HardwareConfigText's parseConfigTxt (the I2C card's DPI check).
inline bool isDpiOverlay(std::string_view name)
{
    const std::string n = lower(name);
    return contains(n, "dpi") || contains(n, "hyperpixel");
}

// ---- lines --------------------------------------------------------------------------------------

struct Line {
    std::string raw;               // as in the file, without its '\n' ('\r' kept)
    std::string filter;            // the [section] in force: lowercase, no brackets, "" before the first one
    bool applies = true;           // that section applies to this board
    bool header = false;           // a [section] line
    bool comment = false;          // '#' is the first non-blank character
    bool fenced = false;           // inside a # BEGIN APERTAR ... / # END APERTAR ... block (the fan block): never edited
    std::string key;               // lowercase key of key=value (of the text after the '#' on a commented line)
    std::string value;             // trimmed, an inline "# ..." cut off
    size_t valueBegin = 0, valueEnd = 0;   // the value's span in raw
};

// One line, no section context.
inline Line lineFrom(std::string raw)
{
    Line line;
    line.raw = std::move(raw);
    const std::string &s = line.raw;
    size_t end = s.size();
    while (end > 0 && s[end - 1] == '\r') --end;
    size_t pos = 0;
    while (pos < end && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos < end && s[pos] == '[') {
        const size_t close = s.find(']', pos);
        if (close != std::string::npos && close < end) {
            line.header = true;
            line.key = lower(trimmed(std::string_view(s).substr(pos + 1, close - pos - 1)));
        }
        return line;
    }
    if (pos < end && s[pos] == '#') {
        line.comment = true;
        while (pos < end && s[pos] == '#') ++pos;
        while (pos < end && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    }
    const size_t eq = s.find('=', pos);
    if (eq == std::string::npos || eq >= end) return line;
    const std::string key = lower(trimmed(std::string_view(s).substr(pos, eq - pos)));
    if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; }))
        return line;
    size_t vb = eq + 1;
    while (vb < end && (s[vb] == ' ' || s[vb] == '\t')) ++vb;
    size_t ve = s.find('#', vb);
    if (ve == std::string::npos || ve > end) ve = end;
    while (ve > vb && (s[ve - 1] == ' ' || s[ve - 1] == '\t')) --ve;
    line.key = key;
    line.value = s.substr(vb, ve - vb);
    line.valueBegin = vb;
    line.valueEnd = ve;
    return line;
}

struct Document {
    std::vector<Line> lines;
    bool finalNewline = true;
    bool crlf = false;
};

inline Document parseDocument(const std::string &text, const Board &board)
{
    Document doc;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        doc.lines.push_back(lineFrom(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start)));
        if (nl == std::string::npos) { doc.finalNewline = false; break; }
        start = nl + 1;
    }
    std::string filter;
    bool applies = true;
    bool fenced = false;
    for (auto &line : doc.lines) {
        if (line.header) {
            filter = line.key;
            applies = filterApplies(filter, board);
        }
        line.filter = filter;
        line.applies = applies;
        const std::string text = trimmed(line.raw);
        if (text.rfind("# BEGIN APERTAR ", 0) == 0) fenced = true;
        line.fenced = fenced;
        if (text.rfind("# END APERTAR ", 0) == 0) fenced = false;
    }
    doc.crlf = !doc.lines.empty() && !doc.lines.front().raw.empty() && doc.lines.front().raw.back() == '\r';
    return doc;
}

struct Overlay {
    std::string name;
    std::vector<std::string> params;
};

inline std::optional<Overlay> overlayOf(const Line &line)
{
    if (line.header || line.key != "dtoverlay" || line.value.empty()) return std::nullopt;
    if (std::any_of(line.value.begin(), line.value.end(), [](char ch) { return ch == ' ' || ch == '\t'; })) return std::nullopt;
    Overlay overlay;
    size_t start = 0;
    while (true) {
        const size_t comma = line.value.find(',', start);
        const std::string token = line.value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (start == 0) overlay.name = token;
        else if (!token.empty()) overlay.params.push_back(token);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (overlay.name.empty()) return std::nullopt;
    return overlay;
}

inline std::string overlayText(const Overlay &overlay)
{
    std::string out = overlay.name;
    for (const auto &param : overlay.params) out += "," + param;
    return out;
}

inline bool hasParam(const Overlay &overlay, std::string_view param)
{
    for (const auto &p : overlay.params)
        if (lower(p) == param) return true;
    return false;
}

inline std::string portOf(const Overlay &overlay) { return hasParam(overlay, "cam0") ? "cam0" : "cam1"; }

inline std::string sensorKeyOf(const Overlay &overlay)
{
    const std::string name = lower(overlay.name);
    if (name == "imx585" && hasParam(overlay, "mono")) return "imx585_mono";
    return name;
}

constexpr std::string_view kLinkToken = "link-frequency=";

inline std::optional<std::string> linkTokenOf(const Overlay &overlay)
{
    for (const auto &p : overlay.params)
        if (lower(p).rfind(kLinkToken, 0) == 0) return p.substr(kLinkToken.size());
    return std::nullopt;
}

struct DtparamItem {
    std::string name;    // as written
    std::string value;   // as written; empty for a bare name (which means on)
    bool bare = false;
};

// dtparam= is a comma list: dtparam=i2c_arm=on,i2c_arm_baudrate=400000. A bare name means on.
inline std::vector<DtparamItem> dtparamItems(const std::string &value)
{
    std::vector<DtparamItem> items;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const std::string item = trimmed(std::string_view(value).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty()) {
            DtparamItem parsed;
            const size_t eq = item.find('=');
            parsed.name = trimmed(std::string_view(item).substr(0, eq));
            parsed.bare = eq == std::string::npos;
            if (!parsed.bare) parsed.value = trimmed(std::string_view(item).substr(eq + 1));
            items.push_back(parsed);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return items;
}

inline std::string dtparamText(const std::vector<DtparamItem> &items)
{
    std::string out;
    for (const auto &item : items) {
        if (!out.empty()) out += ',';
        out += item.name;
        if (!item.bare) out += "=" + item.value;
    }
    return out;
}

// true/false for a dtparam value the firmware understands, nullopt for anything else.
inline std::optional<bool> dtparamBool(const DtparamItem &item)
{
    if (item.bare) return true;
    const std::string v = lower(item.value);
    if (v == "on" || v == "true" || v == "yes" || v == "1" || v == "enable") return true;
    if (v == "off" || v == "false" || v == "no" || v == "0" || v == "disable") return false;
    return std::nullopt;
}

struct Toggle {
    std::string_view key;          // the page's name
    std::string_view param;        // the dtparam name written
    std::string_view alias;        // another spelling the firmware accepts ("" = none)
    std::string_view section;      // "board" or "all": where the Apertar file keeps the line
};

inline const std::vector<Toggle> &toggles()
{
    static const std::vector<Toggle> list = {
        {"i2c", "i2c_arm", "i2c", "board"},
        {"i2s", "i2s", "", "board"},
        {"spi", "spi", "", "board"},
        {"audio", "audio", "", "all"},
    };
    return list;
}

inline bool itemIs(const DtparamItem &item, const Toggle &toggle)
{
    const std::string name = lower(item.name);
    return name == toggle.param || (!toggle.alias.empty() && name == toggle.alias);
}

// ---- the state the card shows -------------------------------------------------------------------

struct CameraPort {
    std::string sensor = "none";             // a key of sensors(), or the overlay name of a sensor it does not list
    std::optional<uint64_t> linkFrequency;   // from the line; nullopt = the overlay's own default
    std::string linkText;                    // the line's link-frequency= value as written ("" = none)
    size_t line = 0;                         // 1-based line that loads it, 0 = none
    std::vector<size_t> extraLines;          // further active sensor lines on this port
};

struct Switch {
    bool on = false;
    size_t line = 0;                          // the line that decides it, 0 = nothing does (the firmware default)
};

struct State {
    Board board;
    bool editable = false;
    std::string problem;                      // why it is not editable
    std::map<std::string, CameraPort> cameras;
    std::map<std::string, Switch> toggles;    // i2c, i2s, spi, audio
    Switch rp1;
    std::string rp1Overlay;
    std::string display = "hdmi";             // a key of displays(), or "custom"
    std::vector<size_t> displayLines;
    std::vector<std::string> dpiOverlays;
    std::optional<int> cameraAutoDetect;
    size_t cameraAutoDetectLine = 0;
    std::map<std::string, std::map<std::string, uint64_t>> candidateLinks;   // port -> sensor -> Hz on a commented line
    std::vector<std::string> warnings;
    // Lines the card's controls or the fan mode may change, commented ones included (line, what: cam0, cam1, i2c, i2s,
    // spi, audio, rp1, display, camera_auto_detect, fan). Every other line is the operator's own.
    std::vector<std::pair<size_t, std::string>> managed;
};

inline bool sectionExists(const Document &doc, const std::string &filter)
{
    return std::any_of(doc.lines.begin(), doc.lines.end(), [&](const Line &l) { return l.header && l.key == filter; });
}

inline State readState(const Document &doc, const Board &board)
{
    State state;
    state.board = board;
    for (const auto &port : {"cam0", "cam1"}) state.cameras[port];
    for (const auto &toggle : toggles()) state.toggles[std::string(toggle.key)];

    std::vector<std::string> dpi;
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        const Line &line = doc.lines[i];
        const size_t number = i + 1;
        if (!line.applies || line.header || line.key.empty()) continue;
        if (line.key == "dtoverlay") {
            const auto overlay = overlayOf(line);
            if (!overlay) continue;
            if (isCameraOverlay(overlay->name)) {
                const std::string port = portOf(*overlay);
                const auto link = linkTokenOf(*overlay);
                const auto hz = link ? parseHz(*link) : std::nullopt;
                if (line.comment) {
                    if (hz) state.candidateLinks[port].emplace(sensorKeyOf(*overlay), *hz);
                    continue;
                }
                CameraPort &camera = state.cameras[port];
                if (camera.line) {
                    camera.extraLines.push_back(number);
                    continue;
                }
                camera.sensor = sensorKeyOf(*overlay);
                camera.line = number;
                camera.linkText = link.value_or("");
                camera.linkFrequency = hz;
                if (link && !hz)
                    state.warnings.push_back("Line " + std::to_string(number) + ": link-frequency=" + *link + " is not a whole number of Hz.");
            } else if (line.comment) {
                continue;
            } else if (isRp1ClockOverlay(overlay->name)) {
                if (!state.rp1.on) state.rp1Overlay = overlay->name;
                state.rp1 = {true, number};
            } else if (isDpiOverlay(overlay->name)) {
                state.displayLines.push_back(number);
                dpi.push_back(overlay->name);
            }
            continue;
        }
        if (line.comment) continue;
        if (line.key == "dtparam") {
            for (const auto &item : dtparamItems(line.value))
                for (const auto &toggle : toggles())
                    if (itemIs(item, toggle))
                        if (const auto on = dtparamBool(item)) state.toggles[std::string(toggle.key)] = {*on, number};
        } else if (line.key == "camera_auto_detect") {
            if (const auto value = parseHz(line.value)) {
                state.cameraAutoDetect = static_cast<int>(*value);
                state.cameraAutoDetectLine = number;
            }
        }
    }
    for (auto &[port, camera] : state.cameras)
        if (!camera.extraLines.empty()) {
            std::string lines = std::to_string(camera.line);
            for (const size_t extra : camera.extraLines) lines += ", " + std::to_string(extra);
            state.warnings.push_back("More than one sensor loads on " + port + " (lines " + lines + "). Saving keeps one.");
        }

    std::set<std::string> active;
    for (const auto &name : dpi) {
        const std::string n = lower(name);
        if (active.insert(n).second) state.dpiOverlays.push_back(name);
    }
    if (active.empty()) {
        state.display = "hdmi";
    } else {
        state.display = "custom";
        for (const auto &display : displays()) {
            std::set<std::string> want;
            for (const auto &overlay : display.overlays) want.insert(lower(overlay));
            if (!want.empty() && want == active) state.display = std::string(display.key);
        }
    }

    for (size_t i = 0; i < doc.lines.size(); ++i) {
        const Line &line = doc.lines[i];
        std::string what;
        if (line.fenced) {
            what = "fan";
        } else if (!line.applies || line.header || line.key.empty()) {
            continue;
        } else if (line.key == "dtoverlay") {
            if (const auto overlay = overlayOf(line)) {
                if (isCameraOverlay(overlay->name)) what = portOf(*overlay);
                else if (isRp1ClockOverlay(overlay->name)) what = "rp1";
                else if (isDpiOverlay(overlay->name)) what = "display";
            }
        } else if (line.key == "dtparam") {
            for (const auto &item : dtparamItems(line.value))
                for (const auto &toggle : toggles())
                    if (what.empty() && itemIs(item, toggle)) what = std::string(toggle.key);
        } else if (line.key == "camera_auto_detect") {
            what = "camera_auto_detect";
        }
        if (!what.empty()) state.managed.emplace_back(i + 1, what);
    }

    const std::string section = boardSection(board);
    if (sectionExists(doc, section) || sectionExists(doc, "all")) {
        state.editable = true;
    } else {
        state.problem = "config.txt has neither a [" + section + "] nor an [all] section, so it is not the Apertar layout. "
                        "Edit it by hand (sudo nano /boot/firmware/config.txt).";
    }
    return state;
}

// ---- a request and its edits --------------------------------------------------------------------

struct Request {
    std::map<std::string, std::string> sensors;                       // port -> sensor key, "none" = empty
    std::map<std::string, std::optional<uint64_t>> linkFrequencies;   // port -> Hz, nullopt = the overlay default
    std::map<std::string, bool> toggles;                              // i2c, i2s, spi, audio
    std::optional<bool> rp1;
    std::optional<std::string> display;
};

struct Facts {
    Board board;
    // lowercase names of the *.dtbo files in the overlays folder; nullopt = unknown (not checked)
    std::optional<std::set<std::string>> overlays;
};

inline bool overlayInstalled(const Facts &facts, std::string_view name)
{
    return !facts.overlays || facts.overlays->count(lower(name)) > 0;
}

// The RP1 clock switch is offered on a Pi 5 that has the overlay (or already loads one).
inline bool rp1Available(const State &state, const Facts &facts)
{
    if (state.board.family != "pi5") return false;
    if (state.rp1.on || !facts.overlays) return true;
    return std::any_of(facts.overlays->begin(), facts.overlays->end(), [](const std::string &n) { return isRp1ClockOverlay(n); });
}

struct Change {
    size_t line = 0;          // 1-based line it replaces; 0 = an added line
    size_t after = 0;         // for an added line: the old line it follows (0 = the top)
    std::string before;
    std::string after_text;
    bool removed = false;     // `line` is deleted (only from lineChanges: the card's own edits never delete)
};

struct ApplyResult {
    bool ok = false;
    std::string error;
    std::string text;
    std::vector<Change> changes;
    State before;
    State after;
    bool displayChanged = false;
};

namespace detail {

inline std::string commentedOut(const std::string &raw)
{
    size_t pos = 0;
    while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) ++pos;
    return raw.substr(0, pos) + "#" + raw.substr(pos);
}

inline std::string uncommented(const std::string &raw)
{
    size_t pos = 0;
    while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) ++pos;
    size_t q = pos;
    while (q < raw.size() && raw[q] == '#') ++q;
    while (q < raw.size() && (raw[q] == ' ' || raw[q] == '\t')) ++q;
    return raw.substr(0, pos) + raw.substr(q);
}

inline std::string withValue(const std::string &raw, const std::string &value)
{
    const Line line = lineFrom(raw);
    return raw.substr(0, line.valueBegin) + value + raw.substr(line.valueEnd);
}

class Editor {
public:
    explicit Editor(const Document &doc) : doc_(doc), replaced_(doc.lines.size()) {}

    std::string current(size_t index) const { return replaced_[index] ? *replaced_[index] : doc_.lines[index].raw; }
    Line currentLine(size_t index) const { return lineFrom(current(index)); }
    void set(size_t index, std::string raw)
    {
        if (!doc_.lines[index].fenced) replaced_[index] = std::move(raw);
    }
    void commentOut(size_t index)
    {
        if (!currentLine(index).comment) set(index, commentedOut(current(index)));
    }
    void uncomment(size_t index) { set(index, uncommented(current(index))); }
    // after = old line index the new one follows, -1 = the very top
    void insertAfter(long after, std::string raw)
    {
        if (doc_.crlf) raw += '\r';
        inserts_[after].push_back(std::move(raw));
    }
    void appendSection(const std::string &header, std::string raw)
    {
        tail_.push_back(doc_.crlf ? header + '\r' : header);
        tail_.push_back(doc_.crlf ? raw + '\r' : std::move(raw));
    }

    std::string build(std::vector<Change> *changes) const
    {
        std::vector<std::string> out;
        auto emitInserts = [&](long after) {
            const auto it = inserts_.find(after);
            if (it == inserts_.end()) return;
            for (const auto &raw : it->second) {
                out.push_back(raw);
                if (changes) changes->push_back({0, static_cast<size_t>(after + 1), {}, raw});
            }
        };
        emitInserts(-1);
        for (size_t i = 0; i < doc_.lines.size(); ++i) {
            const std::string now = current(i);
            out.push_back(now);
            if (changes && now != doc_.lines[i].raw) changes->push_back({i + 1, 0, doc_.lines[i].raw, now});
            emitInserts(static_cast<long>(i));
        }
        if (!tail_.empty()) {
            const size_t last = doc_.lines.size();
            if (!out.empty() && !trimmed(out.back()).empty()) out.push_back("");
            for (const auto &raw : tail_) {
                out.push_back(raw);
                if (changes) changes->push_back({0, last, {}, raw});
            }
        }
        std::string text;
        for (size_t i = 0; i < out.size(); ++i) {
            text += out[i];
            if (i + 1 < out.size() || doc_.finalNewline || !tail_.empty()) text += '\n';
        }
        return text;
    }

private:
    const Document &doc_;
    std::vector<std::optional<std::string>> replaced_;
    std::map<long, std::vector<std::string>> inserts_;
    std::vector<std::string> tail_;
};

// Last line index in sections named `filter` that matches `pick`; -1 if none.
template <typename Pick>
long lastIn(const Document &doc, const std::string &filter, Pick pick)
{
    long found = -1;
    for (size_t i = 0; i < doc.lines.size(); ++i)
        if (doc.lines[i].filter == filter && !doc.lines[i].header && !doc.lines[i].fenced && pick(doc.lines[i])) found = static_cast<long>(i);
    return found;
}

// The last non-blank line of the last `filter` section (its header if it holds nothing); -1 if no such section.
inline long sectionEnd(const Document &doc, const std::string &filter)
{
    long found = -1;
    for (size_t i = 0; i < doc.lines.size(); ++i)
        if (doc.lines[i].filter == filter && !doc.lines[i].fenced && (doc.lines[i].header || !trimmed(doc.lines[i].raw).empty()))
            found = static_cast<long>(i);
    return found;
}

inline std::string withNumber(const std::string &text, size_t line) { return text + " (line " + std::to_string(line) + ")"; }

inline std::string quoteLine(const Document &doc, size_t number)
{
    if (number == 0 || number > doc.lines.size()) return {};
    return "`" + trimmed(doc.lines[number - 1].raw) + "`";
}

} // namespace detail

// Validate a requested link frequency for `sensor` on `port`. `kept` are values config.txt already holds for this
// sensor on this port (the active line, a commented candidate): those stay pickable even when the menu lacks them.
inline std::string linkProblem(const Sensor &sensor, const std::string &port, uint64_t hz, const Board &board,
                               const std::set<uint64_t> &kept)
{
    if (kept.count(hz)) return {};
    for (const auto &option : linkOptionsFor(sensor, board))
        if (option.hz == hz) return {};
    std::string list;
    for (const auto &option : linkOptionsFor(sensor, board)) list += (list.empty() ? "" : ", ") + std::to_string(option.hz);
    return std::to_string(hz) + " Hz is not a " + std::string(sensor.label) + " link frequency on " + port +
           ". Supported: " + list + ".";
}

inline ApplyResult applyRequest(const std::string &text, const Request &request, const Facts &facts)
{
    using namespace detail;
    ApplyResult result;
    const Board &board = facts.board;
    const Document doc = parseDocument(text, board);
    result.before = readState(doc, board);
    if (!result.before.editable) {
        result.error = result.before.problem;
        return result;
    }
    Editor edit(doc);
    const std::string section = sectionExists(doc, boardSection(board)) ? boardSection(board) : std::string("all");
    const std::vector<std::string> ports = boardPorts(board);

    auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    // -- camera ------------------------------------------------------------------------------------
    // where a new sensor line goes: after the last sensor line of the board section, commented or not
    const long cameraAnchor = [&] {
        const long last = lastIn(doc, section, [](const Line &l) {
            const auto o = overlayOf(l);
            return o && isCameraOverlay(o->name);
        });
        return last >= 0 ? last : sectionEnd(doc, section);
    }();

    std::map<std::string, std::optional<std::optional<uint64_t>>> wantLinks;   // port -> (touch?) -> Hz or default
    bool anySensor = false;
    for (const auto &[port, key] : request.sensors) {
        if (std::find(ports.begin(), ports.end(), port) == ports.end())
            return fail("This board has no " + port + " camera connector.");
        const CameraPort &current = result.before.cameras.at(port);
        if (key != "none") anySensor = true;
        const Sensor *sensor = findSensor(key);
        if (key != "none" && !sensor) {
            if (key == current.sensor) continue;   // a sensor this card does not list, kept as it is
            return fail("Unknown sensor \"" + key + "\" for " + port + ".");
        }
        if (sensor && key != current.sensor && !overlayInstalled(facts, sensor->overlay))
            return fail(std::string(sensor->label) + " needs /boot/firmware/overlays/" + std::string(sensor->overlay) +
                        ".dtbo, which is not installed. Install its driver first.");

        std::optional<std::optional<uint64_t>> wantLink;
        if (sensor && sensor->defaultHz) {
            const auto it = request.linkFrequencies.find(port);
            if (it != request.linkFrequencies.end()) {
                std::optional<uint64_t> hz = it->second;
                if (hz) {
                    std::set<uint64_t> kept;
                    if (current.sensor == key && current.linkFrequency) kept.insert(*current.linkFrequency);
                    const auto candidates = result.before.candidateLinks.find(port);
                    if (candidates != result.before.candidateLinks.end()) {
                        const auto candidate = candidates->second.find(key);
                        if (candidate != candidates->second.end()) kept.insert(candidate->second);
                    }
                    if (const std::string problem = linkProblem(*sensor, port, *hz, board, kept); !problem.empty())
                        return fail(problem);
                    if (*hz == sensor->defaultHz) hz.reset();   // the overlay's own value: no number to look up later
                }
                wantLink = hz;
            }
        }
        wantLinks[port] = wantLink;

        // active sensor lines on this port, and the one to keep
        std::vector<size_t> active;
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            const Line &l = doc.lines[i];
            if (!l.applies || l.comment) continue;
            const auto o = overlayOf(l);
            if (o && isCameraOverlay(o->name) && portOf(*o) == port) active.push_back(i);
        }
        long reuse = -1;
        if (sensor) {
            for (const size_t i : active)
                if (lower(overlayOf(doc.lines[i])->name) == sensor->overlay) { reuse = static_cast<long>(i); break; }
            if (reuse < 0) {
                // a commented candidate in the board section, the right mono-ness first
                long loose = -1;
                for (size_t i = 0; i < doc.lines.size(); ++i) {
                    const Line &l = doc.lines[i];
                    if (!l.comment || !l.applies || l.filter != section) continue;
                    const auto o = overlayOf(l);
                    if (!o || lower(o->name) != sensor->overlay || portOf(*o) != port) continue;
                    if (hasParam(*o, "mono") == sensor->mono) { reuse = static_cast<long>(i); break; }
                    if (loose < 0) loose = static_cast<long>(i);
                }
                if (reuse < 0) reuse = loose;
            }
        }
        for (const size_t i : active)
            if (static_cast<long>(i) != reuse) edit.commentOut(i);
        if (!sensor) continue;

        Overlay overlay;
        const bool fresh = reuse < 0;
        if (fresh) {
            overlay.name = std::string(sensor->overlay);
            if (port == "cam0") overlay.params.push_back("cam0");
        } else {
            overlay = *overlayOf(edit.currentLine(static_cast<size_t>(reuse)));
        }
        // mono: after the port word, as the Apertar/Cinemate lines put it
        auto monoAt = std::find_if(overlay.params.begin(), overlay.params.end(), [](const std::string &p) { return lower(p) == "mono"; });
        if (sensor->mono && monoAt == overlay.params.end()) {
            auto portAt = std::find_if(overlay.params.begin(), overlay.params.end(), [](const std::string &p) {
                const std::string l = lower(p);
                return l == "cam0" || l == "cam1";
            });
            overlay.params.insert(portAt == overlay.params.end() ? overlay.params.begin() : portAt + 1, "mono");
        } else if (!sensor->mono && monoAt != overlay.params.end()) {
            overlay.params.erase(monoAt);
        }
        if (wantLink) {
            auto linkAt = std::find_if(overlay.params.begin(), overlay.params.end(), [](const std::string &p) {
                return lower(p).rfind(kLinkToken, 0) == 0;
            });
            if (*wantLink) {
                const std::string token = std::string(kLinkToken) + std::to_string(**wantLink);
                if (linkAt == overlay.params.end()) overlay.params.push_back(token);
                else *linkAt = token;
            } else if (linkAt != overlay.params.end()) {
                overlay.params.erase(linkAt);
            }
        }
        if (fresh) {
            if (cameraAnchor >= 0) edit.insertAfter(cameraAnchor, "dtoverlay=" + overlayText(overlay));
            else edit.appendSection("[all]", "dtoverlay=" + overlayText(overlay));
        } else {
            const size_t index = static_cast<size_t>(reuse);
            std::string raw = withValue(edit.current(index), overlayText(overlay));
            if (lineFrom(raw).comment) raw = uncommented(raw);
            edit.set(index, raw);
        }
    }
    // The Apertar file names its sensors, so the firmware must not probe for one on its own as well.
    if (anySensor && result.before.cameraAutoDetect && *result.before.cameraAutoDetect != 0 && result.before.cameraAutoDetectLine)
        edit.set(result.before.cameraAutoDetectLine - 1, withValue(edit.current(result.before.cameraAutoDetectLine - 1), "0"));

    // -- interfaces --------------------------------------------------------------------------------
    for (const auto &toggle : toggles()) {
        const auto want = request.toggles.find(std::string(toggle.key));
        if (want == request.toggles.end()) continue;
        const Switch now = result.before.toggles.at(std::string(toggle.key));
        if (now.on == want->second) continue;
        const std::string home = toggle.section == "all" || !sectionExists(doc, section) ? std::string("all") : section;
        if (want->second) {
            // the line that switches it off, or a commented "dtparam=<name>=on", or a new line
            if (now.line) {
                const size_t index = now.line - 1;
                auto items = dtparamItems(edit.currentLine(index).value);
                for (auto &item : items)
                    if (itemIs(item, toggle)) { item.bare = false; item.value = "on"; }
                edit.set(index, withValue(edit.current(index), dtparamText(items)));
                continue;
            }
            long candidate = -1;
            for (int pass = 0; pass < 2 && candidate < 0; ++pass)
                for (size_t i = 0; i < doc.lines.size(); ++i) {
                    const Line &l = doc.lines[i];
                    if (!l.comment || !l.applies || l.key != "dtparam" || (pass == 0 && l.filter != home)) continue;
                    const auto items = dtparamItems(l.value);
                    if (items.size() == 1 && itemIs(items[0], toggle) && dtparamBool(items[0]).value_or(false)) {
                        candidate = static_cast<long>(i);
                        break;
                    }
                }
            if (candidate >= 0) {
                edit.uncomment(static_cast<size_t>(candidate));
                continue;
            }
            long anchor = -1;
            for (const auto &other : toggles())
                if (other.section == toggle.section) {
                    const long last = lastIn(doc, home, [&](const Line &l) {
                        if (l.key != "dtparam") return false;
                        const auto items = dtparamItems(l.value);
                        return std::any_of(items.begin(), items.end(), [&](const DtparamItem &item) { return itemIs(item, other); });
                    });
                    anchor = std::max(anchor, last);
                }
            if (anchor < 0) anchor = lastIn(doc, home, [](const Line &l) { return l.key == "dtparam"; });
            if (anchor < 0) anchor = sectionEnd(doc, home);
            const std::string line = "dtparam=" + std::string(toggle.param) + "=on";
            if (anchor >= 0) edit.insertAfter(anchor, line);
            else edit.appendSection("[all]", line);
        } else {
            for (size_t i = 0; i < doc.lines.size(); ++i) {
                const Line &l = doc.lines[i];
                if (!l.applies || l.comment || l.key != "dtparam") continue;
                auto items = dtparamItems(edit.currentLine(i).value);
                bool touches = false;
                for (const auto &item : items) touches = touches || (itemIs(item, toggle) && dtparamBool(item).value_or(false));
                if (!touches) continue;
                if (items.size() == 1) {
                    edit.commentOut(i);   // "uncomment to enable", as the file says
                } else {
                    for (auto &item : items)
                        if (itemIs(item, toggle) && dtparamBool(item).value_or(false)) { item.bare = false; item.value = "off"; }
                    edit.set(i, withValue(edit.current(i), dtparamText(items)));
                }
            }
        }
    }

    // -- RP1 clock ---------------------------------------------------------------------------------
    if (request.rp1 && *request.rp1 != result.before.rp1.on) {
        if (*request.rp1) {
            if (board.family != "pi5") return fail("The RP1 clock overlay is for the Pi 5 family only.");
            long candidate = -1;
            for (int pass = 0; pass < 2 && candidate < 0; ++pass)
                for (size_t i = 0; i < doc.lines.size(); ++i) {
                    const Line &l = doc.lines[i];
                    if (!l.comment || !l.applies || (pass == 0 && l.filter != "all")) continue;
                    const auto o = overlayOf(l);
                    if (o && isRp1ClockOverlay(o->name)) { candidate = static_cast<long>(i); break; }
                }
            const std::string name = candidate >= 0 ? overlayOf(doc.lines[static_cast<size_t>(candidate)])->name : std::string(kRp1ClockOverlay);
            if (!overlayInstalled(facts, name))
                return fail("The RP1 overclock needs /boot/firmware/overlays/" + name + ".dtbo, which is not installed "
                            "(will127534's rp1-overclock-tool installs it).");
            if (candidate >= 0) {
                edit.uncomment(static_cast<size_t>(candidate));
            } else {
                long anchor = lastIn(doc, "all", [](const Line &l) { return !l.comment && l.key == "arm_boost"; });
                if (anchor < 0) anchor = sectionEnd(doc, "all");
                const std::string line = "dtoverlay=" + std::string(kRp1ClockOverlay);
                if (anchor >= 0) edit.insertAfter(anchor, line);
                else edit.appendSection("[all]", line);
            }
        } else {
            for (size_t i = 0; i < doc.lines.size(); ++i) {
                const auto o = overlayOf(doc.lines[i]);
                if (doc.lines[i].applies && !doc.lines[i].comment && o && isRp1ClockOverlay(o->name)) edit.commentOut(i);
            }
        }
    }

    // -- display -----------------------------------------------------------------------------------
    if (request.display && *request.display != "custom" && *request.display != result.before.display) {
        const Display *display = findDisplay(*request.display);
        if (!display) return fail("Unknown display \"" + *request.display + "\".");
        result.displayChanged = true;
        std::set<std::string> want;
        for (const auto &overlay : display->overlays) want.insert(lower(overlay));
        std::set<std::string> kept;
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            const Line &l = doc.lines[i];
            const auto o = overlayOf(l);
            if (!l.applies || l.comment || !o || !isDpiOverlay(o->name)) continue;
            if (want.count(lower(o->name)) && kept.insert(lower(o->name)).second) continue;
            edit.commentOut(i);
        }
        long anchor = -1;
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            const auto o = overlayOf(doc.lines[i]);
            if (doc.lines[i].applies && o && isDpiOverlay(o->name)) anchor = static_cast<long>(i);
        }
        if (anchor < 0) anchor = sectionEnd(doc, "all");
        for (const auto &overlay : display->overlays) {
            const std::string name = lower(overlay);
            if (kept.count(name)) continue;
            if (!overlayInstalled(facts, name))
                return fail(std::string(display->label) + " needs /boot/firmware/overlays/" + std::string(overlay) +
                            ".dtbo, which is not installed.");
            long candidate = -1;
            for (size_t i = 0; i < doc.lines.size() && candidate < 0; ++i) {
                const Line &l = doc.lines[i];
                const auto o = overlayOf(l);
                if (l.comment && l.applies && o && lower(o->name) == name) candidate = static_cast<long>(i);
            }
            if (candidate >= 0) edit.uncomment(static_cast<size_t>(candidate));
            else if (anchor >= 0) edit.insertAfter(anchor, "dtoverlay=" + std::string(overlay));
            else edit.appendSection("[all]", "dtoverlay=" + std::string(overlay));
            kept.insert(name);
        }
    }

    // -- read it back ------------------------------------------------------------------------------
    result.text = edit.build(&result.changes);
    const Document after = parseDocument(result.text, board);
    result.after = readState(after, board);
    const State &a = result.after;

    for (const auto &[port, key] : request.sensors) {
        const CameraPort &now = a.cameras.at(port);
        if (findSensor(key) || key == "none") {
            if (now.sensor != key)
                return fail(port + " would load " + (now.sensor == "none" ? std::string("no sensor") : now.sensor) +
                            ", not " + key + ": " + withNumber(quoteLine(after, now.line), now.line) + " decides it.");
            if (!now.extraLines.empty())
                return fail(port + " would load two sensors: " + withNumber(quoteLine(after, now.extraLines.front()), now.extraLines.front()) + ".");
        }
        const auto want = wantLinks.find(port);
        if (want != wantLinks.end() && want->second && now.linkFrequency != *want->second)
            return fail(port + " link frequency did not take: " + withNumber(quoteLine(after, now.line), now.line) + ".");
    }
    for (const auto &[key, on] : request.toggles) {
        const Switch &now = a.toggles.at(key);
        if (now.on != on)
            return fail(withNumber(quoteLine(after, now.line), now.line) + " still turns " + key + (now.on ? " on" : " off") +
                        ". Apertar does not edit that line; change it by hand.");
    }
    if (request.rp1 && a.rp1.on != *request.rp1)
        return fail(withNumber(quoteLine(after, a.rp1.line), a.rp1.line) + " still loads the RP1 clock overlay. Change it by hand.");
    if (request.display && *request.display != "custom" && a.display != *request.display)
        return fail("The DPI overlays would not match " + *request.display + " (now: " + a.display + ").");

    result.ok = true;
    return result;
}

// ---- the file edited as text (the card's Edit file) ---------------------------------------------
//
// The operator may edit any line by hand; the card's choices are then applied on top of that text by applyRequest,
// so the read-back checks above still hold. What is reviewed and written is that result against the file on disk.

// Why `text` cannot be written as config.txt; empty when it can. The root helper refuses the same.
inline std::string textProblem(const std::string &text)
{
    if (trimmed(text).empty()) return "config.txt cannot be empty.";
    if (text.size() > 65536) return "config.txt is at most 64 KiB.";
    if (text.find('\0') != std::string::npos) return "config.txt is text: it has a NUL byte.";
    return {};
}

namespace detail {

inline std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace detail

// What turns `from` into `to`, line by line (longest common subsequence after the common head and tail): a changed
// line, a removed line, or an added line after an old one, numbered as in `from`. A middle too long to compare is
// shown as all of it removed and added.
inline std::vector<Change> lineChanges(const std::string &from, const std::string &to)
{
    const std::vector<std::string> a = detail::splitLines(from);
    const std::vector<std::string> b = detail::splitLines(to);
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    const size_t n = a.size() - head - tail;
    const size_t m = b.size() - head - tail;

    // keep[i][j]: length of the common subsequence of a[head+i..] and b[head+j..]
    std::vector<std::vector<uint32_t>> keep;
    const bool compare = n * m <= 4000000;
    if (compare) {
        keep.assign(n + 1, std::vector<uint32_t>(m + 1, 0));
        for (size_t i = n; i-- > 0;)
            for (size_t j = m; j-- > 0;)
                keep[i][j] = a[head + i] == b[head + j] ? keep[i + 1][j + 1] + 1 : std::max(keep[i + 1][j], keep[i][j + 1]);
    }

    std::vector<Change> changes;
    std::vector<size_t> dels, adds;   // the current run between two kept lines (indexes into a and b)
    size_t before = head;             // 1-based number of the old line the run follows (0 = the top)
    auto flush = [&]() {
        const size_t paired = std::min(dels.size(), adds.size());
        for (size_t k = 0; k < paired; ++k) changes.push_back({dels[k] + 1, 0, a[dels[k]], b[adds[k]]});
        for (size_t k = paired; k < dels.size(); ++k) changes.push_back({dels[k] + 1, 0, a[dels[k]], {}, true});
        const size_t after = dels.empty() ? before : dels.back() + 1;
        for (size_t k = paired; k < adds.size(); ++k) changes.push_back({0, after, {}, b[adds[k]]});
        dels.clear();
        adds.clear();
    };
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (compare && i < n && j < m && a[head + i] == b[head + j]) {
            flush();
            ++i;
            ++j;
            before = head + i;
        } else if (i < n && (j == m || !compare || keep[i + 1][j] >= keep[i][j + 1])) {
            dels.push_back(head + i++);
        } else {
            adds.push_back(head + j++);
        }
    }
    flush();
    return changes;
}

// The display overlays differ between two readings (what needs the confirm_display tick).
inline bool displayDiffers(const State &a, const State &b)
{
    std::vector<std::string> x = a.dpiOverlays, y = b.dpiOverlays;
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    return a.display != b.display || x != y;
}

// ---- JSON for the page --------------------------------------------------------------------------

inline std::string jsonString(std::string_view text)
{
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            } else {
                out += ch;
            }
        }
    }
    return out + "\"";
}

inline std::string jsonHz(const std::optional<uint64_t> &hz) { return hz ? std::to_string(*hz) : "null"; }

inline std::string stateJson(const State &state, const Facts &facts)
{
    std::string out = "{";
    out += "\"board\":{\"family\":" + jsonString(state.board.family) + ",\"model\":" + jsonString(state.board.model) +
           ",\"detected\":" + (state.board.detected ? "true" : "false") + ",\"section\":" + jsonString(boardSection(state.board)) +
           ",\"ports\":[";
    const auto ports = boardPorts(state.board);
    for (size_t i = 0; i < ports.size(); ++i) out += (i ? "," : "") + jsonString(ports[i]);
    out += "]}";
    out += ",\"editable\":" + std::string(state.editable ? "true" : "false") + ",\"problem\":" + jsonString(state.problem);
    out += ",\"cameras\":{";
    bool first = true;
    for (const auto &[port, camera] : state.cameras) {
        out += (first ? "" : ",") + jsonString(port) + ":{\"sensor\":" + jsonString(camera.sensor) +
               ",\"link_frequency\":" + jsonHz(camera.linkFrequency) + ",\"link_text\":" + jsonString(camera.linkText) +
               ",\"line\":" + std::to_string(camera.line) + "}";
        first = false;
    }
    out += "},\"toggles\":{";
    first = true;
    for (const auto &[key, toggle] : state.toggles) {
        out += (first ? "" : ",") + jsonString(key) + ":{\"on\":" + (toggle.on ? "true" : "false") + ",\"line\":" + std::to_string(toggle.line) + "}";
        first = false;
    }
    out += "},\"rp1\":{\"on\":" + std::string(state.rp1.on ? "true" : "false") + ",\"line\":" + std::to_string(state.rp1.line) +
           ",\"overlay\":" + jsonString(state.rp1Overlay.empty() ? std::string(kRp1ClockOverlay) : state.rp1Overlay) +
           ",\"available\":" + (rp1Available(state, facts) ? "true" : "false") + "}";
    out += ",\"display\":{\"value\":" + jsonString(state.display) + ",\"overlays\":[";
    for (size_t i = 0; i < state.dpiOverlays.size(); ++i) out += (i ? "," : "") + jsonString(state.dpiOverlays[i]);
    out += "],\"lines\":[";
    for (size_t i = 0; i < state.displayLines.size(); ++i) out += (i ? "," : "") + std::to_string(state.displayLines[i]);
    out += "]}";
    out += ",\"camera_auto_detect\":" + (state.cameraAutoDetect ? std::to_string(*state.cameraAutoDetect) : std::string("null"));
    out += ",\"candidates\":{";
    first = true;
    for (const auto &[port, bySensor] : state.candidateLinks) {
        out += (first ? "" : ",") + jsonString(port) + ":{";
        bool inner = true;
        for (const auto &[sensor, hz] : bySensor) {
            out += (inner ? "" : ",") + jsonString(sensor) + ":" + std::to_string(hz);
            inner = false;
        }
        out += "}";
        first = false;
    }
    out += "},\"sensors\":[";
    for (size_t i = 0; i < sensors().size(); ++i) {
        const Sensor &sensor = sensors()[i];
        out += std::string(i ? "," : "") + "{\"key\":" + jsonString(sensor.key) + ",\"label\":" + jsonString(sensor.label) +
               ",\"overlay\":" + jsonString(sensor.overlay) + ",\"installed\":" +
               (facts.overlays ? (overlayInstalled(facts, sensor.overlay) ? "true" : "false") : "null") + ",\"link\":";
        if (!sensor.defaultHz) {
            out += "null}";
            continue;
        }
        out += "{\"default_hz\":" + std::to_string(sensor.defaultHz) + ",\"options\":[";
        const auto options = linkOptionsFor(sensor, state.board);
        for (size_t j = 0; j < options.size(); ++j)
            out += std::string(j ? "," : "") + "{\"hz\":" + std::to_string(options[j].hz) + ",\"label\":" +
                   jsonString(linkOptionLabel(options[j], sensor.defaultHz)) + ",\"over_rp1_spec\":" +
                   (options[j].overRp1Spec ? "true" : "false") + "}";
        out += "]}}";
    }
    out += "],\"displays\":[";
    for (size_t i = 0; i < displays().size(); ++i) {
        const Display &display = displays()[i];
        bool installed = true;
        for (const auto &overlay : display.overlays) installed = installed && overlayInstalled(facts, overlay);
        out += std::string(i ? "," : "") + "{\"key\":" + jsonString(display.key) + ",\"label\":" + jsonString(display.label) +
               ",\"installed\":" + (installed ? "true" : "false") + "}";
    }
    out += "],\"warnings\":[";
    for (size_t i = 0; i < state.warnings.size(); ++i) out += (i ? "," : "") + jsonString(state.warnings[i]);
    out += "],\"managed\":[";
    for (size_t i = 0; i < state.managed.size(); ++i)
        out += std::string(i ? "," : "") + "{\"line\":" + std::to_string(state.managed[i].first) + ",\"what\":" + jsonString(state.managed[i].second) + "}";
    out += "]}";
    return out;
}

inline std::string changesJson(const std::vector<Change> &changes)
{
    std::string out = "[";
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change &c = changes[i];
        out += std::string(i ? "," : "") + "{\"line\":" + std::to_string(c.line) + ",\"after\":" + std::to_string(c.after) +
               ",\"before\":" + jsonString(trimmed(c.before)) + ",\"now\":" + jsonString(trimmed(c.after_text)) +
               (c.removed ? ",\"removed\":true}" : "}");
    }
    return out + "]";
}

// A file's revision for the X-Apertar-Revision check (FNV-1a 64).
inline std::string revisionOf(const std::string &text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "b%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

} // namespace apertar::bootconf
