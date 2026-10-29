#pragma once
// What a GPIO chip says about itself and its lines: name, label, and who holds each line. Read-only: the kernel's
// chip-info and line-info calls never request or change a line, so this is safe while Core's inputs run.
#include <string>
#include <vector>

namespace apertar::hardware {

struct GpioLineInfo {
    int offset = -1;
    std::string name;       // "GPIO10"
    std::string consumer;   // who holds it: "apertar-gpio-input", a kernel driver, or empty
    bool used = false;
    bool output = false;
};

struct GpioChipInfo {
    bool ok = false;
    std::string path;       // /dev/gpiochip0
    std::string name;       // gpiochip0
    std::string label;      // pinctrl-rp1
    int lines = 0;
    std::vector<GpioLineInfo> lineInfo;   // offsets 0 .. maxLines-1
    std::string error;
};

// How well a chip fits as the 40-pin header: RP1 (Pi 5) first, then the BCM pin controllers. -1 when it cannot be opened.
int gpioChipScore(const std::string &chipPath);

// The chip "chip=auto" means: the best scoring /dev/gpiochip0..9, or "" when there is none.
std::string headerGpioChipPath();

GpioChipInfo readGpioChipInfo(const std::string &chipPath, int maxLines);

} // namespace apertar::hardware
