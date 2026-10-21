// RotorHazard API 35 serial-node endpoint. Portable: no USB or radio driver.
#ifndef C5MK_RH_USB_H
#define C5MK_RH_USB_H

#include <stddef.h>
#include <stdint.h>

namespace rhusb {

constexpr unsigned kSeats = 8;
constexpr uint32_t kCommandTimeoutMs = 100;
constexpr uint32_t kIdleTimeoutMs = 5000;
// After a frequency change no crossing starts for this long: the scan
// restarts with empty filters, so a lone settling sample isn't a pass.
constexpr uint32_t kSettleMs = 100;
// The RSSI RH shows is the highest sample of about the last 250 ms, so its
// graph, a point every ~200 ms, gets every peak at its true height.
constexpr unsigned kShowBuckets = 10;
constexpr uint32_t kShowBucketMs = 25;
// The crossing flag RH reads stays set this long after a pass ends: RH reads
// it about every 100 ms and a fast pass can be over EnterAt for less.
constexpr uint32_t kShowCrossingMs = 250;

// A VTX powering on or changing channel jumps off the noise floor in a few
// ms; a quad flying in needs hundreds of ms to rise that far. Such a jump is
// hidden from RH (RSSI, history, NodePeak and crossing) for holdMs and dropped
// if it falls back sooner; one that lasts is shown and counts as usual, a
// crossing timed from its peak like any other.
struct Reject {
    uint16_t riseMs = 30;    // left the floor no longer ago than this: a jump
    uint16_t holdMs = 90;    // a jump that ends sooner is not a pass
    uint16_t quietMs = 100;  // the floor must have held this long before the jump
    uint8_t margin = 16;     // "the floor": within this of the seat's nadir (~4 dB)
};

// A quad passing a gate in a small room dips under ExitAt and back for a few
// hundred ms (multipath). A pass therefore ends only once the level has stayed
// under ExitAt for mergeMs; back over EnterAt sooner is the same pass, timed
// at its highest sample, as FPVGate does. The lap reaches RH mergeMs later,
// still timed at the peak. smoothMs, if set, averages the level over about
// that long before crossing detection and before RH sees it; it also lowers
// the height of passes shorter than it, so it is off by default.
struct Detect {
    uint16_t mergeMs = 300;  // 0: a pass ends at the first sample under ExitAt
    uint16_t smoothMs = 0;   // 0: off
};

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
    // 0 for riseMs turns the rejection off.
    void setReject(const Reject& reject) { reject_ = reject; }
    const Reject& reject() const { return reject_; }
    void setDetect(const Detect& detect) { detect_ = detect; }
    const Detect& detect() const { return detect_; }

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
        // Rolling maximum for the RSSI RH shows: the highest sample in each
        // kShowBucketMs bucket, and which bucket (sample ms / kShowBucketMs).
        uint8_t bucketMax[kShowBuckets] = {};
        uint32_t bucketId[kShowBuckets] = {};
        bool bucketUsed[kShowBuckets] = {};
        uint32_t settleUntilMs = 0;
        // The current run of samples at the floor (see Reject); a jump off it,
        // hidden from RH until jumpUntilMs, with its highest level; and a
        // crossing that started in a jump, with the pass levels to put back
        // if it is dropped.
        bool quiet = false, quietSeen = false, jumping = false, jumpDropped = false, held = false;
        uint32_t quietStartMs = 0, quietLastMs = 0, jumpUntilMs = 0, jumpFirstMs = 0, jumpLastMs = 0;
        uint8_t jumpMax = 0, keptPeak = 1, keptNadir = 254;
        // A pass that ended, still shown as crossing until shownUntilMs.
        bool shownCrossing = false;
        uint32_t shownUntilMs = 0;
        // A crossing under ExitAt since endingMs, waiting out Detect::mergeMs.
        bool ending = false;
        uint32_t endingMs = 0;
        // The Detect::smoothMs average of the level (valid once smoothed).
        bool smoothed = false;
        float smooth = 0;
    } seats_[kSeats];
    Port& port_;
    Reject reject_;
    Detect detect_;
    uint8_t selected_ = 0, command_ = 0, needed_ = 0, received_ = 0;
    uint8_t input_[3] = {}, stats_[16] = {};
    uint32_t lastByteMs_ = 0, lastCommandMs_ = 0;
    char version_[16] = {}, board_[16] = {};
    void read(uint8_t command, uint32_t nowMs);
    void applyWrite(uint32_t nowMs);
    void reply(const uint8_t* data, size_t size);
    void latchStats(uint32_t nowMs);
    void settleAll(uint32_t nowMs);
    static uint8_t shownRssi(const Seat& seat, uint32_t nowMs);
    // A level for RH's RSSI and history, sampled from firstMs to lastMs.
    static void show(Seat& seat, uint8_t level, uint32_t firstMs, uint32_t lastMs);
    // Crossings and laps; may hold a jump back from show (see Reject).
    void detect(Seat& seat, uint8_t level, uint32_t sampleMs);
    // The crossing is over: count the lap, timed at the middle of its peak.
    static void endPass(Seat& seat, uint32_t endMs);
    // A crossing already waiting out mergeMs is a whole pass: count it.
    static void finishEnding(Seat& seat, uint32_t endMs) { if (seat.crossing && seat.ending) endPass(seat, endMs); }
    static void stopCrossing(Seat& seat) {
        seat.crossing = seat.held = seat.shownCrossing = seat.jumping = seat.ending = false;
    }
};

}  // namespace rhusb
#endif
