// Host test for src/util/BufferBudget.hpp (the "ram" queue budget and the safety rule).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src tools/buffer_budget_test.cpp -o /tmp/bbt && /tmp/bbt
#include "util/BufferBudget.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>

using namespace apertar;
using apertar::bufferbudget::kMiB;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static constexpr uint64_t GB = 1000ull * 1000ull * 1000ull;
static constexpr uint64_t kFrame4k = 16900000ull;     // 4K 16-bit container
static constexpr uint64_t kFrame1080 = 4147200ull;    // 1920x1080 16-bit... close to 4.1 MB

static BufferPlanInputs board(uint64_t availBytes, uint64_t totalBytes, uint64_t frameBytes, uint64_t frames = 12)
{
    BufferPlanInputs in;
    in.memAvailableBytes = availBytes;
    in.memTotalBytes = totalBytes;
    in.frameBytes = frameBytes;
    in.framesModeFrames = frames;
    return in;
}

int main()
{
    {
        const BufferPlan p = planBuffer(board(3200ull * 1000000ull, 4 * GB, kFrame4k));
        std::printf("      %s\n", p.reason.c_str());
        check(p.ram, "4 GB Pi 5, 4K 16-bit: ram");
        check(p.budgetBytes > 1300ull * 1000000ull && p.budgetBytes < 1500ull * 1000000ull, "4 GB: budget about 1.4 GB");
        const uint64_t frames = p.budgetBytes / kFrame4k;
        check(frames >= 80 && frames <= 86, "4 GB: about 83 frames");
    }
    {
        const BufferPlan p = planBuffer(board(7900ull * 1000000ull, 8 * GB, kFrame4k));
        std::printf("      %s\n", p.reason.c_str());
        check(p.ram && p.budgetBytes == 4 * GB, "8 GB board (7.9 GB free): the 0.5 ceiling binds (4 GB)");
    }
    {
        const BufferPlan p = planBuffer(board(700ull * 1000000ull, 1 * GB, kFrame4k));
        std::printf("      %s\n", p.reason.c_str());
        check(!p.ram && p.reason.find("staying on frames") != std::string::npos, "1 GB board, 4K: refused");
        check(p.reason.find("below the frames-mode 193 MiB") != std::string::npos, "1 GB, 4K: reason names the frames-mode MiB");
    }
    {
        const BufferPlan p = planBuffer(board(700ull * 1000000ull, 1 * GB, kFrame1080));
        std::printf("      %s\n", p.reason.c_str());
        check(!p.ram, "1 GB board, 1080p: refused (budget 0 MiB..., floor)");
    }
    {
        const BufferPlan p = planBuffer(board(900ull * 1000000ull, 1 * GB, kFrame1080));
        std::printf("      %s\n", p.reason.c_str());
        check(!p.ram && p.budgetBytes > 0, "1 GB board with 900 MB free, 1080p: budget 2 MiB < 47 MiB, refused");
    }
    {
        const BufferPlan p = planBuffer(board(380ull * 1000000ull, 512ull * 1000000ull, kFrame4k));
        std::printf("      %s\n", p.reason.c_str());
        check(!p.ram && p.reason.find("ram budget 0 MiB") != std::string::npos, "512 MB Pi Zero, 4K: refused, budget 0");
    }
    {
        const BufferPlan p = planBuffer(board(380ull * 1000000ull, 512ull * 1000000ull, kFrame1080));
        std::printf("      %s\n", p.reason.c_str());
        check(!p.ram, "512 MB Pi Zero, 1080p: refused");
    }
    {
        // 1080p on a roomy board is still ram; the rule compares like with like.
        const BufferPlan p = planBuffer(board(3200ull * 1000000ull, 4 * GB, kFrame1080));
        check(p.ram && p.budgetBytes / kFrame1080 > 12, "4 GB, 1080p: ram, more than the 12 frames");
    }
    {
        const BufferPlan p = planBuffer(board(300ull * 1000000ull, 4 * GB, kFrame4k));
        check(!p.ram && p.budgetBytes == 0, "MemAvailable below the floor: budget 0, refused");
        const BufferPlan z = planBuffer(board(0, 0, kFrame4k));
        check(!z.ram && z.budgetBytes == 0, "MemAvailable 0 and MemTotal 0: refused");
    }
    {
        BufferPlanInputs in = board(3200ull * 1000000ull, 4 * GB, kFrame4k);
        in.fraction = 7.0;       // -> 0.9
        in.maxFraction = 9.0;    // -> 0.75
        in.floorMb = ~0ull;      // clamped, no overflow
        const BufferPlan big = planBuffer(in);
        check(!big.ram && big.budgetBytes == 0, "absurd floor: clamped, budget 0, no overflow");
        in.floorMb = 512;
        const BufferPlan hi = planBuffer(in);
        check(hi.ram && hi.budgetBytes <= 3200ull * 1000000ull * 9 / 10, "fraction above range clamps to 0.9");
        in.fraction = -1.0;      // -> 0.05
        in.maxFraction = 0.0;    // -> 0.1
        const BufferPlan lo = planBuffer(in);
        check(!lo.ram, "fractions below range clamp up, tiny budget refused");
        in.fraction = std::numeric_limits<double>::quiet_NaN();
        in.maxFraction = std::numeric_limits<double>::infinity();
        const BufferPlan nan = planBuffer(in);
        check(nan.ram && nan.budgetBytes > 0 && nan.budgetBytes <= 2 * GB, "NaN/inf fractions fall back to 0.6/0.5");
    }
    {
        const BufferPlan p = planBuffer(board(3200ull * 1000000ull, 4 * GB, ~0ull / 2));
        check(!p.ram, "huge frame size: no overflow, refused");
        const BufferPlan q = planBuffer(board(3200ull * 1000000ull, 4 * GB, ~0ull, ~0ull));
        check(!q.ram, "huge frame size and frame count: saturates, refused");
    }
    {
        const BufferPlan p = planBuffer(board(3200ull * 1000000ull, 4 * GB, kFrame4k, 0));
        check(p.ram && p.budgetBytes > 0, "framesModeFrames 0: ram still allowed when it holds one frame");
        const BufferPlan z = planBuffer(board(300ull * 1000000ull, 4 * GB, kFrame4k, 0));
        check(!z.ram, "framesModeFrames 0 and budget 0: refused");
        const BufferPlan u = planBuffer(board(3200ull * 1000000ull, 4 * GB, 0));
        check(!u.ram, "frame size 0 (unknown): frames");
    }
    {
        check(bufferbudget::parseDouble("0.7", 0.6) == 0.7, "parseDouble reads 0.7");
        check(bufferbudget::parseDouble("abc", 0.6) == 0.6, "parseDouble: junk gives the fallback");
        check(bufferbudget::parseDouble(nullptr, 0.6) == 0.6, "parseDouble: null gives the fallback");
        const char *path = "/tmp/bbt_meminfo.txt";
        { std::ofstream f(path); f << "MemTotal:        4046832 kB\nMemFree: 1 kB\nMemAvailable:    3300000 kB\n"; }
        check(bufferbudget::readMemInfoBytes("MemTotal", path) == 4046832ull * 1024, "meminfo MemTotal");
        check(bufferbudget::readMemInfoBytes("MemAvailable", path) == 3300000ull * 1024, "meminfo MemAvailable");
        check(bufferbudget::readMemInfoBytes("Nope", path) == 0, "meminfo missing key is 0");
        std::remove(path);
    }
    std::printf("%s\n", g_failed ? "FAILED" : "all ok");
    return g_failed ? 1 : 0;
}
