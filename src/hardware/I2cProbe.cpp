#include "hardware/I2cProbe.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace apertar::hardware {

I2cProbe probeI2cAddress(int bus, int address)
{
    I2cProbe probe;
    const std::string path = "/dev/i2c-" + std::to_string(bus);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        probe.error = errno == ENOENT ? "Bus " + std::to_string(bus) + " does not exist on this Pi." : path + ": " + std::strerror(errno);
        return probe;
    }
    probe.busExists = true;
    if (::ioctl(fd, I2C_SLAVE, address) < 0) {
        if (errno == EBUSY) {
            probe.kernelDriver = true;
            probe.found = true;
        } else {
            probe.error = std::strerror(errno);
        }
        ::close(fd);
        return probe;
    }
    unsigned char byte = 0;
    probe.found = ::read(fd, &byte, 1) == 1;
    ::close(fd);
    return probe;
}

} // namespace apertar::hardware
