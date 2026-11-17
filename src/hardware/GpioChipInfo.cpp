#include "hardware/GpioChipInfo.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace apertar::hardware {

namespace {

std::string lowered(const char *text)
{
    std::string out(text);
    for (char &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-' || ch == ' ' || ch == '.')
            ch = '_';
    }
    return out;
}

} // namespace

int gpioChipScore(const std::string &chipPath)
{
    const int fd = ::open(chipPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    gpiochip_info info{};
    int score = 0;
    if (::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
        const std::string combined = lowered(info.name) + " " + lowered(info.label);
        if (combined.find("rp1") != std::string::npos)
            score += 100;
        if (combined.find("pinctrl_bcm") != std::string::npos ||
            combined.find("bcm2711") != std::string::npos ||
            combined.find("bcm2835") != std::string::npos)
            score += 90;
        if (combined.find("raspberrypi_exp") != std::string::npos)
            score -= 50;
        if (info.lines >= 28)
            score += 10;
    }

    ::close(fd);
    return score;
}

std::string headerGpioChipPath()
{
    std::string best;
    int bestScore = -1;
    for (int chip = 0; chip <= 9; ++chip) {
        const std::string path = "/dev/gpiochip" + std::to_string(chip);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            continue;
        if (const int score = gpioChipScore(path); score > bestScore) {
            bestScore = score;
            best = path;
        }
    }
    return best;
}

GpioChipInfo readGpioChipInfo(const std::string &chipPath, int maxLines)
{
    GpioChipInfo out;
    out.path = chipPath;
    const int fd = ::open(chipPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out.error = "Cannot open " + chipPath + ": " + std::strerror(errno);
        return out;
    }
    gpiochip_info chip{};
    if (::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) != 0) {
        out.error = "Cannot read " + chipPath + ": " + std::strerror(errno);
        ::close(fd);
        return out;
    }
    out.ok = true;
    out.name = chip.name;
    out.label = chip.label;
    out.lines = static_cast<int>(chip.lines);
    for (int offset = 0; offset < std::min(maxLines, out.lines); ++offset) {
        gpio_v2_line_info line{};
        line.offset = static_cast<__u32>(offset);
        if (::ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) != 0)
            continue;
        GpioLineInfo info;
        info.offset = offset;
        info.name = line.name;
        info.consumer = line.consumer;
        info.used = (line.flags & GPIO_V2_LINE_FLAG_USED) != 0;
        info.output = (line.flags & GPIO_V2_LINE_FLAG_OUTPUT) != 0;
        out.lineInfo.push_back(std::move(info));
    }
    ::close(fd);
    return out;
}

std::vector<GpioChipInfo> listGpioChips()
{
    std::vector<int> numbers;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 8 && name.rfind("gpiochip", 0) == 0 &&
            std::all_of(name.begin() + 8, name.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            numbers.push_back(std::stoi(name.substr(8)));
    }
    std::sort(numbers.begin(), numbers.end());

    std::vector<GpioChipInfo> chips;
    for (const int number : numbers) {
        GpioChipInfo chip = readGpioChipInfo("/dev/gpiochip" + std::to_string(number), 0);
        if (chip.ok)
            chips.push_back(std::move(chip));
    }
    return chips;
}

} // namespace apertar::hardware
