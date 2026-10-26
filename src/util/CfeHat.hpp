#pragma once

#include "util/MediaProbe.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace apertar {

// The CFexpress HAT's status byte, read the way apertar-cfe-automounter reads it
// (SMBus receive byte): bit 0 set = no card in the slot, bit 1 clear = eject held,
// 0x69 = the HAT has nothing to report. The automounter polls the same byte every
// 100 ms; a single-byte read does not disturb it.
struct CfeHatConfig {
    bool enabled = false;
    int bus = 10;
    int address = 0x34;
};

class CfeHatReader {
public:
    explicit CfeHatReader(CfeHatConfig config) : config_(config) {}
    ~CfeHatReader()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    CfeHatReader(const CfeHatReader &) = delete;
    CfeHatReader &operator=(const CfeHatReader &) = delete;

    // Two reads that agree, or Unknown: a glitch must not end a take.
    CardPresence cardPresence()
    {
        if (!config_.enabled)
            return CardPresence::Unknown;
        const int first = readByte();
        const int second = readByte();
        if (first < 0 || second < 0 || first == 0x69 || second == 0x69 ||
            (first & 0x01) != (second & 0x01))
            return CardPresence::Unknown;
        return (first & 0x01) ? CardPresence::Absent : CardPresence::Present;
    }

private:
    int readByte()
    {
        if (fd_ < 0) {
            fd_ = ::open(("/dev/i2c-" + std::to_string(config_.bus)).c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ < 0)
                return -1;
            if (::ioctl(fd_, I2C_SLAVE, config_.address) < 0) {
                ::close(fd_);
                fd_ = -1;
                return -1;
            }
        }
        i2c_smbus_data data {};
        i2c_smbus_ioctl_data request { I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data };
        if (::ioctl(fd_, I2C_SMBUS, &request) < 0) {
            ::close(fd_);
            fd_ = -1;
            return -1;
        }
        return data.byte;
    }

    CfeHatConfig config_;
    int fd_ = -1;
};

} // namespace apertar
