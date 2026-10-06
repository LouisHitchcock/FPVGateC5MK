// FPVGate RF-node protocol (docs/LINK_PROTOCOL.md): the C5 end of the UART
// link to an FPVGate S3 with receiverRadio = 2, or any other host.
//
// No ESP-IDF in here: the radio, the clock and the UART are reached through
// small interfaces, so the PC tests drive this exact code.
//
// Lines are  <payload>*<HH>\n  with HH the upper-case hex XOR of the payload.
//   S3 -> C5:  F,<MHz>   G,<gain>   Q
//              P,<MHz>,...                     up to 8 slots, 0 = slot off (fw 2)
//   C5 -> S3:  R,<seq u8>,<rssi 0-1023>       about 1 kHz, only in OK
//              S,<MHz>,<gain>,<state>,<fw>     after every F/G/P/Q, and a heartbeat
//
// Scan mode (P): the C5 cycles through the slots itself, one fast retune and
// one short dump per slot, and sends one binary record per cycle instead of
// R lines. A record starts with a byte no line contains, so both share the
// UART:
//   0xA5, len, payload[len], crc8(len, payload)      crc8: poly 0x07, init 0
//   payload: 'M', seq u8, t0 u32, slot mask u8, then for each slot in the
//   mask, in slot order: value u16 (bits 0-9 rssi, bit 15 dump failed), dt
//   u16 (us after t0). t0 and t0 + dt are the C5's clock at the middle of
//   each slot's dump. All little-endian. Status shows MHz 0 and SCAN.
#ifndef MP_NODE_CORE_H
#define MP_NODE_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "meter.h"

namespace node {

// Up to Raceband R8 (5917), the highest frequency checked with a carrier.
constexpr int kMinMhz = 5180, kMaxMhz = 5917;
constexpr size_t kMaxLine = 96;
constexpr uint32_t kSampleUs = 1000;            // 1 kHz
constexpr uint32_t kHeartbeatUs = 900000;       // the protocol asks for one every 1 s or less
constexpr int kFailsForErrRf = 5;               // consecutive failed dumps
constexpr const char* kFirmware = "2";          // 2: scan mode (P)
constexpr int kMaxSlots = 8;
constexpr uint8_t kRecordSync = 0xA5;
constexpr char kRecordScan = 'M';
constexpr uint16_t kValueFailed = 0x8000;
constexpr uint8_t kViewEvery = 16;              // scan cycles per onSample() pass (power of 2)

uint8_t crc8(const uint8_t* p, size_t n);

uint8_t xorSum(const char* p, size_t n);
// Writes payload*HH\n; returns its length (0 if it doesn't fit).
size_t formatLine(const char* payload, char* out, size_t cap);

// Accumulates bytes into lines and checks them. feed() returns the payload of
// a valid line (valid until the next call), else nullptr.
class LineReader {
public:
    const char* feed(char c);
    uint32_t good = 0, bad = 0, overflow = 0;

private:
    char buf_[kMaxLine + 1];
    size_t len_ = 0;
    bool overflowed_ = false;
};

class RadioPort {
public:
    virtual ~RadioPort() {}
    // Retune to mhz with this gain and settle; false if the receiver didn't
    // come back (a test dump failed).
    virtual bool tune(int mhz, int gain) = 0;
    virtual void setGain(int gain) = 0;
    // Scan mode: retune to mhz with this gain as fast as possible; settled
    // on return, ready for one measure().
    virtual void hop(int mhz, int gain) = 0;
    // One power measurement at the current frequency. tUs: the clock at the
    // middle of the measurement (set even when it fails).
    virtual bool measure(float* db, uint32_t* tUs) = 0;
    virtual int gainMax() = 0;
};

class Io {
public:
    virtual ~Io() {}
    virtual void write(const char* line, size_t n) = 0;
    // True if bytes from the S3 are waiting. A measurement taken while a
    // command is waiting is thrown away, so a sample can never be sent after
    // a retune command has arrived.
    virtual bool rxPending() = 0;
};

enum class State : uint8_t { NoFreq, Tuning, Ok, ErrFreq, ErrRf, Scan };
const char* stateName(State s);

class Core {
public:
    Core(RadioPort& radio, Io& io) : radio_(radio), io_(io) {}
    // After the radio is up. radioOk false reports ERR_RF.
    void boot(uint32_t nowUs, int gain, bool radioOk);
    // A byte from the S3.
    void onByte(char c, uint32_t nowUs);
    // A payload without framing (USB console injection, tests).
    void handle(const char* payload, uint32_t nowUs);
    // Call as often as possible: finishes retunes, takes samples, heartbeats.
    void step(uint32_t nowUs);

    // dB -> 0..1023: dbLo maps to 0, dbHi to 1023, clamped. The host maps
    // this back to the existing 0..255 calibration scale at 4x resolution.
    void setMap(float dbLo, float dbHi);
    void setAlpha(float a) { alpha_ = a; }
    float mapLo() const { return dbLo_; }
    float mapHi() const { return dbHi_; }

    State state() const { return state_; }
    int mhz() const { return mhz_; }
    int gain() const { return gain_; }
    uint16_t rssiFromDb(float db) const;

    // Called for every R sample sent, and in scan mode for each slot every
    // kViewEvery cycles (for the USB view).
    void (*onSample)(int mhz, float rawDb, float filtDb, uint16_t rssi, uint32_t nowUs) = nullptr;

    uint8_t scanMask() const { return scanMask_; }

    LineReader reader;
    uint32_t samples = 0, staleDropped = 0, failedDumps = 0, statuses = 0, commands = 0, rejected = 0;
    // Scan mode: records sent, cycles cut short by a waiting command.
    uint32_t cycles = 0, abortedCycles = 0;

private:
    RadioPort& radio_;
    Io& io_;
    State state_ = State::NoFreq;
    int mhz_ = 0;
    int gain_ = 30;
    bool needTune_ = false, needGain_ = false;
    uint8_t seq_ = 0;
    uint32_t nextSampleUs_ = 0, lastStatusUs_ = 0;
    int failRun_ = 0;
    float dbLo_ = -10.0f, dbHi_ = 53.75f;   // 16 counts per dB; idle about 116
    float alpha_ = 0.3f;
    mp::PilotFilter filt_;
    uint16_t scanMhz_[kMaxSlots] = {};
    uint8_t scanMask_ = 0;
    uint8_t cycleSeq_ = 0;
    mp::PilotFilter scanFilt_[kMaxSlots];

    bool parseSlots(const char* s);
    void scanCycle(uint32_t nowUs);
    void status(uint32_t nowUs);
    void send(const char* payload);
};

}  // namespace node

#endif
