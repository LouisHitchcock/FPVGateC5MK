// Host side of the C5MK UART link: a portable reference implementation.
//
// Drop c5host.h and c5host.cpp into any C++11 firmware (ESP32, RP2040,
// STM32, Linux). No dependencies: you pass in the bytes the UART received
// and a microsecond clock, and give it a function that writes bytes to the
// UART. It does the rest of docs/LINK_PROTOCOL.md: start-up, the slot list,
// gain, checksums, record parsing, and mapping the C5's timestamps onto your
// clock. GateDetector turns one pilot's readings into gate passes.
//
// Needs C5MK firmware 2 or later (scan mode).
#ifndef C5HOST_H
#define C5HOST_H

#include <stddef.h>
#include <stdint.h>

namespace c5host {

constexpr int kSlots = 8;
constexpr uint32_t kBaud = 921600;
constexpr uint16_t kMinMhz = 5180, kMaxMhz = 5917;
constexpr int kMaxGain = 89;   // the C5 reports its real limit; 89 on chip v1.0

// One reading of one pilot.
struct Sample {
    uint8_t slot;     // 0-7, the position in the slot list
    uint16_t rssi;    // 0..1023, 16 counts per dB
    uint32_t timeUs;  // on your clock: when the C5 took the reading
};

// Link counters, for diagnostics. All count up from begin().
struct Counters {
    uint32_t records = 0;      // scan records accepted
    uint32_t samples = 0;      // readings delivered
    uint32_t seqGaps = 0;      // records missing (record sequence skipped)
    uint32_t badRecords = 0;   // bad length or CRC
    uint32_t badLines = 0;     // text lines with a bad checksum or format
    uint32_t failedDumps = 0;  // readings the C5 flagged as failed
    uint32_t staleRecords = 0; // records for an old slot list, dropped
};

class Link {
public:
    using WriteFn = void (*)(void* ctx, const uint8_t* data, size_t len);
    using SampleFn = void (*)(void* ctx, const Sample& s);

    // write: sends bytes to the C5's UART RX. onSample: called from feed()
    // for every reading, in time order per slot.
    Link(WriteFn write, SampleFn onSample, void* ctx) : write_(write), onSample_(onSample), ctx_(ctx) {}

    // The frequency for each slot in MHz, 0 = off. Sent when it changes.
    // Returns false (and keeps the old list) if an entry is out of range.
    bool setSlots(const uint16_t mhz[kSlots]);
    // Receiver gain index, 0..89. 30 is a good bench start; see
    // INTEGRATION_GUIDE.md for choosing it.
    bool setGain(int gain);

    // Call once after opening the UART.
    void begin(uint32_t nowUs);
    // Every byte read from the UART, in order, as soon as practical. Read in
    // bulk: at 8 pilots the C5 sends ~43 KB/s.
    void feed(const uint8_t* data, size_t len, uint32_t nowUs);
    // Call every few ms: resends the slot list if the C5 rebooted or lost it,
    // and polls status.
    void tick(uint32_t nowUs);

    // A status line or record arrived in the last 2.5 s.
    bool online(uint32_t nowUs) const;
    // The C5 acknowledged the current slot list and is scanning it.
    bool scanning() const { return scanning_ && !awaitingAck_; }
    // From the C5's last status line.
    int firmware() const { return firmware_; }
    int reportedGain() const { return reportedGain_; }
    const char* state() const { return state_; }   // SCAN, ERR_FREQ, ERR_RF, ...
    const Counters& counters() const { return n_; }

private:
    WriteFn write_;
    SampleFn onSample_;
    void* ctx_;

    uint16_t slots_[kSlots] = {};
    uint8_t wantMask_ = 0;
    bool slotsDirty_ = true;
    int gain_ = 30;
    bool gainDirty_ = true;

    bool begun_ = false;
    bool heard_ = false;
    uint32_t lastHeardUs_ = 0;
    uint32_t lastQUs_ = 0;
    uint32_t lastPUs_ = 0;
    bool awaitingAck_ = false;
    bool scanning_ = false;
    int firmware_ = 0;
    int reportedGain_ = -1;
    char state_[12] = "OFFLINE";

    char line_[97] = {};
    size_t lineLen_ = 0;
    bool lineOverflow_ = false;
    uint8_t recState_ = 0;   // 0 text, 1 length next, 2 payload and CRC
    uint8_t recLen_ = 0, recPos_ = 0;
    uint8_t rec_[1 + 7 + 4 * kSlots] = {};
    bool seqValid_ = false;
    uint8_t lastSeq_ = 0;

    bool clockValid_ = false;
    uint32_t clockOffset_ = 0;
    uint8_t clockLeak_ = 0, clockLateRun_ = 0;

    Counters n_;

    void send(const char* payload);
    void sendSlots(uint32_t nowUs);
    void onLine(uint32_t nowUs);
    void onRecord(uint32_t nowUs);
};

// Gate passes for one pilot, the way FPVGate times laps: smooth the
// readings, enter the gate above `enter`, leave below `exit`, and time the
// pass at its highest reading. Thresholds are on the 0..1023 scale.
class GateDetector {
public:
    void setThresholds(uint16_t enter, uint16_t exit) { enter_ = enter, exit_ = exit; }
    void reset() { inside_ = false, valid_ = false; }
    // Feed every sample of this pilot. Returns true when a pass ends;
    // passUs is then the time of the pass's peak.
    bool push(uint16_t rssi, uint32_t timeUs, uint32_t* passUs);
    uint16_t filtered() const { return f_; }
    bool inside() const { return inside_; }

private:
    uint16_t enter_ = 600, exit_ = 500;
    uint16_t f_ = 0;
    bool valid_ = false, inside_ = false;
    uint16_t peak_ = 0;
    uint32_t peakUs_ = 0;
};

}  // namespace c5host

#endif
