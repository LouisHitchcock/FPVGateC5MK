// RotorHazard API 35 serial-node endpoint. Portable: no USB or radio driver.
#ifndef C5MK_RH_USB_H
#define C5MK_RH_USB_H

#include <stddef.h>
#include <stdint.h>

namespace rhusb {

constexpr unsigned kSeats = 8;
constexpr uint32_t kCommandTimeoutMs = 100;
constexpr uint32_t kIdleTimeoutMs = 5000;

// Command IDs and byte order follow RotorHazard's src/interface/RHInterface.py.
enum Command : uint8_t {
    READ_FREQUENCY = 0x03, TEST_RX_REGISTER = 0x04, READ_LAP_STATS = 0x05,
    READ_LAP_PASS_STATS = 0x0D, READ_LAP_EXTREMUMS = 0x0E, READ_RHFEAT_FLAGS = 0x11,
    READ_REVISION_CODE = 0x22, READ_NODE_RSSI_PEAK = 0x23, READ_NODE_RSSI_NADIR = 0x24,
    READ_ENTER_AT_LEVEL = 0x31, READ_EXIT_AT_LEVEL = 0x32, READ_TIME_MILLIS = 0x33,
    READ_MULTINODE_COUNT = 0x39, READ_CURNODE_INDEX = 0x3A, READ_NODE_SLOTIDX = 0x3C,
    READ_FW_VERSION = 0x3D, READ_FW_BUILDDATE = 0x3E, READ_FW_BUILDTIME = 0x3F,
    READ_FW_PROCTYPE = 0x40, WRITE_FREQUENCY = 0x51, WRITE_ENTER_AT_LEVEL = 0x71,
    WRITE_EXIT_AT_LEVEL = 0x72, SEND_STATUS_MESSAGE = 0x75, FORCE_END_CROSSING = 0x78,
    WRITE_CURNODE_INDEX = 0x7A,
};

class Port {
public:
    virtual ~Port() {}
    virtual void write(const uint8_t* data, size_t size) = 0;
    virtual void frequencies(const uint16_t* mhz) = 0;
};

class Core {
public:
    explicit Core(Port& port) : port_(port) {}
    void begin(uint32_t nowMs);
    void end();
    // Call only after claiming the radio. First byte must be READ_REVISION_CODE.
    void feed(uint8_t byte, uint32_t nowMs);
    bool expired(uint32_t nowMs) const;
    void sample(unsigned seat, uint16_t rawRssi, uint32_t sampleMs);
    void setIdentity(const char* version, const char* board);

private:
    struct Seat {
        uint16_t frequency = 0;
        uint8_t enter = 90, exit = 80, rssi = 1;
        uint8_t peak = 1, nadir = 254, passPeak = 1, passNadir = 254, lap = 0;
        bool crossing = false, sampled = false;
        uint32_t firstPeakMs = 0, lastPeakMs = 0, passMs = 0, sampleMs = 0;
        uint16_t loopUs = 0;
        // Highest and lowest level since RH's last poll, for its RSSI history:
        // it polls a few times a second, the scan samples ~1000 times. 'shown'
        // is the extremum reported last.
        bool windowed = false;
        uint8_t windowMax = 0, windowMin = 0, shown = 0;
        uint32_t maxFirstMs = 0, maxLastMs = 0, minFirstMs = 0, minLastMs = 0;
    } seats_[kSeats];
    Port& port_;
    uint8_t selected_ = 0, command_ = 0, needed_ = 0, received_ = 0;
    uint8_t input_[3] = {}, stats_[16] = {};
    uint32_t lastByteMs_ = 0, lastCommandMs_ = 0;
    char version_[16] = {}, board_[16] = {};
    void read(uint8_t command, uint32_t nowMs);
    void applyWrite(uint32_t nowMs);
    void reply(const uint8_t* data, size_t size);
    void latchStats(uint32_t nowMs);
};

}  // namespace rhusb
#endif
