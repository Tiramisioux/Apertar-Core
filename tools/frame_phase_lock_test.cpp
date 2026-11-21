// Host test for src/camera/FramePhaseLock.hpp: the grid lock of design §7.2-§7.4 (T57: the grid, Q, the PI, the
// stretch, its landing and its tracking, a take's own phase), against exact grid values and a plant model.
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/frame_phase_lock_test.cpp -o /tmp/fplt && /tmp/fplt
#include "camera/FramePhaseLock.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace apertar;
using Lock = FramePhaseLock;

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);       \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

static const double kH = 550.0 / 74.25;   // 7.407407 us: HMAX 550 at 891 MHz, 4K Clear HDR 16-bit
static const int kStep = 2;               // IMX585 VMAX quantum
static const int kN = 8;                  // Clear HDR raw buffers: the hold bound is N + 6 = 14

static double periodNs(int64_t num, int64_t den) { return 1e9 * double(den) / double(num); }

// What libcamera's default blanking and the IMX585 driver make of a {d, d+1} pin: floor to whole lines,
// then floor VMAX to the step.
static int64_t realisedLines(int64_t pinUs, double H, int step)
{
    const int64_t lines = int64_t(std::floor(double(pinUs) / H));
    return lines / step * step;
}

static Lock::Input frame(int64_t ts, int64_t num, int64_t den, double H = kH, int step = kStep)
{
    Lock::Input in;
    in.enabled = true;
    in.recording = false;
    in.rateNum = num;
    in.rateDen = den;
    in.timestampNs = ts;
    in.lineUs = H;
    in.vmaxStep = step;
    in.inFlightExcessNs = 0;
    in.stretchLanded = false;
    in.requestCount = kN;
    return in;
}

// A timestamp at slot k plus e ns on the num/den grid (exact for whole-ns periods, else rounded).
static int64_t at(int64_t slot, int64_t num, int64_t den, int64_t errorNs)
{
    const PhaseLockInt128 scaled = static_cast<PhaseLockInt128>(slot) * 1000000000 * den;
    return int64_t((scaled + num / 2) / num) + errorNs;
}

// 1. The grid: slot and error at large timestamps, against exact values from Python's fractions.
static void testGridOracle()
{
    struct Case { int64_t ts, num, den, slot, errorNs; };
    const Case cases[] = {
        { 432000012345678LL, 25000, 1000, 10800000LL, 12345678 },        // 5 days, 25 fps
        { 432000032345678LL, 25000, 1000, 10800001LL, -7654322 },        // past T/2: the next slot
        { 432000020000000LL, 25000, 1000, 10800000LL, 20000000 },        // exactly +T/2 stays: (-T/2, T/2]
        { 431999980000000LL, 25000, 1000, 10799999LL, 20000000 },        // exactly -T/2 is the slot before
        { 2592000987654321LL, 24000, 1001, 62145878LL, -7262345 },       // 30 days, 23.976 (exact -7262345.67)
        { 8640000123456789LL, 24000, 1001, 207152850LL, 4706789 },       // 100 days
        { 8640000123456789LL, 25000, 1000, 216000003LL, 3456789 },
        { 8640000123456789LL, 30000, 1001, 258941063LL, -11976544 },     // 29.97 (exact -11976544.33)
        { 8640000123456789LL, 120000, 1001, 1035764251LL, -3634877 },    // 119.88 (exact -3634877.67)
    };
    for (const Case &c : cases) {
        const Lock::GridPoint p = Lock::gridPoint(c.ts, c.num, c.den);
        CHECK(p.slot == c.slot && p.errorNs == c.errorNs, "ts=%" PRId64 " %" PRId64 "/%" PRId64 ": slot %" PRId64
              " err %" PRId64 ", want %" PRId64 " %" PRId64, c.ts, c.num, c.den, p.slot, p.errorNs, c.slot, c.errorNs);
        // step() reports the same, lock on or off.
        Lock lock;
        Lock::Input in = frame(c.ts, c.num, c.den);
        Lock::Decision off;
        in.enabled = false;
        off = lock.step(in);
        CHECK(off.slot == c.slot && off.errorNs == c.errorNs && !off.active, "step with the lock off");
        in.enabled = true;
        const Lock::Decision on = lock.step(in);
        CHECK(on.slot == c.slot && on.errorNs == c.errorNs && on.active && on.rearmed, "step with the lock on");
    }
    // ts * num overflows int64 here: the case only passes with 128-bit arithmetic.
    CHECK(8640000123456789.0 * 120000.0 > 9.3e18, "the 100-day case overflows int64");
}

// 1b. e stays in (-T/2, T/2] and the slot is the nearest one, over random timestamps up to 200 days.
static void testGridSweep()
{
    std::mt19937_64 rng(57);
    const int64_t rates[][2] = { { 25000, 1000 }, { 24000, 1001 }, { 24000, 1000 }, { 30000, 1001 }, { 50000, 1000 } };
    int bad = 0;
    for (const auto &r : rates) {
        const PhaseLockInt128 M = static_cast<PhaseLockInt128>(1000000000) * r[1];
        for (int i = 0; i < 200000; ++i) {
            const int64_t ts = int64_t(rng() % 17280000000000000ULL);  // < 200 days
            const Lock::GridPoint p = Lock::gridPoint(ts, r[0], r[1]);
            // exact error in ns * num: E = ts * num - slot * M
            const PhaseLockInt128 E = static_cast<PhaseLockInt128>(ts) * r[0] - static_cast<PhaseLockInt128>(p.slot) * M;
            const bool inRange = 2 * E > -M && 2 * E <= M;
            const PhaseLockInt128 truncErr = E - static_cast<PhaseLockInt128>(p.errorNs) * r[0];  // 0 <= |.| < num
            const bool truncOk = (truncErr >= 0 ? truncErr : -truncErr) < r[0] && (truncErr == 0 || (truncErr > 0) == (E > 0));
            const bool nsRange = 2 * static_cast<PhaseLockInt128>(p.errorNs) * r[0] > -M &&
                2 * static_cast<PhaseLockInt128>(p.errorNs) * r[0] <= M;
            if (!inRange || !truncOk || !nsRange) {
                if (++bad < 5)
                    std::printf("  sweep bad: ts=%" PRId64 " rate %" PRId64 "/%" PRId64 " slot %" PRId64 " err %" PRId64 "\n",
                                ts, r[0], r[1], p.slot, p.errorNs);
            }
        }
    }
    CHECK(bad == 0, "%d of 1000000 random timestamps outside (-T/2, T/2] or not the nearest slot", bad);
    // Consecutive slots: a frame exactly one period later is the next slot with the same error (25 fps).
    const Lock::GridPoint a = Lock::gridPoint(at(123456789, 25000, 1000, 1234), 25000, 1000);
    const Lock::GridPoint b = Lock::gridPoint(at(123456790, 25000, 1000, 1234), 25000, 1000);
    CHECK(b.slot == a.slot + 1 && a.errorNs == 1234 && b.errorNs == 1234, "next slot %" PRId64 " %" PRId64, a.slot, b.slot);
}

// 2. Q: the mean period equals the request within 0.01 us over 1000 frames, and every pin floors to an
//    even V at H = 7.407407 us.
static void testQuantiser()
{
    const double lineTimes[] = { kH, 7.407407 };
    const double requests[] = { 40000.0, 40003.3, 39987.65, 41666.6666667, 41708.3333333, 40150.0, 39850.0, 33366.6667 };
    for (double H : lineTimes) {
        for (double r : requests) {
            Lock lock;
            double sum = 0.0;
            int badFloor = 0, odd = 0, badMargin = 0;
            for (int i = 0; i < 1000; ++i) {
                const Lock::Pin pin = lock.quantise(r, H, kStep);
                sum += double(pin.lines) * H;
                if (realisedLines(pin.us, H, kStep) != pin.lines || int64_t(std::floor(double(pin.us) / H)) != pin.lines)
                    ++badFloor;
                if (pin.lines % 2 != 0)
                    ++odd;
                // the half line: (V + 1/2) H <= d < (V + 1/2) H + 1
                const double half = (double(pin.lines) + 0.5) * H;
                if (!(double(pin.us) >= half && double(pin.us) < half + 1.0))
                    ++badMargin;
                if (std::llabs(pin.periodNs - std::llround(double(pin.lines) * H * 1000.0)) > 0)
                    ++badFloor;
            }
            const double mean = sum / 1000.0;
            CHECK(std::fabs(mean - r) <= 0.01, "H %.6f request %.4f: mean %.6f (off %.6f us)", H, r, mean, mean - r);
            CHECK(badFloor == 0 && odd == 0 && badMargin == 0, "H %.6f request %.4f: %d pins floor wrong, %d odd V, %d outside the half line",
                  H, r, badFloor, odd, badMargin);
        }
    }
    // vmaxStep 1 (every other family): V any integer, same mean.
    {
        Lock lock;
        double sum = 0.0;
        for (int i = 0; i < 1000; ++i) {
            const Lock::Pin pin = lock.quantise(41666.6666667, 10.0, 1);
            sum += double(pin.lines) * 10.0;
        }
        CHECK(std::fabs(sum / 1000.0 - 41666.6666667) <= 0.01, "step 1 mean %.6f", sum / 1000.0);
    }
    // Without H: today's whole-us rounding, no carry.
    {
        Lock lock;
        const Lock::Pin a = lock.quantise(40003.4, 0.0, kStep);
        const Lock::Pin b = lock.quantise(40003.6, 0.0, kStep);
        CHECK(a.us == 40003 && b.us == 40004 && a.lines == 0 && a.periodNs == 40003000 && lock.carryUs == 0.0,
              "no H: %" PRId64 " %" PRId64, a.us, b.us);
    }
    // Through step(): a meter (deadband 100000) at 24 fps holds the request at T and Q alternates
    // 5624/5626 lines; the realised mean is T.
    {
        Lock lock;
        lock.tuning.deadbandUs = 100000.0;
        const int64_t num = 24000, den = 1000;
        double sum = 0.0;
        int64_t pin = 0, v5624 = 0, v5626 = 0, others = 0;
        for (int k = 0; k < 1000; ++k) {
            Lock::Input in = frame(at(1000000 + k, num, den, 3000000), num, den);
            const Lock::Decision d = lock.step(in);
            CHECK(!d.stretch, "a meter never stretches");
            if (d.pushDuration)
                pin = d.durationUs;
            const int64_t v = realisedLines(pin, kH, kStep);
            sum += double(v) * kH;
            v == 5624 ? ++v5624 : v == 5626 ? ++v5626 : ++others;
        }
        CHECK(std::fabs(sum / 1000.0 - periodNs(num, den) / 1000.0) <= 0.01 && others == 0,
              "24 fps meter mean %.6f us (5624 x%" PRId64 ", 5626 x%" PRId64 ", other x%" PRId64 ")", sum / 1000.0, v5624, v5626, others);
        CHECK(lock.requestedUs == lock.nominalUs, "the meter's request stays T");
        std::printf("  Q at 24 fps through step(): 5624 x%" PRId64 ", 5626 x%" PRId64 ", mean %.6f us (T %.6f), pushes %" PRIu64 "\n",
                    v5624, v5626, sum / 1000.0, periodNs(num, den) / 1000.0, lock.pushes);
    }
}

// Landing error of a stretch: where the frame after the stretched one starts, against the grid (ns).
static double landingErrorNs(int64_t errorNs, int64_t excessNs, int64_t stretchNs, double T)
{
    double x = double(errorNs) + double(excessNs) + (double(stretchNs) - T);
    x -= std::round(x / T) * T;
    return x;
}

// 3. The stretch: size and sign with a known in-flight excess.
static void testStretchSize()
{
    const double halfStepNs = kStep * kH * 1000.0 / 2.0 + 1.0;
    struct Case { int64_t num, den, errorNs, excessNs; };
    const Case cases[] = {
        { 25000, 1000, -3210000, 120000 },   // early 3.21 ms, frames in flight long by 120 us: ext ~ 3.09 ms
        { 25000, 1000, 3210000, 120000 },    // late 3.21 ms: ext ~ T - 3.33 ms
        { 25000, 1000, -19950000, 0 },       // nearly half a period early
        { 25000, 1000, 1500000, -2000000 },  // excess larger than e
        { 24000, 1000, 5000000, -81481 },    // 24 fps, 11 base pins of 5624 lines in flight
        { 24000, 1001, -7262345, -50926 },   // 23.976
        { 24000, 1001, 12000000, 300000 },
    };
    for (const Case &c : cases) {
        const double T = periodNs(c.num, c.den);
        Lock lock;
        Lock::Input in = frame(at(5000000000LL, c.num, c.den, c.errorNs), c.num, c.den);
        in.inFlightExcessNs = c.excessNs;
        const Lock::Decision d = lock.step(in);
        CHECK(d.rearmed && d.stretch && d.holding && !d.servoRan, "e %" PRId64 ": stretch on the arm frame", c.errorNs);
        // the exact extension, from first principles: (-e - excess) mod T
        double ext = std::fmod(-double(d.errorNs) - double(c.excessNs), T);
        if (ext < 0)
            ext += T;
        const double got = double(d.stretchNs) - T;
        CHECK(std::fabs(got - ext) <= halfStepNs, "e %" PRId64 " excess %" PRId64 ": stretch +%.0f ns, want +%.0f", c.errorNs, c.excessNs, got, ext);
        CHECK(got > -halfStepNs && got < T + halfStepNs, "the stretch only lengthens: %.0f", got);
        const double land = landingErrorNs(d.errorNs, c.excessNs, d.stretchNs, T);
        CHECK(std::fabs(land) <= halfStepNs, "e %" PRId64 ": the next frame lands %.0f ns off the grid", c.errorNs, land);
        // the pin realises the period the Decision states
        CHECK(realisedLines(d.stretchUs, kH, kStep) * 1000.0 * kH - double(d.stretchNs) < 1.0 &&
              realisedLines(d.stretchUs, kH, kStep) % 2 == 0, "stretch pin %" PRId64 " floors to its V", d.stretchUs);
        // sign: an early frame (e < 0) stretches by about |e|, a late one by about T - e
        if (c.excessNs == 0 || std::llabs(c.excessNs) < std::llabs(c.errorNs))
            CHECK((c.errorNs < 0) == (got < T / 2), "e %" PRId64 ": early stretches less than T/2, late more (%.0f)", c.errorNs, got);
        // the requests after it get the lock pin again: Q's first pin of T, from a zero carry
        CHECK(d.pushDuration && d.durationUs == Lock::pinFor(T / 1000.0, kH, kStep).us && lock.carryUs == 0.0,
              "the lock pin %" PRId64 " goes out with the stretch", d.durationUs);
    }
    // The in-flight term moves the stretch by exactly the excess (within Q's step).
    {
        const int64_t num = 24000, den = 1000;
        Lock a, b;
        Lock::Input in = frame(at(7000000000LL, num, den, 5000000), num, den);
        in.inFlightExcessNs = 0;
        const Lock::Decision d0 = a.step(in);
        in.inFlightExcessNs = -81481;
        const Lock::Decision d1 = b.step(in);
        const double shift = double(d1.stretchNs - d0.stretchNs);
        CHECK(std::fabs(shift - 81481.0) <= kStep * kH * 1000.0 + 1.0, "-81 us in flight lengthens the stretch by %.0f ns", shift);
        std::printf("  stretch at 24 fps, e = +5 ms: %" PRId64 " us pin (%" PRId64 " ns) without the in-flight term, %" PRId64
                    " us (%" PRId64 " ns) with -81481 ns\n", d0.stretchUs, d0.stretchNs, d1.stretchUs, d1.stretchNs);
    }
    // Without H: whole us, from the same exact extension.
    {
        Lock lock;
        Lock::Input in = frame(at(3000000000LL, 25000, 1000, -3210000), 25000, 1000, 0.0);
        in.inFlightExcessNs = 120000;
        const Lock::Decision d = lock.step(in);
        CHECK(d.stretch && d.stretchUs == 40000 + 3090 && d.stretchNs == 43090000, "no H: stretch %" PRId64, d.stretchUs);
    }
}

// 3b. The stretched frame stands out from every frame around it, so its FrameDuration finds it: an extension within
//     the clamp, two Q steps and twice the landing tolerance of a whole period takes one period more.
static void testStretchStandsOut()
{
    const int64_t num = 25000, den = 1000;
    const double T = periodNs(num, den);
    const double clampNs = Lock::Tuning{}.clampUs * 1000.0;
    const double stepNs = kStep * kH * 1000.0;
    const double tolNs = double(Lock::landingToleranceNs(kH));
    const double standOutNs = clampNs + 2.0 * stepNs + 2.0 * tolNs;  // about 187 us
    const double lockMaxNs = T + clampNs + stepNs;                    // the longest lock or Q frame
    int bad = 0, grown = 0, runs = 0;
    for (int64_t errorUs = -19990; errorUs <= 19990; errorUs += 7) {
        if (std::llabs(errorUs) <= 1000)
            continue;
        for (int64_t excessUs : { -1500, -400, 0, 300, 1200 }) {
            Lock lock;
            Lock::Input in = frame(at(4000000, num, den, errorUs * 1000), num, den);
            in.inFlightExcessNs = excessUs * 1000;
            const Lock::Decision d = lock.step(in);
            ++runs;
            if (!d.stretch) {
                ++bad;
                continue;
            }
            double ext = std::fmod(-double(d.errorNs) - double(in.inFlightExcessNs), T);
            if (ext < 0)
                ext += T;
            const bool grows = ext <= standOutNs;
            grown += grows;
            const double want = grows ? ext + T : ext;
            if (std::fabs(double(d.stretchNs) - T - want) > stepNs / 2.0 + 1.0)
                ++bad;
            if (std::fabs(landingErrorNs(d.errorNs, in.inFlightExcessNs, d.stretchNs, T)) > stepNs / 2.0 + 1.0)
                ++bad;
            // no lock frame's FrameDuration (truncated to whole us) comes within the tolerance of it
            if (double(d.stretchNs) - lockMaxNs <= tolNs + 1000.0)
                ++bad;
        }
    }
    CHECK(bad == 0 && grown > 0, "%d of %d stretches wrong; %d took one period more", bad, runs, grown);
    std::printf("  stand-out: %d of %d stretches (|e| > 1 ms, 5 in-flight sums) took one period more\n", grown, runs);
    // e = -1.1 ms with +1.0 ms in flight wants a 0.1 ms extension: the stretch is 2T + 0.1 ms, and lands
    Lock lock;
    Lock::Input in = frame(at(4000000, num, den, -1100000), num, den);
    in.inFlightExcessNs = 1000000;
    const Lock::Decision d = lock.step(in);
    CHECK(d.stretch && std::fabs(double(d.stretchNs) - (2.0 * T + 100000.0)) <= stepNs / 2.0 + 1.0 &&
          std::fabs(landingErrorNs(d.errorNs, in.inFlightExcessNs, d.stretchNs, T)) <= stepNs / 2.0 + 1.0,
          "0.1 ms wanted: stretch %" PRId64 " ns", d.stretchNs);
    // without H: whole us, a 50 us tolerance, the same rule
    Lock noLine;
    in = frame(at(4000000, num, den, -1100000), num, den, 0.0);
    in.inFlightExcessNs = 1000000;
    const Lock::Decision n = noLine.step(in);
    CHECK(n.stretch && n.stretchNs == 2 * 40000000 + 100000 && Lock::landingToleranceNs(0.0) == Lock::kLandingToleranceNoLineNs,
          "no H: stretch %" PRId64 " ns", n.stretchNs);
}

// Run frames after a stretch at age 0: +6 ms through switchAge, then errorAfterNs. Returns the decisions
// by age (age 0 = the stretch frame).
static std::vector<Lock::Decision> holdRun(int landAge, int frames, int switchAge, int64_t errorAfterNs)
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    std::vector<Lock::Decision> out;
    Lock::Input in = frame(at(9000000, num, den, 6000000), num, den);
    out.push_back(lock.step(in));
    for (int age = 1; age <= frames; ++age) {
        in = frame(at(9000000 + age, num, den, age <= switchAge ? 6000000 : errorAfterNs), num, den);
        in.stretchLanded = age == landAge;
        out.push_back(lock.step(in));
        if (out.back().holding)
            CHECK(lock.requestedUs == lock.nominalUs && lock.integralUs == 0.0, "age %d: the PI is held at T", age);
    }
    return out;
}

// 4. The hold: until the stretched frame (Input::stretchLanded) plus 2 frames, and at most N + 6 frames.
static void testHold()
{
    // the stretched frame comes N + d frames after the stretch, d the pin's VBLANK delay: for d = 1 to 4 the hold
    // ends 2 frames after it, inside the bound, with no "not seen"
    for (int delay = 1; delay <= 4; ++delay) {
        const int land = kN + delay;
        const std::vector<Lock::Decision> d = holdRun(land, 20, land, 20000);
        bool held = true, notSeen = false, landedOnce = true;
        for (int age = 1; age <= land + Lock::kHoldAfterLanding; ++age)
            held = held && d[age].holding && !d[age].servoRan;
        for (int age = 0; age <= 20; ++age) {
            notSeen = notSeen || d[age].stretchNotSeen;
            landedOnce = landedOnce && d[age].stretchLanded == (age == land);
        }
        CHECK(held && !d[land + 3].holding && d[land + 3].servoRan, "pin delay %d: held through %d, the PI at %d", delay,
              land + 2, land + 3);
        CHECK(!notSeen && landedOnce, "pin delay %d: landed on %d only, no 'not seen'", delay, land);
    }
    CHECK(Lock::landingToleranceNs(kH) == 3704 && Lock::landingToleranceNs(2.0 * kH) == 7407, "half a line: %" PRId64,
          Lock::landingToleranceNs(kH));
    // landed at age 9: held through 11, the PI runs at 12
    {
        const std::vector<Lock::Decision> d = holdRun(9, 16, 9, 20000);
        CHECK(d[0].stretch && d[0].holding, "age 0 stretches");
        for (int age = 1; age <= 11; ++age)
            CHECK(d[age].holding && !d[age].servoRan && !d[age].stretch, "landed 9: age %d held", age);
        CHECK(d[9].stretchLanded && !d[8].stretchLanded && !d[10].stretchLanded, "landed flagged on age 9 only");
        CHECK(!d[12].holding && d[12].servoRan, "landed 9: the PI runs at age 12");
        for (int age = 0; age <= 16; ++age)
            CHECK(!d[age].stretchNotSeen, "landed 9: no 'not seen' (age %d)", age);
        // ages 1..9 still see +6 ms, more than 1 ms, and the lock is not locked: no second stretch while held
        for (int age = 1; age <= 16; ++age)
            CHECK(!d[age].stretch, "landed 9: no second stretch at age %d", age);
    }
    // never landed: held through N + 6 = 14, 'not seen' on 14, the PI at 15
    {
        const std::vector<Lock::Decision> d = holdRun(1000, 16, 14, 20000);
        for (int age = 1; age <= 14; ++age)
            CHECK(d[age].holding && !d[age].servoRan, "not landed: age %d held", age);
        for (int age = 1; age <= 16; ++age)
            CHECK(d[age].stretchNotSeen == (age == 14), "'not seen' only at age 14 (age %d: %d)", age, int(d[age].stretchNotSeen));
        CHECK(!d[15].holding && d[15].servoRan && !d[15].stretch, "not landed: the PI runs at age 15 (|e| 20 us)");
    }
    // landed late, at 13: the bound still ends the hold at 14
    {
        const std::vector<Lock::Decision> d = holdRun(13, 16, 13, 20000);
        CHECK(d[13].stretchLanded && d[14].holding && !d[15].holding && d[15].servoRan && !d[14].stretchNotSeen,
              "landed 13: held through 14, PI at 15");
    }
    // still off by more than 1 ms when the hold ends: preview stretches again
    {
        const std::vector<Lock::Decision> d = holdRun(1000, 16, 1000, 6000000);
        CHECK(!d[14].stretch && d[15].stretch, "a lock still > 1 ms off after the bound stretches again at age 15");
    }
    // a landed flag with no stretch under way is ignored
    {
        Lock lock;
        Lock::Input in = frame(at(100, 25000, 1000, 200000), 25000, 1000);
        in.stretchLanded = true;
        const Lock::Decision d = lock.step(in);
        CHECK(!d.stretchLanded && !d.holding && d.servoRan, "a stray landed flag is ignored");
    }
}

// 5. No stretch while recording (takePending_ || recordingFramesEnabled_) or with a deadband of 100000.
static void testNoStretch()
{
    const int64_t num = 25000, den = 1000;
    {
        Lock lock;
        Lock::Input in = frame(at(100, num, den, 5000000), num, den);
        in.recording = true;
        const Lock::Decision d = lock.step(in);
        CHECK(!d.stretch && !d.holding && d.servoRan, "recording: no stretch");
        // an arm in a take 5 ms off keeps the take's phase (testTakePhase): the PI holds T instead of pulling in
        CHECK(d.takePhaseKept && lock.takeOffsetNs == d.errorNs && lock.requestedUs == lock.nominalUs,
              "recording: the take keeps its phase, the PI holds T (%.3f)", lock.requestedUs);
        // a take keeps the PI: 300 frames late by 5 ms, never a stretch
        bool any = false;
        for (int k = 1; k < 300; ++k) {
            in = frame(at(100 + k, num, den, 5000000), num, den);
            in.recording = true;
            any = any || lock.step(in).stretch;
        }
        CHECK(!any, "recording: no stretch in 300 frames");
    }
    {
        Lock lock;
        lock.tuning.deadbandUs = 100000.0;
        bool any = false, moved = false;
        for (int k = 0; k < 300; ++k) {
            const Lock::Decision d = lock.step(frame(at(100 + k, num, den, k % 2 ? 15000000 : -19000000), num, den));
            any = any || d.stretch || d.holding;
            moved = moved || lock.requestedUs != lock.nominalUs;
        }
        CHECK(!any && !moved, "deadband 100000: no stretch and no PI step");
    }
    {
        // just above the deadband but under 1 ms: the PI, not a stretch
        Lock lock;
        const Lock::Decision d = lock.step(frame(at(100, num, den, 999000), num, den));
        CHECK(!d.stretch && d.servoRan, "|e| = 999 us: no stretch");
        Lock lock2;
        const Lock::Decision d2 = lock2.step(frame(at(100, num, den, -1001000), num, den));
        CHECK(d2.stretch, "|e| = 1001 us: a stretch");
    }
    {
        // no stretch while the controller forbids it (the saturated flag, a pre-roll pin): the PI runs instead,
        // and the first frame that allows it stretches
        Lock lock;
        Lock::Input in = frame(at(200, num, den, 5000000), num, den);
        in.stretchAllowed = false;
        const Lock::Decision d = lock.step(in);
        CHECK(d.rearmed && !d.stretch && !d.holding && d.servoRan && lock.requestedUs == lock.nominalUs - lock.tuning.clampUs,
              "stretch not allowed: the PI (%.3f)", lock.requestedUs);
        bool any = false;
        for (int k = 1; k < 50; ++k) {
            in = frame(at(200 + k, num, den, 5000000), num, den);
            in.stretchAllowed = false;
            const Lock::Decision x = lock.step(in);
            any = any || x.stretch || x.holding;
        }
        CHECK(!any, "not allowed: no stretch and no hold in 50 frames");
        CHECK(lock.step(frame(at(250, num, den, 5000000), num, den)).stretch, "allowed again: a stretch");
    }
}

// 5b. A stretch that never went out (its request could not carry it, the pre-roll pin replaced it): cancelStretch()
//     leaves no hold, no gate wait and no "not seen", and the PI runs on the next frame.
static void testCancel()
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    const Lock::Decision s = lock.step(frame(at(300, num, den, 6000000), num, den));
    CHECK(s.stretch && s.holding && lock.stretchAge == 0, "a stretch");
    lock.cancelStretch();
    CHECK(lock.stretchAge == -1 && lock.stretchLandedAge == -1, "cancelled");
    bool held = false, notSeen = false, servo = true;
    for (int k = 1; k < 30; ++k) {
        const Lock::Decision x = lock.step(frame(at(300 + k, num, den, 20000), num, den));
        held = held || x.holding;
        notSeen = notSeen || x.stretchNotSeen;
        servo = servo && x.servoRan;
    }
    CHECK(!held && !notSeen && servo, "after a cancel: the PI from the next frame, no hold, no 'not seen'");
    // still off: a new stretch may be decided at once
    Lock again;
    again.step(frame(at(400, num, den, 6000000), num, den));
    again.cancelStretch();
    CHECK(again.step(frame(at(401, num, den, 6000000), num, den)).stretch, "a cancel lets a new stretch be decided");
}

// 5c. A stretch in flight survives a release, disabled frames, a re-arm and a rate change: its request is still
//     queued, so the hold (and with it a take's first-frame gate) waits for it, and no second stretch starts.
static void testInFlight()
{
    const int64_t num = 25000, den = 1000;
    {
        // released at once, the lock off from then on: held through landing 10 + 2, inactive
        Lock lock;
        CHECK(lock.step(frame(at(500, num, den, 6000000), num, den)).stretch, "a stretch");
        lock.release(PhaseLockArmReason::FpsChange);
        std::vector<Lock::Decision> d(1);
        for (int age = 1; age <= 16; ++age) {
            Lock::Input in = frame(at(500 + age, num, den, 6000000), num, den);
            in.enabled = false;
            in.stretchLanded = age == 10;
            d.push_back(lock.step(in));
        }
        bool held = true;
        for (int age = 1; age <= 12; ++age)
            held = held && d[age].holding && !d[age].active;
        CHECK(held && !d[13].holding && d[10].stretchLanded, "released: held through landing 10 + 2 with the lock off");
        CHECK(lock.stretchAge == -1 && lock.nextArmReason == PhaseLockArmReason::FpsChange, "then over; the arm reason kept");
    }
    {
        // released, never landing: "not seen" at N + 6 with the lock off
        Lock lock;
        lock.step(frame(at(600, num, den, 6000000), num, den));
        lock.release();
        int notSeenAt = -1, lastHeld = -1;
        for (int age = 1; age <= 16; ++age) {
            Lock::Input in = frame(at(600 + age, num, den, 6000000), num, den);
            in.enabled = false;
            const Lock::Decision x = lock.step(in);
            if (x.stretchNotSeen)
                notSeenAt = age;
            if (x.holding)
                lastHeld = age;
        }
        CHECK(notSeenAt == kN + 6 && lastHeld == kN + 6, "released, never landed: 'not seen' at %d, held to %d", notSeenAt, lastHeld);
    }
    {
        // released and re-armed inside the hold (a toggle, a tiny fps change past its grace): the new arm waits for
        // the stretch, decides none of its own, and its PI starts after landing + 2
        Lock lock;
        lock.step(frame(at(700, num, den, 6000000), num, den));
        std::vector<Lock::Decision> d(1);
        for (int age = 1; age <= 16; ++age) {
            if (age == 3)
                lock.release(PhaseLockArmReason::Enable);
            Lock::Input in = frame(at(700 + age, num, den, age <= 10 ? 6000000 : 20000), num, den);
            in.stretchLanded = age == 10;
            d.push_back(lock.step(in));
        }
        bool held = true, stretched = false;
        for (int age = 1; age <= 12; ++age) {
            held = held && d[age].holding && !d[age].servoRan;
            stretched = stretched || d[age].stretch;
        }
        CHECK(d[3].rearmed && d[3].armReason == PhaseLockArmReason::Enable && d[3].active, "re-armed at age 3");
        CHECK(held && !stretched && d[13].servoRan && !d[13].holding, "re-armed in the hold: held to 12, no stretch, the PI at 13");
    }
    {
        // a period change re-arms on the new rate and keeps the hold
        Lock lock;
        lock.step(frame(at(800, num, den, 6000000), num, den));
        const Lock::Decision d = lock.step(frame(at(801, 24000, 1000, 6000000), 24000, 1000));
        CHECK(d.rearmed && d.armReason == PhaseLockArmReason::FpsChange && d.holding && !d.stretch, "a rate change keeps the hold");
    }
    {
        // a stream stop: release(Stream) keeps it, the controller's cancel ends it
        Lock lock;
        lock.step(frame(at(900, num, den, 6000000), num, den));
        lock.release(PhaseLockArmReason::Stream);
        CHECK(lock.stretchAge == 0, "release(Stream) keeps the stretch");
        lock.cancelStretch();
        const Lock::Decision d = lock.step(frame(at(901, num, den, 20000), num, den));
        CHECK(d.rearmed && d.armReason == PhaseLockArmReason::Stream && !d.holding && d.servoRan, "after the cancel: a plain arm");
    }
}

// 6. Conditional integration at the clamp.
static void testAntiWindup()
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    const double kp = lock.tuning.proportional, ki = lock.tuning.integral, clamp = lock.tuning.clampUs;
    // +900 us late for 2000 frames (recording, so no stretch; 900 < 1000 anyway): the output goes to the
    // lower clamp and the integral stops where the unclamped output reaches it.
    double maxIntegral = 0.0;
    int64_t k = 0;
    for (; k < 2000; ++k) {
        Lock::Input in = frame(at(500 + k, num, den, 900000), num, den);
        in.recording = true;
        lock.step(in);
        maxIntegral = std::max(maxIntegral, lock.integralUs);
    }
    const double limit = (clamp - kp * 900.0) / ki;  // 64000 us*frames
    CHECK(maxIntegral <= limit + 1e-6 && maxIntegral > limit - 900.0 - 1e-6, "integral %.1f, limit %.1f (shipped bound: %.0f)",
          maxIntegral, limit, clamp / ki);
    // the integral stops one step short, so the output rests just inside the clamp (within ki * 900 us)
    CHECK(lock.requestedUs >= lock.nominalUs - clamp && lock.requestedUs <= lock.nominalUs - clamp + ki * 900.0 + 1e-9,
          "at the clamp: T %+.3f", lock.requestedUs - lock.nominalUs);
    // the error turns: the output leaves the clamp on the first frame and the integral unwinds at once
    Lock::Input in = frame(at(500 + k, num, den, -900000), num, den);
    in.recording = true;
    lock.step(in);
    CHECK(lock.requestedUs > lock.nominalUs - clamp + 1.0, "the first early frame leaves the clamp: %.3f", lock.requestedUs);
    CHECK(lock.integralUs < maxIntegral, "and integrates (%.1f)", lock.integralUs);

    // saturated high from a wound integral: an error that drives the output back is integrated, one that
    // drives it further out is not
    Lock a;
    a.step(frame(at(10, num, den, 0), num, den));  // arm
    a.integralUs = -300000.0;                      // ki * I = -450 us: the output wants T + 450
    Lock::Input late = frame(at(11, num, den, 500000), num, den);
    late.recording = true;
    a.step(late);
    CHECK(a.integralUs == -299500.0, "a late frame (drives it back in) integrates: %.1f", a.integralUs);
    Lock::Input early = frame(at(12, num, den, -500000), num, den);
    early.recording = true;
    a.step(early);
    CHECK(a.integralUs == -299500.0, "an early frame (drives it further out) does not: %.1f", a.integralUs);
    CHECK(a.requestedUs == a.nominalUs + clamp, "still at the upper clamp");
    // inside the deadband nothing moves
    Lock::Input quiet = frame(at(13, num, den, 5000), num, den);
    const double before = a.requestedUs;
    a.step(quiet);
    CHECK(a.integralUs == -299500.0 && a.requestedUs == before, "inside the deadband the request holds");
}

// 7. locked: after 25 frames <= 100 us in a row, false on any frame above, on arm and on release.
static void testLocked()
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    int64_t k = 1000;
    for (int i = 1; i <= 25; ++i) {
        const Lock::Decision d = lock.step(frame(at(k++, num, den, i % 2 ? 100000 : -100000), num, den));
        CHECK(d.locked == (i >= 25), "frame %d at +-100 us: locked %d", i, int(d.locked));
    }
    CHECK(lock.locked, "locked after 25");
    // one frame at 101 us clears it, and the count starts again
    CHECK(!lock.step(frame(at(k++, num, den, 101000), num, den)).locked, "101 us clears locked");
    for (int i = 1; i <= 25; ++i) {
        const Lock::Decision d = lock.step(frame(at(k++, num, den, 50000), num, den));
        CHECK(d.locked == (i >= 25), "again: frame %d locked %d", i, int(d.locked));
    }
    // a locked lock does not stretch on one stray frame 2 ms off; the next frame back on time keeps it unstretched
    const Lock::Decision stray = lock.step(frame(at(k++, num, den, 2000000), num, den));
    CHECK(!stray.stretch && !stray.locked && stray.servoRan, "one stray frame: no stretch, locked cleared");
    const Lock::Decision back = lock.step(frame(at(k++, num, den, 20000), num, den));
    CHECK(!back.stretch, "back on time: no stretch");
    // two frames off in a row: the second stretches (preview)
    for (int i = 0; i < 25; ++i)
        lock.step(frame(at(k++, num, den, 0), num, den));
    CHECK(lock.locked, "locked again");
    CHECK(!lock.step(frame(at(k++, num, den, 2000000), num, den)).stretch, "first frame off: no stretch");
    CHECK(lock.step(frame(at(k++, num, den, 2000000), num, den)).stretch, "second frame off: stretch");
    // release and arm clear it
    Lock r;
    for (int i = 0; i < 30; ++i)
        r.step(frame(at(k++, num, den, 0), num, den));
    CHECK(r.locked, "locked before release");
    r.release();
    CHECK(!r.locked && !r.active, "release clears locked");
    const Lock::Decision rearm = r.step(frame(at(k++, num, den, 0), num, den));
    CHECK(rearm.rearmed && !rearm.locked && r.lockedFrames == 1, "arm starts the count again");
    // disabled frames clear it too
    Lock s;
    for (int i = 0; i < 30; ++i)
        s.step(frame(at(k++, num, den, 0), num, den));
    Lock::Input off = frame(at(k++, num, den, 0), num, den);
    off.enabled = false;
    CHECK(!s.step(off).locked && !s.locked, "a disabled frame clears locked");
}

// 8. arm keeps the grid: no origin, so a re-arm measures the same error, and two locks armed at
//    different frames agree on every slot and error.
static void testArmKeepsGrid()
{
    const int64_t num = 24000, den = 1001;
    Lock lock;
    int64_t k = 77777777;
    for (int i = 0; i < 40; ++i)
        lock.step(frame(at(k++, num, den, 300000), num, den));
    lock.release(PhaseLockArmReason::FpsChange);
    Lock::Input off = frame(at(k++, num, den, 300000), num, den);
    off.enabled = false;
    lock.step(off);
    const Lock::Decision d = lock.step(frame(at(k, num, den, 300000), num, den));
    CHECK(d.rearmed && d.slot == k && std::llabs(d.errorNs - 300000) <= 1, "re-armed: slot %" PRId64 " err %" PRId64
          " (the old arm datum would say 0)", d.slot, d.errorNs);
    // two Cores: one armed 3 frames later, after a frame drop; the same timestamps give the same slot and error
    Lock a, b;
    std::vector<int64_t> ts;
    for (int i = 0; i < 20; ++i)
        if (i != 7)
            ts.push_back(at(5000 + i, num, den, 12345 * (i % 5) - 20000));
    bool same = true;
    for (size_t i = 0; i < ts.size(); ++i) {
        const Lock::Decision da = a.step(frame(ts[i], num, den));
        if (i >= 3) {
            const Lock::Decision db = b.step(frame(ts[i], num, den));
            same = same && da.slot == db.slot && da.errorNs == db.errorNs;
        }
    }
    CHECK(same, "two locks armed apart agree on slot and error");
    // the slot is absolute: the dropped frame 7 leaves slot 5007 out, and frame 8 is slot 5008
    CHECK(Lock::gridPoint(ts[7], num, den).slot == 5008, "a drop skips a slot, no gap counting needed");
}

// 9. Arm reasons.
static void testArmReasons()
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    int64_t k = 10;
    Lock::Decision d = lock.step(frame(at(k++, num, den, 0), num, den));
    CHECK(d.rearmed && d.armReason == PhaseLockArmReason::Stream, "the first arm reports stream");
    CHECK(!lock.step(frame(at(k++, num, den, 0), num, den)).rearmed, "no re-arm while enabled");
    lock.release();
    d = lock.step(frame(at(k++, num, den, 0), num, den));
    CHECK(d.rearmed && d.armReason == PhaseLockArmReason::Enable, "release() reports enable");
    // fps change: release, then the grace keeps it disabled; the reason survives the disabled frames
    lock.release(PhaseLockArmReason::FpsChange);
    for (int i = 0; i < 16; ++i) {
        Lock::Input off = frame(at(k++, 24000, 1000, 0), 24000, 1000);
        off.enabled = false;
        CHECK(!lock.step(off).active, "grace frame inactive");
    }
    d = lock.step(frame(at(k++, 24000, 1000, 0), 24000, 1000));
    CHECK(d.rearmed && d.armReason == PhaseLockArmReason::FpsChange && lock.rateNum == 24000, "fps change after the grace");
    lock.release(PhaseLockArmReason::PhotoModeExit);
    d = lock.step(frame(at(k++, 24000, 1000, 0), 24000, 1000));
    CHECK(d.armReason == PhaseLockArmReason::PhotoModeExit, "photo mode exit");
    lock.release(PhaseLockArmReason::Stream);
    d = lock.step(frame(at(k++, 24000, 1000, 0), 24000, 1000));
    CHECK(d.armReason == PhaseLockArmReason::Stream, "stream restart");
    // a period change the controller did not release for still re-arms, as fps
    d = lock.step(frame(at(k++, 25000, 1000, 0), 25000, 1000));
    CHECK(d.rearmed && d.armReason == PhaseLockArmReason::FpsChange, "period change while armed");
    CHECK(std::string(phaseLockArmReasonName(PhaseLockArmReason::Enable)) == "enable" &&
          std::string(phaseLockArmReasonName(PhaseLockArmReason::FpsChange)) == "fps" &&
          std::string(phaseLockArmReasonName(PhaseLockArmReason::Stream)) == "stream" &&
          std::string(phaseLockArmReasonName(PhaseLockArmReason::PhotoModeExit)) == "photo", "reason names");
    // no period: nothing to measure, inactive
    Lock::Input none = frame(123, 0, 1);
    d = lock.step(none);
    CHECK(!d.active && d.slot == 0 && d.errorNs == 0, "no period: inactive");
    const Lock::GridPoint g0 = Lock::gridPoint(123456789, 0, 1000), g1 = Lock::gridPoint(123456789, 25000, 0);
    CHECK(g0.slot == 0 && g0.errorNs == 0 && g1.slot == 0 && g1.errorNs == 0, "gridPoint without a period: {0, 0}");
}

// 10. Telemetry: pushes count lock pin changes, maxAbsErrorUs only frames the PI ran, resetTelemetry.
static void testTelemetry()
{
    const int64_t num = 25000, den = 1000;
    Lock lock;
    lock.step(frame(at(10, num, den, 8000000), num, den));  // stretch: |e| 8 ms not in max
    CHECK(lock.maxAbsErrorUs == 0.0 && lock.pushes == 1, "the stretch frame: max %.1f pushes %" PRIu64, lock.maxAbsErrorUs, lock.pushes);
    lock.release();
    // the release keeps the stretch in flight: the next frame is a hold frame, not a servo frame
    const Lock::Decision held = lock.step(frame(at(11, num, den, 40000), num, den));
    CHECK(held.holding && !held.servoRan && lock.maxAbsErrorUs == 0.0, "the stretch outlives the release: a hold frame");
    lock.cancelStretch();
    lock.step(frame(at(12, num, den, 40000), num, den));
    CHECK(std::fabs(lock.maxAbsErrorUs - 40.0) < 1e-9, "a servo frame counts: %.1f", lock.maxAbsErrorUs);
    lock.resetTelemetry();
    CHECK(lock.maxAbsErrorUs == 0.0 && lock.pushes == 0, "reset");
    // status-style fields
    PhaseLockStatus status;
    status.locked = lock.locked;
    CHECK(!status.locked && status.errorUs == 0.0, "PhaseLockStatus has locked");
}

// 11. Closed loop against a plant: dead time D, Gaussian timestamp jitter, 1 us timestamps, the sensor's
//     own rate offset, VMAX floored to even, the base pin in flight at arm. Design §7.4/§7.5 and the
//     simulator's C6: after a toggle |e| < 50 us at about frame 13 (D 11), locked at about 36.
struct LoopResult {
    int stretchAt = -1;     // the frame of the first stretch (0 = the arm frame), -1: none (|e| never > 1 ms)
    int firstUnder50 = -1;  // first frame after the first stretch with |true error| < 50 us
    int firstServo = -1;
    int lockedAt = -1;
    int stretches = 0;
    int notSeen = 0;
    double landingUs = 0.0;      // true error of the first frame after the stretched one
    double trueRms = 0.0, trueMax = 0.0, measMax = 0.0, measRms = 0.0;
    double vmaxChangesPer1000 = 0.0;
};

// D: the frames from a request to the frame its pin decides (N + the pin delay); the stretched frame is D frames after
// the stretch, and that frame is the landing input. pinDelay: where the trace marks the echo (D - pinDelay).
// missing: how many of the D frames already decided (the oldest: this frame and the next) the controller
// leaves out of the in-flight sum, e.g. 3 if it counts only the N - 1 queued requests while D = N + 2.
// trace: print frames 0..trace-1 as CC's trace line would (D11).
static LoopResult runLoop(int64_t num, int64_t den, int D, double sigmaUs, double ppm, unsigned seed, int pinDelay,
                          bool countInFlight, int frames, int missing = 0, int trace = 0)
{
    LoopResult r;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> jitter(0.0, sigmaUs * 1000.0);
    std::uniform_real_distribution<double> phase(-0.5, 0.5);
    const double T = periodNs(num, den);
    const int64_t basePinUs = int64_t(1e6 * double(den) / double(num));  // controlsForSettings: trunc(1e6 / fps), {d, d+1}
    const int64_t baseLines = realisedLines(basePinUs, kH, kStep);
    double t = std::round(3.0 * 86400e9 / T) * T + phase(rng) * T;  // 3 days of uptime, a random phase
    std::deque<double> trueDur, believed;  // the D frames already decided: true (with ppm) and as the controller sees them
    for (int i = 0; i < D; ++i) {
        trueDur.push_back(double(baseLines) * kH * 1000.0 * (1.0 + ppm * 1e-6));
        believed.push_back(double(baseLines) * kH * 1000.0);
    }
    Lock lock;
    int64_t lockPin = -1;
    int stretchAt = -1;
    int64_t prevLines = baseLines;
    int changes = 0, counted = 0;
    double sumTrue2 = 0.0, sumMeas2 = 0.0;
    int steady = 0;
    for (int k = 0; k < frames; ++k) {
        const double trueErr = t - std::round(t / T) * T;
        const int64_t ts = int64_t(std::llround((t + jitter(rng)) / 1000.0)) * 1000;  // 1 us timestamps
        double excess = 0.0;
        for (size_t i = size_t(std::max(0, missing)); i < believed.size(); ++i)
            excess += believed[i] - T;
        Lock::Input in = frame(ts, num, den);
        in.inFlightExcessNs = countInFlight ? int64_t(std::llround(excess)) : 0;
        in.stretchLanded = stretchAt >= 0 && k == stretchAt + D;
        const Lock::Decision d = lock.step(in);
        if (d.stretch) {
            ++r.stretches;
            stretchAt = k;
            if (r.stretchAt < 0)
                r.stretchAt = k;
        }
        r.notSeen += d.stretchNotSeen;
        if (d.servoRan && r.firstServo < 0)
            r.firstServo = k;
        if (r.stretches > 0 && r.firstUnder50 < 0 && std::fabs(trueErr) < 50000.0 && k > stretchAt)
            r.firstUnder50 = k;
        if (stretchAt >= 0 && k == stretchAt + D + 1 && r.stretches == 1)
            r.landingUs = trueErr / 1000.0;
        if (d.locked && r.lockedAt < 0)
            r.lockedAt = k;
        if (d.pushDuration)
            lockPin = d.durationUs;
        if (k < trace) {
            const int64_t v = std::llround(trueDur.front() / (kH * 1000.0 * (1.0 + ppm * 1e-6)));
            const bool echo = stretchAt >= 0 && k == stretchAt + D - pinDelay;
            std::printf("    ApertarCore phase-lock trace: slot=%" PRId64 " ts=%" PRId64 " err_ns=%" PRId64 " v=%" PRId64
                        " lock=%d locked=%d%s%s%s%s%s%s%s\n", d.slot, ts, d.errorNs, v, int(d.active), int(d.locked),
                        d.rearmed ? " arm=" : "", d.rearmed ? phaseLockArmReasonName(d.armReason) : "",
                        d.pushDuration ? (" push=" + std::to_string(d.durationUs)).c_str() : "",
                        d.stretch ? (" stretch=" + std::to_string(d.stretchUs)).c_str() : "",
                        echo ? " echo" : "", d.stretchLanded ? " landed" : "", d.holding ? " (held)" : "");
        }
        // the request sent now: the stretch pin once, else the lock pin, else the base pin
        const int64_t pin = d.stretch ? d.stretchUs : lockPin > 0 ? lockPin : basePinUs;
        const int64_t lines = realisedLines(pin, kH, kStep);
        trueDur.push_back(double(lines) * kH * 1000.0 * (1.0 + ppm * 1e-6));
        believed.push_back(double(lines) * kH * 1000.0);
        believed.pop_front();
        const double now = trueDur.front();
        trueDur.pop_front();
        if (k >= 200) {
            const double measErr = double(d.errorNs);
            sumTrue2 += trueErr * trueErr;
            sumMeas2 += measErr * measErr;
            r.trueMax = std::max(r.trueMax, std::fabs(trueErr) / 1000.0);
            r.measMax = std::max(r.measMax, std::fabs(measErr) / 1000.0);
            ++steady;
            const int64_t nowLines = std::llround(now / (kH * 1000.0 * (1.0 + ppm * 1e-6)));
            if (nowLines != prevLines)
                ++changes;
            prevLines = nowLines;
            ++counted;
        }
        t += now;
    }
    r.trueRms = std::sqrt(sumTrue2 / steady) / 1000.0;
    r.measRms = std::sqrt(sumMeas2 / steady) / 1000.0;
    r.vmaxChangesPer1000 = 1000.0 * changes / counted;
    return r;
}

static int median(std::vector<int> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static void testClosedLoop()
{
    struct Mode { const char *name; int64_t num, den; double sigma; };
    const Mode modes[] = { { "25", 25000, 1000, 3.4 }, { "24", 24000, 1000, 4.9 }, { "23.976", 24000, 1001, 4.9 } };
    for (const Mode &m : modes) {
        for (int D : { kN + 1, kN + 2, kN + 3, kN + 4 }) {
            {
                std::vector<int> under50, servo, locked;  // frames after the first stretch
                double worstLanding = 0.0, worstRms = 0.0, worstMax = 0.0, worstMeasMax = 0.0, changes = 0.0;
                int stretchesOver1 = 0, notSeen = 0, noStretch = 0, laterStretch = 0;
                const int seeds = 40;
                for (int s = 0; s < seeds; ++s) {
                    const LoopResult r = runLoop(m.num, m.den, D, m.sigma, 1.5, 1000 + s, D - kN, true, 3000);
                    // |e| <= 1 ms at the arm: the PI alone (no stretch, or one later if the base pins in
                    // flight carry it past 1 ms); counted, not scored for acquisition
                    noStretch += r.stretchAt < 0;
                    laterStretch += r.stretchAt > 0;
                    if (r.stretchAt == 0) {
                        under50.push_back(r.firstUnder50 - r.stretchAt);
                        servo.push_back(r.firstServo - r.stretchAt);
                        locked.push_back(r.lockedAt - r.stretchAt);
                    } else if (r.stretchAt > 0) {
                        CHECK(r.firstUnder50 - r.stretchAt <= D + 3, "%s D %d seed %d: a later stretch acquires too (%d)", m.name, D,
                              1000 + s, r.firstUnder50 - r.stretchAt);
                    }
                    worstLanding = std::max(worstLanding, std::fabs(r.landingUs));
                    worstRms = std::max(worstRms, r.trueRms);
                    worstMax = std::max(worstMax, r.trueMax);
                    worstMeasMax = std::max(worstMeasMax, r.measMax);
                    changes += r.vmaxChangesPer1000 / seeds;
                    stretchesOver1 += r.stretches > 1;
                    notSeen += r.notSeen;
                }
                std::printf("  loop %-6s fps D %d (pin delay %d): after the stretch |e|<50 at frame %d (max %d), PI from %d, locked at %d"
                            " (max %d); landing <= %.1f us; steady true RMS <= %.1f max <= %.1f, measured max <= %.1f us;"
                            " VMAX changes %.0f/1000; runs %d: stretched at arm %zu, later %d, PI only %d; >1 stretch %d, not seen %d\n",
                            m.name, D, D - kN, median(under50), *std::max_element(under50.begin(), under50.end()),
                            median(servo), median(locked), *std::max_element(locked.begin(), locked.end()), worstLanding, worstRms,
                            worstMax, worstMeasMax, changes, seeds, under50.size(), laterStretch, noStretch, stretchesOver1, notSeen);
                CHECK(median(under50) <= D + 2 && *std::max_element(under50.begin(), under50.end()) <= D + 3,
                      "%s D %d: acquired by frame %d", m.name, D, median(under50));
                CHECK(median(servo) == D + 3, "%s D %d: PI resumes at landed + 3 (%d)", m.name, D, median(servo));
                CHECK(median(locked) <= D + 27, "%s D %d: locked at %d", m.name, D, median(locked));
                CHECK(worstLanding <= 30.0, "%s D %d: landing %.1f us", m.name, D, worstLanding);
                CHECK(worstRms <= 15.0 && worstMax <= 60.0, "%s D %d: steady RMS %.1f max %.1f", m.name, D, worstRms, worstMax);
                CHECK(stretchesOver1 == 0 && notSeen == 0, "%s D %d: at most one stretch per run", m.name, D);
            }
        }
    }
    // without the in-flight term at 24 fps the stretch lands about 81 us early (11 base pins of 5624 lines,
    // 7.4 us short each, are still in flight)
    double sum = 0.0, sumWith = 0.0;
    int n = 0;
    for (int s = 0; s < 40; ++s) {
        const LoopResult without = runLoop(24000, 1000, 11, 0.0, 0.0, 2000 + s, 3, false, 40);
        const LoopResult with = runLoop(24000, 1000, 11, 0.0, 0.0, 2000 + s, 3, true, 40);
        if (without.stretchAt == 0 && with.stretchAt == 0) {
            sum += without.landingUs;
            sumWith += with.landingUs;
            ++n;
        }
    }
    std::printf("  24 fps D 11, no jitter, %d runs: mean landing %+.1f us without the in-flight term, %+.1f us with it\n", n,
                sum / n, sumWith / n);
    // the sum over only the N - 1 queued requests, when D = N + 2 (the 3 oldest frames left out)
    double sumMissing = 0.0;
    int m3 = 0;
    for (int s = 0; s < 40; ++s) {
        const LoopResult r = runLoop(24000, 1000, 10, 0.0, 0.0, 2000 + s, 2, true, 40, 3);
        if (r.stretchAt == 0) {
            sumMissing += r.landingUs;
            ++m3;
        }
    }
    std::printf("  24 fps D 10 (N 8 + 2), the sum leaving out the 3 oldest of the 10 frames: mean landing %+.1f us\n",
                sumMissing / m3);
    // one run as CC's trace would print it (25 fps, D 10 = N + 2: the echo on the request's own frame, N after the
    // stretch, and the stretched frame 2 later)
    std::printf("  trace of one run, 25 fps, D 10 (pin delay 2): echo at N, landed at N + 2:\n");
    runLoop(25000, 1000, 10, 3.4, 1.5, 4242, 2, true, 16, 0, 16);
    CHECK(n >= 30 && std::fabs(sum / n + 81.5) < 3.0 && std::fabs(sumWith / n) < 3.0, "in-flight term: %+.1f / %+.1f us", sum / n,
          sumWith / n);
}

// 12. A take's own phase: decided on the take's first frame outside a hold and on every arm in the take. More than
//     max(1 ms, deadband) off, the PI holds that error (the frames stay at T), locked stays false and the error
//     reported stays the grid's; the offset clears when the take ends. The storage pre-roll's take keeps none.
static void testTakePhase()
{
    const int64_t num = 25000, den = 1000;
    {
        // an arm on the take's first frame, 5 ms late
        Lock lock;
        Lock::Input in = frame(at(1000, num, den, 5000000), num, den);
        in.recording = true;
        const Lock::Decision d = lock.step(in);
        CHECK(d.rearmed && d.takePhaseKept && lock.takeOffsetNs == 5000000 && !d.locked && !d.stretch && d.servoRan,
              "an arm in a take: the phase kept");
        CHECK(lock.requestedUs == lock.nominalUs && lock.integralUs == 0.0 &&
              d.durationUs == Lock::pinFor(lock.nominalUs, kH, kStep).us, "the PI and Q start from T (pin %" PRId64 ")", d.durationUs);
        CHECK(std::fabs(d.phaseErrorUs - 5000.0) < 1e-9, "err stays the grid error (%.1f)", d.phaseErrorUs);
        // 100 frames within the deadband of the kept phase: the PI holds T, never locked, decided once
        bool locked = false, again = false, moved = false;
        for (int k = 1; k <= 100; ++k) {
            in = frame(at(1000 + k, num, den, 5000000 + (k % 3 - 1) * 4000), num, den);
            in.recording = true;
            const Lock::Decision x = lock.step(in);
            locked = locked || x.locked;
            again = again || x.takePhaseKept;
            moved = moved || lock.requestedUs != lock.nominalUs;
        }
        CHECK(!locked && !again && !moved, "at its phase: never locked, decided once, the PI at T");
        // 50 us after its phase: the PI acts on 50 us, not on 5.05 ms
        in = frame(at(1101, num, den, 5050000), num, den);
        in.recording = true;
        lock.step(in);
        CHECK(std::fabs(lock.requestedUs - (lock.nominalUs - (0.06 * 50.0 + 0.0015 * 50.0))) < 1e-6, "the PI acts on e - offset: %+.4f",
              lock.requestedUs - lock.nominalUs);
        // the take ends: the offset clears and the preview stretches back to the grid
        in = frame(at(1102, num, den, 5000000), num, den);
        const Lock::Decision after = lock.step(in);
        CHECK(lock.takeOffsetNs == 0 && after.stretch, "take over: the offset clears, the preview stretches to the grid");
    }
    {
        // the take starts inside a stretch's hold (its stretch never lands): decided on its first frame after it
        Lock lock;
        CHECK(lock.step(frame(at(2000, num, den, 6000000), num, den)).stretch, "a preview stretch");
        int keptAt = -1, kept = 0;
        for (int age = 1; age <= 20; ++age) {
            Lock::Input in = frame(at(2000 + age, num, den, 6000000), num, den);
            in.recording = age >= 3;
            if (lock.step(in).takePhaseKept) {
                keptAt = age;
                ++kept;
            }
        }
        CHECK(kept == 1 && keptAt == kN + 7, "a take that started in the hold keeps its phase on its first frame after it (%d)", keptAt);
    }
    {
        // a take on the grid keeps no phase of its own, and locks
        Lock lock;
        bool kept = false, locked = false;
        for (int k = 0; k < 40; ++k) {
            Lock::Input in = frame(at(3000 + k, num, den, 30000), num, den);
            in.recording = true;
            const Lock::Decision x = lock.step(in);
            kept = kept || x.takePhaseKept;
            locked = locked || x.locked;
        }
        CHECK(!kept && locked && lock.takeOffsetNs == 0, "a take on the grid: no kept phase, locked");
        // every arm in the take decides again: released and re-armed 3 ms early, it keeps the new error
        lock.release();
        Lock::Input in = frame(at(3040, num, den, -3000000), num, den);
        in.recording = true;
        const Lock::Decision d = lock.step(in);
        CHECK(d.rearmed && d.takePhaseKept && lock.takeOffsetNs == -3000000 && !d.locked, "an arm in the take decides again");
    }
    {
        // 999 us off: no kept phase, the PI pulls it in
        Lock lock;
        Lock::Input in = frame(at(5000, num, den, 999000), num, den);
        in.recording = true;
        const Lock::Decision d = lock.step(in);
        CHECK(!d.takePhaseKept && lock.takeOffsetNs == 0 && lock.requestedUs < lock.nominalUs, "999 us: no kept phase");
    }
    {
        // the storage pre-roll's take, and a meter (deadband 100000, test 1.1's raw drift): no kept phase
        Lock lock;
        Lock::Input in = frame(at(6000, num, den, 5000000), num, den);
        in.recording = true;
        in.prerollPinned = true;
        in.stretchAllowed = false;
        CHECK(!lock.step(in).takePhaseKept && lock.takeOffsetNs == 0, "the pre-roll take keeps no phase");
        Lock meter;
        meter.tuning.deadbandUs = 100000.0;
        in = frame(at(7000, num, den, 15000000), num, den);
        in.recording = true;
        CHECK(!meter.step(in).takePhaseKept && meter.takeOffsetNs == 0 && meter.requestedUs == meter.nominalUs, "a meter keeps no phase");
    }
    {
        // the servo error wraps: kept at +19.9 ms, a frame at -19.95 ms is 150 us after the kept phase
        Lock lock;
        Lock::Input in = frame(at(8000, num, den, 19900000), num, den);
        in.recording = true;
        lock.step(in);
        in = frame(at(8002, num, den, -19950000), num, den);
        in.recording = true;
        lock.step(in);
        CHECK(std::fabs(lock.requestedUs - (lock.nominalUs - (0.06 * 150.0 + 0.0015 * 150.0))) < 1e-6, "wrapped: %+.4f",
              lock.requestedUs - lock.nominalUs);
    }
    {
        // the wrap the other way: kept at -19.9 ms, a frame at +19.95 ms is 150 us before the kept phase
        Lock lock;
        Lock::Input in = frame(at(8100, num, den, -19900000), num, den);
        in.recording = true;
        lock.step(in);
        in = frame(at(8102, num, den, 19950000), num, den);
        in.recording = true;
        lock.step(in);
        CHECK(std::fabs(lock.requestedUs - (lock.nominalUs + (0.06 * 150.0 + 0.0015 * 150.0))) < 1e-6, "wrapped up: %+.4f",
              lock.requestedUs - lock.nominalUs);
    }
    {
        // the wrap happens at T/2, not at T: a servo error of +-30 ms is 10 ms the other way round, so the PI
        // saturates on the other side
        Lock up;
        Lock::Input in = frame(at(8200, num, den, -15000000), num, den);
        in.recording = true;
        up.step(in);
        in = frame(at(8201, num, den, 15000000), num, den);
        in.recording = true;
        up.step(in);
        CHECK(up.requestedUs == up.nominalUs + up.tuning.clampUs, "kept -15 ms, frame +15 ms: wrapped to -10 ms (%+.4f)",
              up.requestedUs - up.nominalUs);
        Lock down;
        in = frame(at(8300, num, den, 15000000), num, den);
        in.recording = true;
        down.step(in);
        in = frame(at(8301, num, den, -15000000), num, den);
        in.recording = true;
        down.step(in);
        CHECK(down.requestedUs == down.nominalUs - down.tuning.clampUs, "kept +15 ms, frame -15 ms: wrapped to +10 ms (%+.4f)",
              down.requestedUs - down.nominalUs);
    }
    {
        // an arm in the take that finds the lock within 1 ms of the grid leaves nothing of the old phase: the PI
        // then acts on the error itself, not on its distance from a phase the take no longer has
        Lock lock;
        Lock::Input in = frame(at(9000, num, den, 5000000), num, den);
        in.recording = true;
        lock.step(in);
        CHECK(lock.takeOffsetNs == 5000000, "kept 5 ms");
        lock.release();
        in = frame(at(9001, num, den, 500000), num, den);
        in.recording = true;
        const Lock::Decision d = lock.step(in);
        CHECK(d.rearmed && !d.takePhaseKept && lock.takeOffsetNs == 0, "an arm 0.5 ms off: the old phase is gone (%" PRId64 ")",
              lock.takeOffsetNs);
        CHECK(lock.requestedUs < lock.nominalUs - 20.0, "the PI acts on 500 us (%+.4f)", lock.requestedUs - lock.nominalUs);
    }
    {
        // locked needs the take to have no phase of its own: frames that come back to the grid under a kept phase
        // (a clock step) are not locked, however many
        Lock lock;
        Lock::Input in = frame(at(10000, num, den, 5000000), num, den);
        in.recording = true;
        lock.step(in);
        bool locked = false;
        for (int k = 1; k <= 60; ++k) {
            in = frame(at(10000 + k, num, den, 30000), num, den);
            in.recording = true;
            const Lock::Decision x = lock.step(in);
            locked = locked || x.locked || lock.locked;
        }
        CHECK(!locked && lock.takeOffsetNs == 5000000 && lock.lockedFrames == Lock::kLockedAfterFrames,
              "60 frames within 100 us under a kept phase: not locked (%d frames counted)", lock.lockedFrames);
    }
    {
        // a lock that was locked before the take waits a frame: one stray timestamp keeps no phase
        auto lockedLock = [&](Lock &lock) {
            for (int k = 0; k < 40; ++k)
                lock.step(frame(at(11000 + k, num, den, 30000), num, den));
            return lock.locked;
        };
        auto takeFrame = [&](Lock &lock, int k, int64_t errorNs) {
            Lock::Input in = frame(at(11040 + k, num, den, errorNs), num, den);
            in.recording = true;
            return lock.step(in);
        };
        {
            Lock lock;
            CHECK(lockedLock(lock), "locked before the take");
            const Lock::Decision a = takeFrame(lock, 0, 3000000);
            const Lock::Decision b = takeFrame(lock, 1, 30000);
            const Lock::Decision c = takeFrame(lock, 2, 3000000);
            CHECK(!a.takePhaseKept && !b.takePhaseKept && !c.takePhaseKept && lock.takeOffsetNs == 0 && !lock.takePhaseDue,
                  "one stray frame (+3 ms) on a locked lock: no kept phase, and the next frame decided");
            CHECK(!c.stretch, "a stray frame in a take is no stretch");
        }
        {
            Lock lock;
            lockedLock(lock);
            const Lock::Decision a = takeFrame(lock, 0, 3000000);
            const Lock::Decision b = takeFrame(lock, 1, 3100000);
            CHECK(!a.takePhaseKept && b.takePhaseKept && lock.takeOffsetNs == 3100000 && !b.locked,
                  "two frames in a row past 1 ms on the same side: the second keeps its phase (%" PRId64 ")", lock.takeOffsetNs);
        }
        {
            Lock lock;
            lockedLock(lock);
            const Lock::Decision a = takeFrame(lock, 0, 3000000);
            const Lock::Decision b = takeFrame(lock, 1, -3000000);
            const Lock::Decision c = takeFrame(lock, 2, -3000000);
            CHECK(!a.takePhaseKept && !b.takePhaseKept && c.takePhaseKept && lock.takeOffsetNs == -3000000,
                  "opposite sides do not count: kept on the second frame of the new side");
        }
        {
            // a lock that was not locked decides at once, as before
            Lock lock;
            lock.step(frame(at(11000, num, den, 30000), num, den));
            const Lock::Decision a = takeFrame(lock, 0, 3000000);
            CHECK(a.takePhaseKept && lock.takeOffsetNs == 3000000, "not locked before the take: kept on the first frame");
        }
        {
            // the wait does not leak into the next take
            Lock lock;
            lockedLock(lock);
            takeFrame(lock, 0, 3000000);
            lock.step(frame(at(11100, num, den, 30000), num, den));
            CHECK(lock.takeStraySign == 0 && lock.takeOffsetNs == 0, "the take ends: nothing waits");
        }
    }
}

// 5c. Three stretches in a row that are never seen end the stretches until the next arm; the PI pulls in alone.
//     A stretch that lands starts the count again.
struct StretchRun {
    int stretches = 0;
    int notSeen = 0;
    int stopFrames = 0;
    int stoppedAtStretch = -1;
    int afterStopStretches = 0;
    int afterStopServo = 0;
};

// Preview 5 ms off the grid for `frames` frames; stretch number n (from 0) lands on its frame of age 3 when
// landing[n] is true, and is never seen otherwise.
static StretchRun driveStretches(Lock &lock, int64_t base, int frames, const std::vector<bool> &landing)
{
    const int64_t num = 25000, den = 1000;
    StretchRun run;
    bool stopped = false;
    for (int k = 0; k < frames; ++k) {
        Lock::Input in = frame(at(base + k, num, den, 5000000), num, den);
        const int index = run.stretches - 1;   // the stretch in flight, when one is
        in.stretchLanded = lock.stretchAge >= 0 && lock.stretchAge + 1 == 3 && index >= 0 &&
            index < int(landing.size()) && landing[size_t(index)];
        const Lock::Decision d = lock.step(in);
        if (d.stretch) ++run.stretches;
        if (d.stretchNotSeen) ++run.notSeen;
        if (d.stretchesStopped) {
            ++run.stopFrames;
            run.stoppedAtStretch = run.stretches;
            stopped = true;
        } else if (stopped) {
            if (d.stretch) ++run.afterStopStretches;
            if (d.servoRan) ++run.afterStopServo;
        }
    }
    return run;
}

static void testStretchesStop()
{
    {
        Lock lock;
        const StretchRun run = driveStretches(lock, 20000, 300, {});
        CHECK(run.stretches == Lock::kStopStretchesAfterNotSeen && run.notSeen == Lock::kStopStretchesAfterNotSeen &&
              run.stopFrames == 1 && run.stoppedAtStretch == 3,
              "never seen: %d stretches, %d not seen, stopped once at stretch %d", run.stretches, run.notSeen, run.stoppedAtStretch);
        CHECK(run.afterStopStretches == 0 && run.afterStopServo > 100 && lock.stretchesStopped && lock.stretchAge < 0,
              "after the third: no stretch, the PI runs alone (%d frames)", run.afterStopServo);
        // an arm starts it again, with the count at zero
        lock.release();
        const StretchRun again = driveStretches(lock, 30000, 300, {});
        CHECK(again.stretches == 3 && again.stopFrames == 1, "after an arm: stretches again (%d), stopped again after three", again.stretches);
    }
    {
        // two not seen, a landed one, two not seen: the count restarted; the third in a row then stops them
        Lock lock;
        const StretchRun run = driveStretches(lock, 40000, 400, {false, false, true, false, false});
        CHECK(run.stoppedAtStretch == 6 && run.stretches == 6 && run.notSeen == 5,
              "a landed stretch resets the count: stopped at stretch %d, %d stretches, %d not seen", run.stoppedAtStretch,
              run.stretches, run.notSeen);
    }
    {
        // two not seen are counted; the arm after a release clears them
        Lock partial;
        const StretchRun two = driveStretches(partial, 60000, 14 * 2 + 4, {});
        CHECK(two.notSeen == 2 && !partial.stretchesStopped, "two not seen: not stopped yet");
        partial.release();
        partial.cancelStretch();
        const Lock::Decision d = partial.step(frame(at(60500, 25000, 1000, 5000000), 25000, 1000));
        CHECK(d.rearmed && partial.stretchesNotSeen == 0 && d.stretch, "an arm clears the count");
    }
}

// 13. A take whose lock arms on its first frame more than 1 ms off the grid, against the plant of runLoop: no
//     stretch and no lock in the take, the frames within Q's dither of T, the sound offset (frame time - (first frame
//     + k T)) within the drift of the base frames in flight, no hole in the frame index CC derives from the time;
//     after the take the preview stretches back to the grid and locks.
struct TakeResult {
    int kept = 0;
    double offsetUs = 0.0;
    bool lockedInTake = false;
    int stretchesInTake = 0;
    double maxPeriodDevUs = 0.0;
    double maxSoundUs = 0.0;
    double lateSoundUs = 0.0;   // from the take's frame 200 on
    int indexJumps = 0;
    int afterUnder50 = -1;      // frames from the take's end to |true e| < 50 us
    bool lockedAfter = false;
};

static TakeResult runTake(int64_t num, int64_t den, int D, double sigmaUs, double ppm, double phaseFrac, unsigned seed,
                          int takeFrames, int afterFrames)
{
    TakeResult r;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> jitter(0.0, std::max(1e-9, sigmaUs * 1000.0));
    const double T = periodNs(num, den);
    const int64_t basePinUs = int64_t(1e6 * double(den) / double(num));
    const int64_t baseLines = realisedLines(basePinUs, kH, kStep);
    double t = std::round(3.0 * 86400e9 / T) * T + phaseFrac * T;
    std::deque<double> trueDur, believed;
    for (int i = 0; i < D; ++i) {
        trueDur.push_back(double(baseLines) * kH * 1000.0 * (1.0 + ppm * 1e-6));
        believed.push_back(double(baseLines) * kH * 1000.0);
    }
    Lock lock;
    int64_t lockPin = -1;
    int stretchAt = -1;
    const double t0 = t;
    int64_t firstUs = 0, lastIndex = -1;
    for (int k = 0; k < takeFrames + afterFrames; ++k) {
        const bool recording = k < takeFrames;
        const double trueErr = t - std::round(t / T) * T;
        const int64_t ts = int64_t(std::llround((t + (sigmaUs > 0.0 ? jitter(rng) : 0.0)) / 1000.0)) * 1000;
        double excess = 0.0;
        for (double b : believed)
            excess += b - T;
        Lock::Input in = frame(ts, num, den);
        in.recording = recording;
        in.inFlightExcessNs = int64_t(std::llround(excess));
        in.stretchLanded = stretchAt >= 0 && k == stretchAt + D;
        const Lock::Decision d = lock.step(in);
        if (d.stretch)
            stretchAt = k;
        if (recording) {
            r.kept += d.takePhaseKept;
            if (d.takePhaseKept)
                r.offsetUs = d.phaseErrorUs;
            r.lockedInTake = r.lockedInTake || d.locked;
            r.stretchesInTake += d.stretch;
            const double soundUs = std::fabs((t - t0) - double(k) * T) / 1000.0;
            r.maxSoundUs = std::max(r.maxSoundUs, soundUs);
            if (k >= 200)
                r.lateSoundUs = std::max(r.lateSoundUs, soundUs);
            // CC's raw frame index: max(frames so far, llround((ts - first) * fps))
            if (k == 0)
                firstUs = ts / 1000;
            const int64_t timeIndex = std::llround(double(ts / 1000 - firstUs) * double(num) / double(den) / 1e6);
            const int64_t index = std::max<int64_t>(k, timeIndex);
            if (k > 0 && index - lastIndex >= 2)
                ++r.indexJumps;
            lastIndex = index;
        } else {
            if (r.afterUnder50 < 0 && stretchAt >= takeFrames && k > stretchAt && std::fabs(trueErr) < 50000.0)
                r.afterUnder50 = k - takeFrames;
            r.lockedAfter = r.lockedAfter || d.locked;
        }
        if (d.pushDuration)
            lockPin = d.durationUs;
        const int64_t pin = d.stretch ? d.stretchUs : lockPin > 0 ? lockPin : basePinUs;
        const int64_t lines = realisedLines(pin, kH, kStep);
        trueDur.push_back(double(lines) * kH * 1000.0 * (1.0 + ppm * 1e-6));
        believed.push_back(double(lines) * kH * 1000.0);
        believed.pop_front();
        const double now = trueDur.front();
        trueDur.pop_front();
        if (recording)
            r.maxPeriodDevUs = std::max(r.maxPeriodDevUs, std::fabs(now - T) / 1000.0);
        t += now;
    }
    return r;
}

static void testTakeLoop()
{
    struct Mode { const char *name; int64_t num, den; double sigma; };
    const Mode modes[] = { { "25", 25000, 1000, 3.4 }, { "24", 24000, 1000, 4.9 }, { "23.976", 24000, 1001, 4.9 } };
    const int D = kN + 2;
    for (const Mode &m : modes) {
        const double T = periodNs(m.num, m.den);
        const int64_t baseLines = realisedLines(int64_t(1e6 * double(m.den) / double(m.num)), kH, kStep);
        // the base frames in flight at the arm drift by this before the lock's own pins act
        const double baseDriftUs = std::fabs(double(D) * (double(baseLines) * kH * 1000.0 - T)) / 1000.0;
        const double stepUs = kStep * kH;
        for (const double sigma : { 0.0, m.sigma }) {
            int runs = 0, kept = 0, locked = 0, stretched = 0, jumps = 0, after = 0, afterLocked = 0;
            double maxDev = 0.0, maxSound = 0.0, lateSound = 0.0;
            for (int s = 0; s < 40; ++s) {
                const double phase = -0.45 + 0.9 * (s + 0.5) / 40.0;
                if (std::fabs(phase * T) <= 1.5e6)
                    continue;
                const TakeResult r = runTake(m.num, m.den, D, sigma, 1.5, phase, 3000 + s, 1500, 200);
                ++runs;
                kept += r.kept == 1 && std::fabs(r.offsetUs) > 1000.0;
                locked += r.lockedInTake;
                stretched += r.stretchesInTake > 0;
                jumps += r.indexJumps;
                after += r.afterUnder50 >= 0 && r.afterUnder50 <= D + 3;
                afterLocked += r.lockedAfter;
                maxDev = std::max(maxDev, r.maxPeriodDevUs);
                maxSound = std::max(maxSound, r.maxSoundUs);
                lateSound = std::max(lateSound, r.lateSoundUs);
            }
            std::printf("  take arming off the grid, %-6s fps, jitter %.1f us, %d runs: kept %d, locked in take %d, stretched %d;"
                        " |period - T| <= %.1f us, sound offset <= %.1f us (base drift %.1f), from frame 200 <= %.1f us;"
                        " index jumps %d; back on the grid within D + 3 after the take %d, locked %d\n", m.name, sigma, runs, kept,
                        locked, stretched, maxDev, maxSound, baseDriftUs, lateSound, jumps, after, afterLocked);
            CHECK(runs > 30 && kept == runs && locked == 0 && stretched == 0, "%s: every take keeps its phase, never locks or stretches",
                  m.name);
            CHECK(maxDev <= stepUs + 1.0, "%s jitter %.1f: frames within Q's dither of T (%.1f us)", m.name, sigma, maxDev);
            // the sound offset: the drift of the base frames in flight at the arm, then the shipped PI's own wander
            // around the kept phase (its deadband against Q's dither and the sensor's ppm), no more than on the grid
            // (testClosedLoop's steady state, at most 60 us)
            CHECK(maxSound <= baseDriftUs + 60.0 && lateSound <= 60.0, "%s jitter %.1f: sound offset %.1f us, %.1f from frame 200",
                  m.name, sigma, maxSound, lateSound);
            CHECK(jumps == 0, "%s: %d jumps in the frame index", m.name, jumps);
            CHECK(after == runs && afterLocked == runs, "%s: after the take the preview stretches back to the grid and locks", m.name);
        }
    }
}

int main()
{
    testGridOracle();
    testGridSweep();
    testQuantiser();
    testStretchSize();
    testStretchStandsOut();
    testHold();
    testNoStretch();
    testCancel();
    testInFlight();
    testAntiWindup();
    testLocked();
    testArmKeepsGrid();
    testArmReasons();
    testTelemetry();
    testTakePhase();
    testStretchesStop();
    testClosedLoop();
    testTakeLoop();
    if (failures == 0)
        std::printf("ALL %d CHECKS PASSED\n", checks);
    else
        std::printf("%d of %d CHECKS FAILED\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
