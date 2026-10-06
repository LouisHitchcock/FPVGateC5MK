// The PHY's I/Q dump engine: N consecutive baseband samples at 80 MS/s
// written by the modem into a fixed SRAM bank. Receive only.
#ifndef MP_DUMP_H
#define MP_DUMP_H

#include <stdint.h>

namespace dump {

constexpr uint32_t kBankAddr = 0x40830000;   // where the engine writes
constexpr int kMaxWords = 8192;              // + 4 guard words, well inside the bank

enum class Mode : uint8_t { Stock, Direct };

struct Stats {
    uint32_t stock = 0, direct = 0;
    uint32_t timeouts = 0;    // direct poll timed out (then fell back to stock)
    uint32_t incomplete = 0;  // sentinel or guard check failed
};

// One stock dump, which also programs the engine's setup registers. Call
// once after the radio is up, before any direct dump.
bool seed(int n);
// Capture n words. Direct mode falls back to a stock dump on a timeout.
// tMidUs: esp_timer time at the middle of the capture. Returns false if the
// capture is incomplete.
bool capture(int n, Mode mode, int64_t* tMidUs, bool* fellBack);
const volatile uint32_t* words();
const Stats& stats();

}  // namespace dump

#endif
