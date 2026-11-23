// The known fps limits: the fps a mode holds for a take of a stated length, per storage type, Pi board,
// resolution, bit depth and Clear HDR, with the label, the take length and the source of every number. A measured
// number is the highest fps held clean at the longest take length run; a number not yet measured is for 30 minutes. Pure, std only (no
// libcamera, no MediaProfile); host-tested by tools/sustained_fps_test.cpp. To update the analysis, edit the
// rows in kCells and kPixelRates and nothing else; the test pins the numbers, so a change is deliberate.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace apertar::sustained {

enum class Kind { Nvme, UsbSsd };
enum class Board { Pi5, Other };                                      // Pi 5 and CM5 share the SoC; a CM5 Lite is measured
enum class Label { Measured, Derived, Probable, Unconfirmed };       // strongest first
enum class Group { BackEnd, Encoder, Drive, Sensor };                // which wall limits the mode

// One measured or derived number. A mode that has no cell of its own takes the nearest larger one (limitFor).
struct Cell {
    Kind storage;
    Board board;
    unsigned width;             // the picture's size as the mode pool names it
    unsigned height;
    bool hdr;
    unsigned bits;
    bool log;                   // the row that replaces the Clear HDR 16-bit one while CineMate Log is on
    const char *compression;    // "none" or "lossless_jpeg"
    double fps30;               // held for a take of `minutes`, split on, reference levers
    unsigned minutes;           // measured: the longest take length held clean at fps30; otherwise 30 (the aim)
    double fpsShort;            // held for takes up to about 8 minutes, measured; 0 = none known (information only)
    Label label;                // of fps30
    Group group;
    const char *source;
};

// IMX585, 16:9, cDNG, Raspberry Pi 5 / CM5. Reference levers: TDN off, phase lock off, audio on, WebUI client
// connected, aids on, split on, exFAT, drive warm. The numbers are from the owner's take campaigns on a CM5 Lite
// (November 2026, NVMe estimator set EST-05fb8f1-a: 15-minute takes P6/P6b, 30-minute and limit-search takes P7/P8);
// each row says what was run. The NVMe column is for a hot drive (a take that starts warm from the one before).
inline constexpr Cell kCells[] = {
    // Corsair MP600 Mini on the CFexpress hat: Clear HDR uncompressed, SDR lossless JPEG.
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 16, false, "none", 23, 15, 25, Label::Measured, Group::BackEnd,
      "23 fps held 15 minutes (343 GB); 24 dropped a frame at 712 s; 15-minute takes at 25-27 stopped after 7-13 minutes, "
      "when the warm drive paused writes at 414-447 MB/s (25-27 held 3 and 5 minutes)" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, true, 16, false, "none", 30, 1, 0, Label::Measured, Group::Sensor,
      "60 s clean at 30 fps (the mode's top), 125 MB/s; longer takes not run" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 12, false, "none", 24, 15, 0, Label::Measured, Group::BackEnd,
      "24 fps held 15 minutes (269 GB); 26, 28 and 30 dropped in the first 5 s (capture gaps); 25 not run" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 12, false, "lossless_jpeg", 40, 5, 0, Label::Measured, Group::Encoder,
      "lossless 40 fps on a hot drive (49 C at the start) filled the buffer at 300 s; from a cool drive (37 C) 40 and 39 "
      "held 15 minutes, about 250 MB/s" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, false, 12, false, "lossless_jpeg", 60, 1, 0, Label::Measured, Group::Sensor,
      "60 s clean at 60 fps (the mode's top); longer takes not run at the reference levers" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 10, false, "lossless_jpeg", 44, 2, 0, Label::Measured, Group::Encoder,
      "lossless 45 held a 60-s probe; 15-minute takes at 45 and 44 filled the buffer at 66 s and 179 s" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, false, 10, false, "lossless_jpeg", 60, 30, 0, Label::Probable, Group::Sensor,
      "follows the 2K 12-bit row" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 16, true, "none", 25, 30, 0, Label::Measured, Group::BackEnd,
      "25 fps held 30 minutes (560 GB, drive 37 -> 71 C) and 15 minutes from a hot start (47 C), 311 MB/s; above 25 the "
      "back end drops" },
    // SDR with Log: the PiSP-compressed stream is decoded on the copy workers, too slow at 4K: not used.
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 12, true, "lossless_jpeg", 0, 0, 0, Label::Measured, Group::BackEnd,
      "lossless and uncompressed + Log dropped at every fps tried, 30 to 40 (copy slots full); nothing lower run" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 10, true, "lossless_jpeg", 0, 0, 0, Label::Derived, Group::BackEnd,
      "the same decode path as 4K 12-bit + Log, which dropped at every fps from 30 to 40" },
    { Kind::Nvme, Board::Pi5, 2880, 2160, false, 12, false, "lossless_jpeg", 60, 1, 0, Label::Measured, Group::Encoder,
      "60 s clean at 60 fps; longer takes not run" },
    // Samsung T7 over USB, about 100-105 MB/s in takes: Clear HDR lossless, where it holds more.
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 16, false, "lossless_jpeg", 9, 30, 0, Label::Derived, Group::Drive,
      "about 10 MB per frame at 105 MB/s" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, true, 16, false, "lossless_jpeg", 27, 30, 0, Label::Measured, Group::Drive,
      "lossless 27 fps passed 30 minutes (older build)" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 12, false, "lossless_jpeg", 12, 30, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, false, 12, false, "lossless_jpeg", 18, 30, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed; 4K never run on this drive" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, false, 12, false, "lossless_jpeg", 48, 30, 0, Label::Measured, Group::Drive,
      "lossless 48 fps passed 30 minutes" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, false, 10, false, "lossless_jpeg", 20, 30, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, false, 10, false, "lossless_jpeg", 48, 30, 0, Label::Probable, Group::Drive,
      "follows the 2K 12-bit row" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 16, true, "lossless_jpeg", 8, 30, 0, Label::Derived, Group::Drive,
      "the Clear HDR 16-bit row less the Log gain" },
    { Kind::UsbSsd, Board::Pi5, 2880, 2160, false, 12, false, "lossless_jpeg", 24, 30, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed" },
};

// SDR 12-bit lossless also has a pixel rate, so a crop without a cell gets a number: the rate over its pixels.
struct PixelRate {
    Kind storage;
    Board board;
    bool hdr;
    unsigned bits;
    double mpxPerSecond;
    Label label;
    Group group;
    const char *source;
};

inline constexpr PixelRate kPixelRates[] = {
    { Kind::Nvme, Board::Pi5, false, 12, 330.0, Label::Derived, Group::Encoder,
      "4K lossless at 40 fps for 15 minutes (measured); 2880x2160 held 60 fps for 60 s" },
    { Kind::UsbSsd, Board::Pi5, false, 12, 149.0, Label::Derived, Group::Drive,
      "4K at 18 fps and 2880x2160 at 24 fps" },
};

// A mode as the pool names it. binned: a 2x2 binned readout (the picture is not cut from the full sensor).
struct Mode {
    unsigned width = 0;
    unsigned height = 0;
    bool hdr = false;
    unsigned bits = 12;
    bool binned = false;
};

// What the table needs to know besides the mode. The camera owns tdn, lock and log; the application owns the rest
// and pushes it. The defaults are the reference levers of the table, so a default Context shifts nothing.
struct Context {
    Kind storage = Kind::Nvme;
    bool storageMeasured = true;    // false: the type has no column, the USB SSD's stands in, labelled unconfirmed
    Board board = Board::Pi5;
    bool tdn = false;               // preview temporal denoise
    bool lock = false;              // frame-rate phase lock
    bool log = false;               // CineMate Log
    bool aidsOn = true;             // focus peaking or zebra
    bool audioOn = true;
    bool splitOn = true;            // the clip folder is split into parts (automatic counts as on: it splits where it matters)
    bool splitAuto = false;         // the split is automatic; the table does not read it, the line does
    bool fatLike = false;           // exFAT or FAT: a flat folder slows file creation
    double fillFraction = 0.0;      // how full the medium is, 0 to 1
    double speedMBps = 0.0;         // the last write speed test of this medium, 0 = none (run_media_speed_test)

    bool operator==(const Context &) const = default;
};

inline const char *labelName(Label label)
{
    switch (label) {
    case Label::Measured: return "measured";
    case Label::Derived: return "derived";
    case Label::Probable: return "probable";
    case Label::Unconfirmed: return "unconfirmed";
    }
    return "unconfirmed";
}

inline const char *groupName(Group group)
{
    switch (group) {
    case Group::BackEnd: return "back end";
    case Group::Encoder: return "encoder";
    case Group::Drive: return "drive";
    case Group::Sensor: return "sensor";
    }
    return "sensor";
}

inline Label weaker(Label a, Label b) { return static_cast<int>(a) > static_cast<int>(b) ? a : b; }

// The steps a requested fps can take: 25, 30, 33, 50, 60, 70, 80 ... (10 apart above 60). A limit rounds down to
// one; below 25 it is the number itself, floored (24 is a cinema rate and must stay usable).
inline double stepDown(double fps)
{
    const double f = fps + 1e-9;
    if (f < 25.0) return f > 0.0 ? std::floor(f) : 0.0;
    if (f < 30.0) return 25.0;
    if (f < 33.0) return 30.0;
    if (f < 50.0) return 33.0;
    if (f < 60.0) return 50.0;
    return std::floor(f / 10.0) * 10.0;
}

// A folder that holds all the files of a take slows file creation by 0.32 us per file, so it holds while
// t < 1.9e6 / fps^2 seconds (derived, measured on a T7 and an NVMe probe: 50 minutes at 25 fps, 35 at 30, 14 at 48).
// The highest fps whose flat folder lasts a take of that many seconds.
inline double flatFolderMaxFps(double takeSeconds = 1800.0) { return std::sqrt(1.9e6 / takeSeconds); }

// The row for a mode: its own cell, else the nearest larger cell of the same class (fewer pixels never hold fewer
// fps), else a pixel rate where the class has one; none when the class has no row at all. With Log on, a mode takes
// the Log cell of its own size where there is one. The compression is the cell's: Dynamic resolution owns it.
struct Limit {
    bool found = false;             // false: no row for this class, only the sensor's ceiling applies
    double fps30 = 0.0;             // before the lever shifts
    unsigned minutes = 30;          // the take length fps30 is for
    double fpsShort = 0.0;
    Label label = Label::Unconfirmed;
    Group group = Group::Sensor;
    const char *compression = "none";
    const char *source = "";
    const Cell *cell = nullptr;     // the cell the number comes from; null for a pixel rate
    bool exact = false;             // the cell is the mode's own
    bool pixelRate = false;
};

inline Limit limitFor(const Context &context, const Mode &mode)
{
    Limit out;
    if (context.board != Board::Pi5)
        return out;
    const double pixels = double(mode.width) * double(mode.height);
    const auto sameClass = [&](const Cell &c) {
        return c.storage == context.storage && c.board == context.board && c.hdr == mode.hdr && c.bits == mode.bits;
    };
    const Cell *exact = nullptr, *larger = nullptr;
    if (context.log)
        for (const Cell &c : kCells)
            if (sameClass(c) && c.log && c.width == mode.width && c.height == mode.height)
                exact = &c;
    if (!exact)
        for (const Cell &c : kCells) {
            if (!sameClass(c) || c.log)
                continue;
            if (c.width == mode.width && c.height == mode.height)
                exact = &c;
            else if (double(c.width) * double(c.height) >= pixels &&
                     (!larger || double(c.width) * double(c.height) < double(larger->width) * double(larger->height)))
                larger = &c;
        }
    if (exact) {
        out = { true, exact->fps30, exact->minutes, exact->fpsShort, exact->label, exact->group, exact->compression, exact->source,
                exact, true, false };
    } else {
        for (const PixelRate &rate : kPixelRates) {
            if (rate.storage != context.storage || rate.board != context.board || rate.hdr != mode.hdr ||
                rate.bits != mode.bits || pixels <= 0.0)
                continue;
            const double fps = std::max(rate.mpxPerSecond * 1e6 / pixels, larger ? larger->fps30 : 0.0);
            out = { true, fps, larger ? larger->minutes : 30u, 0.0, weaker(rate.label, larger ? larger->label : rate.label), rate.group,
                    "lossless_jpeg", rate.source, larger, false, true };
            break;
        }
        if (!out.found && larger)
            out = { true, larger->fps30, larger->minutes, larger->fpsShort, weaker(larger->label, Label::Derived), larger->group,
                    larger->compression, larger->source, larger, false, false };
    }
    if (out.found && !context.storageMeasured)
        out.label = Label::Unconfirmed;
    return out;
}

// The USB SSD column's drive: about 105 MB/s in takes (a Samsung T7, the drive the numbers come from).
inline constexpr unsigned kUsbSsdMegabytesPerSecond = 105;

// A write speed test is cold and sequential; a take runs about 0.75 of it once the drive is warm (465 over 620 MB/s
// on the NVMe card, derived).
inline constexpr double kSpeedTestWarmFactor = 0.75;

// What the measured drive holds for a drive-limited cell: the cell's frame is the T7's speed over the cell's fps,
// and it must fit the warm share of the measured speed. 0 = no guard (no test, no row, or a limit that is not the drive's).
inline double driveGuardFps(const Context &context, const Limit &row)
{
    if (context.speedMBps <= 0.0 || !row.found || row.group != Group::Drive || row.fps30 <= 0.0)
        return 0.0;
    return kSpeedTestWarmFactor * context.speedMBps * row.fps30 / kUsbSsdMegabytesPerSecond;
}

// What a mode serves after the levers: cap is the step its limit rounds down to (0 = the mode is not used).
// limit is the number before rounding, 0 when the table has no row. by names the wall: back end, encoder, drive,
// sensor (the sensor's own ceiling is the lower one, or there is no row) or TDN.
struct Cap {
    double fps = 0.0;
    double limit = 0.0;
    Limit row;
    const char *by = "sensor";
    const char *lever = "";         // the lever that moved the number: TDN, phase lock, aids, split or speed test
};

// The levers (table B). TDN on with an unbinned mode: not used (0 of 24 4K takes were clean). Phase lock and aids
// off cost a back-end mode 1 fps each. Split off on exFAT or FAT caps a per-frame format at the flat-folder limit.
// Audio and the WebUI client change nothing here: the table is for audio on and a client connected.
inline Cap capFor(const Context &context, const Mode &mode, double sensorCeiling)
{
    Cap cap;
    cap.row = limitFor(context, mode);
    double fps = sensorCeiling;
    if (cap.row.found) {
        double limit = cap.row.fps30;
        if (cap.row.group == Group::BackEnd) {
            if (context.lock) {
                limit -= 1.0;
                cap.lever = "phase lock";
            }
            if (!context.aidsOn) {
                limit -= 1.0;
                cap.lever = "aids";
            }
        }
        if (const double guard = driveGuardFps(context, cap.row); guard > 0.0 && guard < limit) {
            limit = guard;
            cap.lever = "speed test";
        }
        cap.limit = std::max(limit, 0.0);
        if (cap.limit < fps) {
            fps = cap.limit;
            cap.by = groupName(cap.row.group);
        }
    }
    if (!context.splitOn && context.fatLike && fps > flatFolderMaxFps()) {
        fps = flatFolderMaxFps();
        cap.lever = "split";
    }
    if (context.tdn && !mode.binned) {
        fps = 0.0;
        cap.by = "TDN";
        cap.lever = "TDN";
    }
    cap.fps = stepDown(fps);
    return cap;
}

// A card above 94 % full stalled at 96-97 % in 4K Clear HDR 16-bit on the NVMe: a warning, never a limit.
inline bool cardNearlyFull(const Context &context)
{
    return context.storage == Kind::Nvme && context.fillFraction > 0.94;
}

// The words the screens use for a mode and a drive: "4K Clear HDR 16-bit", "2K SDR 12-bit", "2880x2160 SDR 12-bit".
inline std::string modeName(const Mode &mode)
{
    const std::string size = mode.width == 3840 && mode.height == 2160 ? "4K"
        : mode.width == 1920 && mode.height == 1080 ? "2K"
        : std::to_string(mode.width) + "x" + std::to_string(mode.height);
    return size + (mode.hdr ? " Clear HDR " : " SDR ") + std::to_string(mode.bits) + "-bit";
}

inline const char *storageName(Kind storage) { return storage == Kind::Nvme ? "NVMe card" : "USB SSD"; }

// fps is shown whole: the steps and the table's numbers are whole.
inline std::string wholeFps(double fps) { return std::to_string(static_cast<long>(std::floor(fps + 1e-9))); }

// What a sentence about a limit ends with: an unmeasured storage type, a nearly full card.
inline std::string contextNotes(const Context &context)
{
    std::string text;
    if (!context.storageMeasured)
        text += " Storage type not measured: limits are the USB SSD's. Run the speed test.";
    if (cardNearlyFull(context))
        text += " The card is over 94 percent full: it stalled near 96 percent in 4K Clear HDR 16-bit takes.";
    return text;
}

// The sentence that names a write speed test when it is what lowered a limit.
inline std::string speedTestNote(const Context &context, const Cap &cap)
{
    return cap.lever == std::string("speed test")
        ? " The write speed test measured " + wholeFps(context.speedMBps) + " MB/s on this card." : std::string();
}

// The one sentence under the fps picker for the running mode (limit_why): what it holds, then what the levers
// and the storage change.
inline std::string whyText(const Context &context, const Mode &mode, const Cap &cap, double sensorCeiling)
{
    const std::string name = modeName(mode) + (cap.row.cell && cap.row.cell->log ? " + Log" : "");
    if (context.board != Board::Pi5)
        return "Limits are not measured on this Raspberry Pi.";
    std::string text;
    if (cap.by == std::string("TDN")) {
        text = "Preview temporal denoise is on: " + name + " drops frames at every rate tried (from 23 fps). "
               "Turn it off to use it.";
    } else if (!cap.row.found) {
        text = "No measurement for " + name + ": the sensor's own limit of " + wholeFps(sensorCeiling) + " fps applies.";
    } else if (cap.row.fps30 <= 0.0) {
        text = name + " drops frames at every fps tried on this " + storageName(context.storage) + " (" +
               labelName(cap.row.label) + ").";
    } else {
        text = name + " holds " + wholeFps(cap.fps) + " fps for " + std::to_string(cap.row.minutes) + "-minute takes on this " + storageName(context.storage) +
               " (" + labelName(cap.row.label) + ").";
        if (cap.row.fpsShort > cap.limit)
            text += " " + wholeFps(cap.row.fpsShort) + " fps holds up to 8 minutes (measured).";
        if (context.lock && cap.row.group == Group::BackEnd) {
            Context free = context;
            free.lock = false;
            const Cap without = capFor(free, mode, sensorCeiling);
            if (without.fps > cap.fps)
                text += " Phase lock is on: " + name + " holds " + wholeFps(cap.fps) + " fps instead of " +
                        wholeFps(without.fps) + ".";
        }
        if (!context.splitOn && context.fatLike && cap.lever == std::string("split"))
            text += " Split long takes is off: takes over 30 minutes slow down above 32 fps. Turn it on.";
        text += speedTestNote(context, cap);
    }
    return text + contextNotes(context);
}

// The sentence for a mode the table chose below a better one: `best` is the mode ranked before the chosen one that
// came closest to the fps (the highest cap, the first of equals), bestCap what it holds. Says what held it back.
inline std::string steppedDownText(const Context &context, double fps, const Mode &chosen, const Mode &best,
                                   const Cap &bestCap)
{
    const std::string at = "At " + wholeFps(fps) + " fps the camera records " + modeName(chosen);
    const std::string name = modeName(best) + (bestCap.row.cell && bestCap.row.cell->log ? " + Log" : "");
    const std::string holds = name + " holds " + wholeFps(bestCap.fps) + " fps";
    std::string text;
    if (context.board != Board::Pi5)
        return "Limits are not measured on this Raspberry Pi.";
    if (bestCap.by == std::string("TDN"))
        text = "Preview temporal denoise is on: " + name + " drops frames at every rate tried (from 23 fps). " + at +
               ". Turn it off to use " + name + ".";
    else if (!bestCap.row.found)
        text = at + ": " + name + " is limited to " + wholeFps(bestCap.fps) + " fps by the sensor.";
    else if (bestCap.row.fps30 <= 0.0)
        text = at + ": " + name + " drops frames at every fps tried (" + labelName(bestCap.row.label) + ").";
    else if (bestCap.row.group == Group::Drive && context.storage == Kind::UsbSsd && context.storageMeasured)
        text = "This USB SSD writes about " + (context.speedMBps > 0.0 ? wholeFps(context.speedMBps) : std::to_string(kUsbSsdMegabytesPerSecond)) +
               " MB/s: " + holds + " (" + labelName(bestCap.row.label) + "). " + at + ".";
    else if (best.hdr && !chosen.hdr)
        text = at + ": " + holds + " (" + labelName(bestCap.row.label) + ").";
    else
        text = at + ": " + holds + " for " + std::to_string(bestCap.row.minutes) + " minutes on this " + storageName(context.storage) + " (" +
               labelName(bestCap.row.label) + ").";
    const bool drive = context.storageMeasured && context.storage == Kind::UsbSsd && bestCap.row.group == Group::Drive;
    return text + (drive ? std::string() : speedTestNote(context, bestCap)) + contextNotes(context);
}

// The sentence when the fps is the most any enabled mode holds: nothing better was left to try.
inline std::string highestText(const Context &context, double fps, const Mode &chosen)
{
    if (context.board != Board::Pi5)
        return "Limits are not measured on this Raspberry Pi.";
    return wholeFps(fps) + " fps is the most the enabled resolutions hold here (" + modeName(chosen) + ")." +
           contextNotes(context);
}

// A step that no fps reaches: an earlier mode of the order already holds as much.
inline std::string neverUsedText(const Mode &earlier, const Cap &earlierCap)
{
    return " Never used here: " + modeName(earlier) + " already holds " + wholeFps(earlierCap.fps) + " fps.";
}

} // namespace apertar::sustained
