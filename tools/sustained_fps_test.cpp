// Host test for src/camera/SustainedFps.hpp (the known fps limits and the take length each is for). The numbers of table A are
// pinned on purpose: a change of an analysis result must change this file too.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/sustained_fps_test.cpp -o /tmp/sft && /tmp/sft
#include "camera/SustainedFps.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace apertar::sustained;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}
static bool same(double a, double b) { return std::fabs(a - b) < 1e-6; }

static Context nvme() { return Context{}; }
static Context t7()
{
    Context c;
    c.storage = Kind::UsbSsd;
    return c;
}
static const Mode k4kHdr16{ 3840, 2160, true, 16, false };
static const Mode k2kHdr16{ 1920, 1080, true, 16, true };
static const Mode k4kHdr12{ 3840, 2160, true, 12, false };
static const Mode k4kSdr12{ 3840, 2160, false, 12, false };
static const Mode k2kSdr12{ 1920, 1080, false, 12, true };
static const Mode k4kSdr10{ 3840, 2160, false, 10, false };
static const Mode k2kSdr10{ 1920, 1080, false, 10, true };
static const Mode kCrop2880{ 2880, 2160, false, 12, false };

int main()
{
    // ---- the steps ----
    check(same(stepDown(0.0), 0.0) && same(stepDown(-3.0), 0.0), "stepDown: nothing stays nothing");
    check(same(stepDown(20.9), 20.0) && same(stepDown(24.0), 24.0) && same(stepDown(24.99), 24.0),
          "stepDown: below 25 the number itself, floored (24 stays usable)");
    check(same(stepDown(25.0), 25.0) && same(stepDown(29.99), 25.0), "stepDown: 25 to 30 is 25");
    check(same(stepDown(30.0), 30.0) && same(stepDown(32.5), 30.0), "stepDown: 30 to 33 is 30");
    check(same(stepDown(33.0), 33.0) && same(stepDown(49.9), 33.0), "stepDown: 33 to 50 is 33");
    check(same(stepDown(50.0), 50.0) && same(stepDown(59.9), 50.0), "stepDown: 50 to 60 is 50");
    check(same(stepDown(60.0), 60.0) && same(stepDown(69.9), 60.0), "stepDown: 60 to 70 is 60");
    check(same(stepDown(70.0), 70.0) && same(stepDown(85.66), 80.0) && same(stepDown(280.0), 280.0),
          "stepDown: above 60 ten apart");
    check(same(stepDown(33.0 - 1e-12), 33.0), "stepDown: a rounding error under a step still lands on it");

    // ---- table A, pinned: fps30 and its step, NVMe then USB SSD ----
    struct Row { const char *name; Mode mode; bool log; double nvme, nvmeCap, t7, t7Cap; };
    const Row rows[] = {
        { "4K Clear HDR 16-bit", k4kHdr16, false, 23, 23, 9, 9 },
        { "2K Clear HDR 16-bit", k2kHdr16, false, 30, 30, 27, 25 },
        { "4K Clear HDR 12-bit", k4kHdr12, false, 24, 24, 12, 12 },
        { "4K SDR 12-bit", k4kSdr12, false, 40, 33, 18, 18 },
        { "2K SDR 12-bit", k2kSdr12, false, 60, 60, 48, 33 },
        { "4K SDR 10-bit", k4kSdr10, false, 44, 33, 20, 20 },
        { "2K SDR 10-bit", k2kSdr10, false, 60, 60, 48, 33 },
        { "4K Clear HDR 16-bit + Log", k4kHdr16, true, 25, 25, 8, 8 },
        { "2880x2160 SDR 12-bit", kCrop2880, false, 60, 60, 24, 24 },
    };
    for (const Row &row : rows) {
        Context a = nvme(), b = t7();
        a.log = b.log = row.log;
        const Limit la = limitFor(a, row.mode), lb = limitFor(b, row.mode);
        const std::string name = row.name;
        check(la.found && la.exact && same(la.fps30, row.nvme) && same(capFor(a, row.mode, 280.0).fps, row.nvmeCap),
              ("table A NVMe: " + name + " " + std::to_string(int(row.nvme)) + " fps, step " +
               std::to_string(int(row.nvmeCap))).c_str());
        check(lb.found && lb.exact && same(lb.fps30, row.t7) && same(capFor(b, row.mode, 280.0).fps, row.t7Cap),
              ("table A USB SSD: " + name + " " + std::to_string(int(row.t7)) + " fps, step " +
               std::to_string(int(row.t7Cap))).c_str());
    }

    // ---- labels, groups, compression, short takes ----
    {
        const Limit hdr = limitFor(nvme(), k4kHdr16);
        check(hdr.label == Label::Measured && hdr.group == Group::BackEnd && !std::strcmp(hdr.compression, "none") &&
                  hdr.minutes == 15 && same(hdr.fpsShort, 25.0),
              "NVMe 4K Clear HDR 16-bit: measured for 15 minutes (25 for short takes), back end, uncompressed");
        const Limit sdr = limitFor(nvme(), k4kSdr12);
        check(sdr.label == Label::Measured && sdr.minutes == 5 && sdr.group == Group::Encoder && !std::strcmp(sdr.compression, "lossless_jpeg"),
              "NVMe 4K SDR 12-bit: measured for 5 minutes on a hot drive, encoder, lossless");
        check(limitFor(nvme(), k2kSdr12).label == Label::Measured && limitFor(nvme(), k2kSdr12).minutes == 1 &&
                  limitFor(nvme(), k2kSdr12).group == Group::Sensor,
              "NVMe 2K SDR 12-bit: measured for 1 minute, sensor wall");
        const Limit t7hdr = limitFor(t7(), k2kHdr16);
        check(t7hdr.label == Label::Measured && t7hdr.group == Group::Drive && !std::strcmp(t7hdr.compression, "lossless_jpeg"),
              "USB SSD 2K Clear HDR 16-bit: measured, drive, lossless");
        bool everyCellHasSource = true, everyFpsPositive = true;
        for (const Cell &cell : kCells) {
            everyCellHasSource = everyCellHasSource && cell.source && *cell.source;
            const bool notUsed = cell.log && !cell.hdr && cell.fps30 == 0.0 && cell.minutes == 0;
            everyFpsPositive = everyFpsPositive && (notUsed || (cell.fps30 > 0.0 && cell.fpsShort >= 0.0 && cell.minutes > 0 &&
                               (cell.label == Label::Measured || cell.minutes == 30)));
        }
        check(everyCellHasSource && everyFpsPositive,
              "every cell has a source, a positive limit and a take length (30 unless measured); SDR + Log may be 0, not used");
        check(!std::strcmp(labelName(Label::Measured), "measured") && !std::strcmp(labelName(Label::Derived), "derived") &&
                  !std::strcmp(labelName(Label::Probable), "probable") && !std::strcmp(labelName(Label::Unconfirmed), "unconfirmed"),
              "labelName: measured, derived, probable, unconfirmed");
        check(!std::strcmp(groupName(Group::BackEnd), "back end") && !std::strcmp(groupName(Group::Encoder), "encoder") &&
                  !std::strcmp(groupName(Group::Drive), "drive") && !std::strcmp(groupName(Group::Sensor), "sensor"),
              "groupName: back end, encoder, drive, sensor");
        check(weaker(Label::Measured, Label::Probable) == Label::Probable && weaker(Label::Unconfirmed, Label::Derived) == Label::Unconfirmed,
              "weaker: the less sure label wins");
    }

    // ---- a mode without a cell: the nearest larger one of its class, else the pixel rate, else nothing ----
    {
        const Limit crop = limitFor(nvme(), Mode{ 3840, 1744, true, 16, false });
        check(crop.found && !crop.exact && same(crop.fps30, 23.0) && crop.minutes == 15 && crop.cell && crop.cell->width == 3840 &&
                  crop.label == Label::Derived,
              "3840x1744 Clear HDR 16-bit takes the 4K cell (23 fps, 15 minutes), derived at best");
        const Limit small = limitFor(nvme(), Mode{ 1920, 1016, true, 16, false });
        check(small.found && !small.exact && same(small.fps30, 30.0) && small.cell && small.cell->width == 1920 &&
                  small.label == Label::Derived,
              "1920x1016 Clear HDR 16-bit takes the 2K cell, not the 4K one: nearest larger, derived at best");
        const Limit hdr12 = limitFor(nvme(), Mode{ 1920, 1080, true, 12, false });
        check(hdr12.found && same(hdr12.fps30, 24.0), "Clear HDR 12-bit 2K has no cell: the 4K one (24 fps) stands in");
        const Limit rate = limitFor(nvme(), Mode{ 3840, 1744, false, 12, false });
        check(rate.found && rate.pixelRate && !rate.exact && same(rate.fps30, 330.0e6 / (3840.0 * 1744.0)) &&
                  rate.label == Label::Derived && rate.group == Group::Encoder && !std::strcmp(rate.compression, "lossless_jpeg"),
              "3840x1744 SDR 12-bit: the pixel rate gives 49.3 fps, derived");
        const Limit rateT7 = limitFor(t7(), Mode{ 3840, 1744, false, 12, false });
        check(rateT7.pixelRate && same(rateT7.fps30, 149.0e6 / (3840.0 * 1744.0)) && rateT7.group == Group::Drive,
              "3840x1744 SDR 12-bit on the USB SSD: the drive's pixel rate, 22.3 fps");
        const Limit between = limitFor(nvme(), Mode{ 3840, 2076, false, 12, false });
        check(between.pixelRate && between.fps30 > 40.0 && between.fps30 < 42.0,
              "3840x2076 SDR 12-bit: the pixel rate, never below the 4K cell's 40");
        check(!limitFor(nvme(), Mode{ 4096, 2304, true, 16, false }).found,
              "a Clear HDR 16-bit mode larger than every cell has no row");
        check(!limitFor(nvme(), Mode{ 3840, 2160, false, 16, false }).found &&
                  !limitFor(nvme(), Mode{ 1920, 1080, false, 8, false }).found,
              "a class with no cell at all has no row");
        Context other = nvme();
        other.board = Board::Other;
        const Cap none = capFor(other, k4kHdr16, 30.0);
        check(!limitFor(other, k4kHdr16).found && same(none.fps, 30.0) && !std::strcmp(none.by, "sensor") && same(none.limit, 0.0),
              "another board: no row, the sensor's ceiling alone (30 fps)");
        Context unknown = nvme();
        unknown.storageMeasured = false;
        unknown.storage = Kind::UsbSsd;
        const Limit stand = limitFor(unknown, k2kSdr12);
        check(stand.found && same(stand.fps30, 48.0) && stand.label == Label::Unconfirmed,
              "storage not measured: the USB SSD column, labelled unconfirmed");
    }

    // ---- the sensor's ceiling and the wall that wins ----
    {
        const Cap held = capFor(nvme(), k2kHdr16, 25.0);
        check(same(held.fps, 25.0) && !std::strcmp(held.by, "sensor") && same(held.limit, 30.0),
              "sensor ceiling 25 under a limit of 30: 25, by the sensor");
        const Cap lower = capFor(nvme(), k4kSdr12, 30.0);
        check(same(lower.fps, 30.0) && !std::strcmp(lower.by, "sensor"), "4K SDR 12-bit with a sensor ceiling of 30: step 30");
        const Cap table = capFor(nvme(), k4kHdr16, 30.0);
        check(same(table.fps, 23.0) && same(table.limit, 23.0) && !std::strcmp(table.by, "back end") && !*table.lever,
              "4K Clear HDR 16-bit: 23 fps, by the back end, no lever");
        check(same(capFor(t7(), k4kSdr12, 60.0).fps, 18.0) && !std::strcmp(capFor(t7(), k4kSdr12, 60.0).by, "drive"),
              "4K SDR 12-bit on the USB SSD: 18 fps, by the drive");
    }

    // ---- levers (table B) ----
    {
        Context c = nvme();
        c.log = true;
        check(same(limitFor(c, k4kHdr16).fps30, 25.0) && limitFor(c, k4kHdr16).cell->log, "Log on: 4K Clear HDR 16-bit takes the Log row");
        check(same(limitFor(c, k2kHdr16).fps30, 30.0) && same(limitFor(c, k2kSdr12).fps30, 60.0),
              "Log on: no effect at 2K");
        check(limitFor(c, k4kSdr12).cell->log && same(capFor(c, k4kSdr12, 60.0).fps, 0.0) && same(capFor(c, k4kSdr10, 60.0).fps, 0.0),
              "Log on: 4K SDR takes its Log row, not used (dropped at every fps tried)");
        c.lock = true;
        const Cap lock = capFor(c, k4kHdr16, 30.0);
        check(same(lock.limit, 24.0) && same(lock.fps, 24.0) && !std::strcmp(lock.lever, "phase lock"),
              "phase lock costs a back-end mode 1 fps: 25 becomes 24");
        check(same(capFor(c, kCrop2880, 60.0).limit, 60.0) && same(capFor(c, k2kHdr16, 30.0).limit, 30.0),
              "phase lock costs nothing in an encoder or sensor mode");
        c.aidsOn = false;
        check(same(capFor(c, k4kHdr16, 30.0).limit, 23.0) && !std::strcmp(capFor(c, k4kHdr16, 30.0).lever, "aids"),
              "aids off costs a back-end mode another fps");
        Context a = nvme();
        a.audioOn = false;
        check(same(capFor(a, k4kHdr16, 30.0).fps, 23.0), "audio off changes nothing: the table is for audio on");
        Context t = nvme();
        t.tdn = true;
        const Cap tdn = capFor(t, k4kSdr12, 60.0);
        check(same(tdn.fps, 0.0) && !std::strcmp(tdn.by, "TDN") && !std::strcmp(tdn.lever, "TDN"),
              "TDN on: an unbinned 4K mode is not used (step 0, by TDN)");
        check(same(capFor(t, Mode{ 3840, 1744, false, 12, false }, 60.0).fps, 0.0), "TDN on: a crop is not used either");
        check(same(capFor(t, k2kSdr12, 60.0).fps, 60.0) && same(capFor(t, k2kHdr16, 30.0).fps, 30.0),
              "TDN on: a binned 2K mode is not affected");
    }

    // ---- the flat folder (split off) ----
    {
        check(same(std::floor(flatFolderMaxFps()), 32.0), "flat folder: a 30-minute take is flat up to 32 fps");
        check(same(std::round(flatFolderMaxFps(3040.0)), 25.0), "flat folder: 50 minutes at 25 fps");
        check(same(std::round(1.9e6 / (30.0 * 30.0) / 60.0), 35.0) && same(std::round(1.9e6 / (48.0 * 48.0) / 60.0), 14.0),
              "flat folder: 35 minutes at 30 fps and 14 at 48");
        Context c = nvme();
        c.splitOn = false;
        c.fatLike = true;
        const Cap flat = capFor(c, k2kSdr12, 60.0);
        check(same(flat.fps, 30.0) && !std::strcmp(flat.lever, "split"),
              "split off on exFAT: 2K SDR 12-bit (60) is held at the flat-folder limit, step 30");
        check(same(capFor(c, k4kHdr16, 30.0).fps, 23.0) && !*capFor(c, k4kHdr16, 30.0).lever,
              "split off: a mode already under 32 fps is left alone");
        c.fatLike = false;
        check(same(capFor(c, k2kSdr12, 60.0).fps, 60.0), "split off on ext4: no flat-folder limit");
        c.fatLike = true;
        c.splitOn = true;
        check(same(capFor(c, k2kSdr12, 60.0).fps, 60.0), "split on: no flat-folder limit");
    }

    // ---- the sentence for the limit line ----
    {
        const auto why = [](const Context &c, const Mode &m, double ceiling) { return whyText(c, m, capFor(c, m, ceiling), ceiling); };
        check(why(nvme(), k4kHdr16, 30.0) == "4K Clear HDR 16-bit holds 23 fps for 15-minute takes on this NVMe card (measured). "
                                         "25 fps holds up to 8 minutes (measured).",
              "why: 4K Clear HDR 16-bit on the NVMe holds 23 fps for the 15 minutes measured, 25 for short takes");
        Context sdrLog = nvme();
        sdrLog.log = true;
        check(why(sdrLog, k4kSdr12, 60.0) == "4K SDR 12-bit + Log drops frames at every fps tried on this NVMe card (measured).",
              "why: 4K SDR + Log is not used");
        check(why(t7(), k2kSdr12, 60.0) == "2K SDR 12-bit holds 33 fps for 30-minute takes on this USB SSD (measured).",
              "why: 2K SDR 12-bit on the USB SSD holds 33 fps, measured, no short-take sentence");
        Context lock = nvme();
        lock.log = lock.lock = true;
        check(why(lock, k4kHdr16, 30.0) == "4K Clear HDR 16-bit + Log holds 24 fps for 30-minute takes on this NVMe card (measured). "
                                           "Phase lock is on: 4K Clear HDR 16-bit + Log holds 24 fps instead of 25.",
              "why: phase lock names the loss");
        Context tdn = nvme();
        tdn.tdn = true;
        check(why(tdn, k4kSdr12, 60.0) == "Preview temporal denoise is on: 4K SDR 12-bit drops frames at every rate tried (from 23 fps). "
                                          "Turn it off to use it.",
              "why: TDN on an unbinned mode");
        Context flat = nvme();
        flat.splitOn = false;
        flat.fatLike = true;
        check(why(flat, k2kSdr12, 60.0) == "2K SDR 12-bit holds 30 fps for 1-minute takes on this NVMe card (measured). "
                                           "Split long takes is off: takes over 30 minutes slow down above 32 fps. Turn it on.",
              "why: split off on exFAT");
        Context unknown = nvme();
        unknown.storage = Kind::UsbSsd;
        unknown.storageMeasured = false;
        check(why(unknown, k4kSdr12, 60.0) == "4K SDR 12-bit holds 18 fps for 30-minute takes on this USB SSD (unconfirmed). "
                                              "Storage type not measured: limits are the USB SSD's. Run the speed test.",
              "why: storage not measured");
        check(why(nvme(), Mode{ 4096, 2304, true, 16, false }, 25.0) ==
                  "No measurement for 4096x2304 Clear HDR 16-bit: the sensor's own limit of 25 fps applies.",
              "why: a mode with no row names the sensor's limit");
        Context board = nvme();
        board.board = Board::Other;
        check(why(board, k4kHdr16, 30.0) == "Limits are not measured on this Raspberry Pi.", "why: another board");
        Context full = nvme();
        full.fillFraction = 0.96;
        check(why(full, k2kHdr16, 30.0) == "2K Clear HDR 16-bit holds 30 fps for 1-minute takes on this NVMe card (measured). "
                                           "The card is over 94 percent full: it stalled near 96 percent in 4K Clear HDR 16-bit takes.",
              "why: a nearly full NVMe card");
        check(modeName(Mode{ 2880, 2160, false, 12, false }) == "2880x2160 SDR 12-bit" && Context{} == Context{},
              "modeName: sizes other than 4K and 2K are written out; a Context compares by value");
    }

    // ---- the write speed test only lowers a drive-limited cell ----
    {
        Context c = t7();
        check(same(capFor(c, k4kSdr12, 60.0).fps, 18.0), "speed test: none, the cell's 18 fps");
        c.speedMBps = 80.0;
        const Cap slow = capFor(c, k4kSdr12, 60.0);
        check(same(slow.limit, 0.75 * 80.0 * 18.0 / 105.0) && same(slow.fps, 10.0) && !std::strcmp(slow.lever, "speed test") &&
                  !std::strcmp(slow.by, "drive"),
              "speed test: 80 MB/s on the USB SSD column lowers 4K SDR 12-bit from 18 to 10, by the drive");
        c.speedMBps = 140.0;
        check(same(capFor(c, k4kSdr12, 60.0).fps, 18.0) && !*capFor(c, k4kSdr12, 60.0).lever, "speed test: 140 MB/s (0.75 of it is the T7's 105) lowers nothing");
        c.speedMBps = 900.0;
        check(same(capFor(c, k4kSdr12, 60.0).fps, 18.0), "speed test: a faster drive never raises a limit");
        Context n = nvme();
        n.speedMBps = 20.0;
        check(same(capFor(n, k4kHdr16, 30.0).fps, 23.0) && same(capFor(n, k2kSdr12, 60.0).fps, 60.0),
              "speed test: the NVMe column's cells are not drive-limited, no guard");
        Context sd = t7();
        sd.storageMeasured = false;
        sd.speedMBps = 40.0;
        check(same(capFor(sd, k2kSdr12, 60.0).fps, 13.0), "speed test: an SD card at 40 MB/s on the USB SSD column: 2K SDR 12-bit 48 fps becomes 13.7, below 25 the whole number");
        const auto why = [](const Context &ctx, const Mode &m, double ceiling) { return whyText(ctx, m, capFor(ctx, m, ceiling), ceiling); };
        check(why(t7(), k4kSdr12, 60.0).find("write speed test") == std::string::npos, "why: no test, no sentence");
        Context slowCtx = t7();
        slowCtx.speedMBps = 80.0;
        Context stepCtx = t7();
        stepCtx.speedMBps = 80.0;
        check(steppedDownText(stepCtx, 20.0, k2kSdr12, k2kHdr16, capFor(stepCtx, k2kHdr16, 30.0)) ==
                  "This USB SSD writes about 80 MB/s: 2K Clear HDR 16-bit holds 15 fps (measured). At 20 fps the camera records 2K SDR 12-bit.",
              "stepped down: a measured drive speed replaces the T7's 105 MB/s");
        Context sdStep = t7();
        sdStep.storageMeasured = false;
        sdStep.speedMBps = 80.0;
        check(steppedDownText(sdStep, 20.0, k2kSdr12, k2kHdr16, capFor(sdStep, k2kHdr16, 30.0)) ==
                  "At 20 fps the camera records 2K SDR 12-bit: 2K Clear HDR 16-bit holds 15 fps (unconfirmed). "
                  "The write speed test measured 80 MB/s on this card. Storage type not measured: limits are the USB SSD's. Run the speed test.",
              "stepped down: the speed test is named when it lowered the better mode of an unmeasured drive");
        check(why(slowCtx, k4kSdr12, 60.0) == "4K SDR 12-bit holds 10 fps for 30-minute takes on this USB SSD (derived). "
                                              "The write speed test measured 80 MB/s on this card.",
              "why: the sentence names the measured speed");
    }

    // ---- the sentences when a mode below the best one runs ----
    {
        const auto stepped = [](const Context &c, double fps, const Mode &chosen, const Mode &best, double bestCeiling) {
            return steppedDownText(c, fps, chosen, best, capFor(c, best, bestCeiling));
        };
        check(stepped(nvme(), 30.0, k2kHdr16, k4kHdr16, 30.0) ==
                  "At 30 fps the camera records 2K Clear HDR 16-bit: 4K Clear HDR 16-bit holds 23 fps for 15 minutes on this NVMe card (measured).",
              "stepped down: 4K Clear HDR 16-bit holds 23, so 2K Clear HDR 16-bit at 30");
        check(stepped(nvme(), 33.0, k4kSdr12, k2kHdr16, 30.0) ==
                  "At 33 fps the camera records 4K SDR 12-bit: 2K Clear HDR 16-bit holds 30 fps (measured).",
              "stepped down: giving up Clear HDR names the last Clear HDR mode, no storage clause");
        check(stepped(t7(), 25.0, k2kHdr16, k4kSdr12, 60.0) ==
                  "This USB SSD writes about 105 MB/s: 4K SDR 12-bit holds 18 fps (derived). At 25 fps the camera records 2K Clear HDR 16-bit.",
              "stepped down: a drive-limited mode names the drive");
        Context unknown = t7();
        unknown.storageMeasured = false;
        check(stepped(unknown, 25.0, k2kHdr16, k4kSdr12, 60.0) ==
                  "At 25 fps the camera records 2K Clear HDR 16-bit: 4K SDR 12-bit holds 18 fps for 30 minutes on this USB SSD (unconfirmed). "
                  "Storage type not measured: limits are the USB SSD's. Run the speed test.",
              "stepped down: an unmeasured drive is not named as the T7");
        Context tdn = nvme();
        tdn.tdn = true;
        check(stepped(tdn, 25.0, k2kHdr16, k4kHdr16, 30.0) ==
                  "Preview temporal denoise is on: 4K Clear HDR 16-bit drops frames at every rate tried (from 23 fps). "
                  "At 25 fps the camera records 2K Clear HDR 16-bit. Turn it off to use 4K Clear HDR 16-bit.",
              "stepped down: TDN names itself and the way back");
        check(stepped(nvme(), 25.0, k2kHdr16, Mode{ 4096, 2304, true, 16, false }, 25.0) ==
                  "At 25 fps the camera records 2K Clear HDR 16-bit: 4096x2304 Clear HDR 16-bit is limited to 25 fps by the sensor.",
              "stepped down: a mode with no row is limited by the sensor");
        Context board = nvme();
        board.board = Board::Other;
        check(stepped(board, 25.0, k2kHdr16, k4kHdr16, 30.0) == "Limits are not measured on this Raspberry Pi.", "stepped down: another board");
        check(highestText(nvme(), 60.0, k2kSdr12) == "60 fps is the most the enabled resolutions hold here (2K SDR 12-bit).",
              "highest: the most the enabled modes hold");
        check(neverUsedText(k4kHdr16, capFor(nvme(), k4kHdr16, 30.0)) == " Never used here: 4K Clear HDR 16-bit already holds 23 fps.",
              "never used: the earlier mode and its cap");
    }

    // ---- the card ----
    {
        Context c = nvme();
        c.fillFraction = 0.95;
        check(cardNearlyFull(c), "NVMe card at 95 percent: the warning");
        c.fillFraction = 0.90;
        check(!cardNearlyFull(c), "NVMe card at 90 percent: no warning");
        c.storage = Kind::UsbSsd;
        c.fillFraction = 0.99;
        check(!cardNearlyFull(c), "the warning is the NVMe card's");
    }

    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
