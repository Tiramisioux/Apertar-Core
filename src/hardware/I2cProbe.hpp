#pragma once
// Is something answering at this I2C address? One read of one byte, the way i2cdetect -r checks. Only for addresses
// no Core driver is using: a read in the middle of a driver's poll could disturb it (a seesaw can report a phantom turn).
#include <string>

namespace apertar::hardware {

struct I2cProbe {
    bool busExists = false;
    bool found = false;
    bool kernelDriver = false;   // the address belongs to a kernel driver (I2C_SLAVE says busy): something is there
    std::string error;
};

I2cProbe probeI2cAddress(int bus, int address);

} // namespace apertar::hardware
