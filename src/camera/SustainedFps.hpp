// The known fps limits: the fps a mode holds for a 30-minute take on a hot drive, per storage type, Pi board,
// resolution, bit depth and Clear HDR, with the label and the source of every number. Pure, std only (no
// libcamera, no MediaProfile); host-tested by tools/sustained_fps_test.cpp. To update the analysis, edit the
// rows in kCells and kPixelRates and nothing else; the test pins the numbers, so a change is deliberate.
#pragma once

#include <algorithm>
#include <cmath>

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
    double fps30;               // held for a 30-minute take, split on, reference levers
    double fpsShort;            // held for takes up to about 8 minutes, measured; 0 = none known (information only)
    Label label;                // of fps30
    Group group;
    const char *source;
};

// IMX585, 16:9, cDNG, Raspberry Pi 5 / CM5. Reference levers: TDN off, phase lock off, audio on, WebUI client
// connected, aids on, split on, exFAT, drive hot. The numbers are from the owner's take campaigns on a CM5 Lite
// (November 2026); each row says what was run.
inline constexpr Cell kCells[] = {
    // Corsair MP600 Mini on the CFexpress hat: Clear HDR uncompressed, SDR lossless JPEG.
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 16, false, "none", 20, 27, Label::Unconfirmed, Group::BackEnd,
      "25, 26 and 27 fps failed 15-minute takes after 190-316 GB (warm drive, 2026-11-21); 20 is a placeholder" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, true, 16, false, "none", 30, 0, Label::Probable, Group::Sensor,
      "60 s clean at 30 fps, 125 MB/s, drive up to 64 C; 30 minutes not run" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 12, false, "none", 20, 0, Label::Unconfirmed, Group::BackEnd,
      "gaps at every fps from 25 to 30; 22 and 23 not run" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 12, false, "lossless_jpeg", 40, 0, Label::Derived, Group::Encoder,
      "40-42 fps at 60 s; uncompressed takes held 37 fps for 15 minutes and failed at 38; lossless takes not run" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, false, 12, false, "lossless_jpeg", 60, 0, Label::Probable, Group::Sensor,
      "60 s and 5 minutes clean at 60 fps" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, false, 10, false, "lossless_jpeg", 45, 0, Label::Derived, Group::Encoder,
      "45-47 fps at 60 s" },
    { Kind::Nvme, Board::Pi5, 1920, 1080, false, 10, false, "lossless_jpeg", 60, 0, Label::Probable, Group::Sensor,
      "follows the 2K 12-bit row" },
    { Kind::Nvme, Board::Pi5, 3840, 2160, true, 16, true, "none", 24, 0, Label::Derived, Group::BackEnd,
      "25 fps held 3 and 5 minutes at 307 MB/s, drive up to 66 C; Log costs the back end 2-3 fps" },
    { Kind::Nvme, Board::Pi5, 2880, 2160, false, 12, false, "lossless_jpeg", 56, 0, Label::Derived, Group::Encoder,
      "59 fps at 60 s" },
    // Samsung T7 over USB, about 100-105 MB/s in takes: Clear HDR lossless, where it holds more.
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 16, false, "lossless_jpeg", 9, 0, Label::Derived, Group::Drive,
      "about 10 MB per frame at 105 MB/s" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, true, 16, false, "lossless_jpeg", 27, 0, Label::Measured, Group::Drive,
      "lossless 27 fps passed 30 minutes (older build)" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 12, false, "lossless_jpeg", 12, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, false, 12, false, "lossless_jpeg", 18, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed; 4K never run on this drive" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, false, 12, false, "lossless_jpeg", 48, 0, Label::Measured, Group::Drive,
      "lossless 48 fps passed 30 minutes" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, false, 10, false, "lossless_jpeg", 20, 0, Label::Derived, Group::Drive,
      "frame size over the drive's speed" },
    { Kind::UsbSsd, Board::Pi5, 1920, 1080, false, 10, false, "lossless_jpeg", 48, 0, Label::Probable, Group::Drive,
      "follows the 2K 12-bit row" },
    { Kind::UsbSsd, Board::Pi5, 3840, 2160, true, 16, true, "lossless_jpeg", 8, 0, Label::Derived, Group::Drive,
      "the Clear HDR 16-bit row less the Log gain" },
    { Kind::UsbSsd, Board::Pi5, 2880, 2160, false, 12, false, "lossless_jpeg", 24, 0, Label::Derived, Group::Drive,
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
      "4K at 40-42 fps and 2880x2160 at 56 fps" },
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
    bool splitOn = true;            // the clip folder is split into parts
    bool fatLike = false;           // exFAT or FAT: a flat folder slows file creation
    double fillFraction = 0.0;      // how full the medium is, 0 to 1
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
// fps), else a pixel rate where the class has one; none when the class has no row at all. With Log on, a Clear HDR
// 16-bit mode takes the Log cell of its own size. The compression is the cell's: Dynamic resolution owns it.
struct Limit {
    bool found = false;             // false: no row for this class, only the sensor's ceiling applies
    double fps30 = 0.0;             // before the lever shifts
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
    if (context.log && mode.hdr && mode.bits == 16)
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
        out = { true, exact->fps30, exact->fpsShort, exact->label, exact->group, exact->compression, exact->source,
                exact, true, false };
    } else {
        for (const PixelRate &rate : kPixelRates) {
            if (rate.storage != context.storage || rate.board != context.board || rate.hdr != mode.hdr ||
                rate.bits != mode.bits || pixels <= 0.0)
                continue;
            const double fps = std::max(rate.mpxPerSecond * 1e6 / pixels, larger ? larger->fps30 : 0.0);
            out = { true, fps, 0.0, weaker(rate.label, larger ? larger->label : rate.label), rate.group,
                    "lossless_jpeg", rate.source, larger, false, true };
            break;
        }
        if (!out.found && larger)
            out = { true, larger->fps30, larger->fpsShort, weaker(larger->label, Label::Derived), larger->group,
                    larger->compression, larger->source, larger, false, false };
    }
    if (out.found && !context.storageMeasured)
        out.label = Label::Unconfirmed;
    return out;
}

// What a mode serves after the levers: cap is the step its limit rounds down to (0 = the mode is not used).
// limit is the number before rounding, 0 when the table has no row. by names the wall: back end, encoder, drive,
// sensor (the sensor's own ceiling is the lower one, or there is no row) or TDN.
struct Cap {
    double fps = 0.0;
    double limit = 0.0;
    Limit row;
    const char *by = "sensor";
    const char *lever = "";         // the lever that moved the number: TDN, phase lock, aids or split
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

} // namespace apertar::sustained
