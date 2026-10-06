// I/Q dump engine. Register facts from our own disassembly of Espressif's
// Apache-2.0 librftest.a (adctrig, in mac_common.o); see
// docs/RADIO_INTERNALS.md and docs/PROVENANCE.md.
#include "dump.h"

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "heap_memory_layout.h"

// The modem owns the whole 128 KiB SRAM bank while a dump runs, and writes
// from 0x40830000 up. Keep the heap out of the bank; check_sram.py fails the
// build if static data reaches it.
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, iq_dump_bank);

// librftest.a. Nine 32-bit arguments; see rxOnlyStockDump() for which are
// safe.
extern "C" void adctrig(uint32_t countMinus1, uint32_t selector, uint32_t mode, uint32_t rateCode,
                        uint32_t unused5, uint32_t waitStyle, uint32_t w7, uint32_t w8, uint32_t w9);

namespace dump {
namespace {

constexpr uint32_t kRegCtrl = 0x600A9004;   // count 0-16, done 18, trigger 19, enable 31
constexpr uint32_t kRegOwner = 0x60095004;  // bits 8-11 = 2 and bit 16: the modem owns the bank
constexpr uint32_t kCountMask = 0x1FFFF;
constexpr uint32_t kBitDone = 1u << 18, kBitTrig = 1u << 19, kBitEnable = 1u << 31;
constexpr uint32_t kCtrlClear = kCountMask | (1u << 17) | kBitTrig | (3u << 20);
constexpr uint32_t kOwnerField = 0xFu << 8, kOwnerModem = 2u << 8, kOwnerBit = 1u << 16;

constexpr uint32_t kSentinel = 0xDEADBEEF;
constexpr uint32_t kGuard[4] = {0xA5A55A5A, 0x0F1E2D3C, 0xC3D2E1F0, 0x5A5AA5A5};
constexpr uint32_t kPollCycles = 240 * 1000;   // 1 ms at 240 MHz

volatile uint32_t* const bank = (volatile uint32_t*)kBankAddr;
Stats st;
bool isSeeded = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

inline volatile uint32_t& reg(uint32_t a) { return *(volatile uint32_t*)a; }

// THE ONLY CALL TO adctrig IN THIS FIRMWARE. Our disassembly shows:
//   selector 5/6 or mode 2 -> trig_tx_frame (transmits a frame)
//   mode 1                 -> phy_start_tx_tone_step (transmits a tone)
//   selector 12            -> ble_rx_start
//   wait style 1/2/3       -> gain-register writes or a timed delay
// selector 0 (software trigger), mode 0, rate code 0 (80 MS/s) and wait
// style 0 reach none of those: a pure receive dump. These values are fixed
// here and nothing else (in particular not the console) can change them.
void rxOnlyStockDump(int n) {
    adctrig((uint32_t)(n - 1), /*selector*/ 0, /*mode*/ 0, /*rate*/ 0, 0, /*wait*/ 0, 0, 0, 0);
}

void prefill(int n) {
    for (int i = 0; i < n; ++i) bank[i] = kSentinel;
    for (int i = 0; i < 4; ++i) bank[n + i] = kGuard[i];
}

bool complete(int n) {
    if (bank[0] == kSentinel || bank[n - 1] == kSentinel) return false;
    for (int i = 0; i < 4; ++i)
        if (bank[n + i] != kGuard[i]) return false;
    return true;
}

bool stock(int n, int64_t* tMidUs) {
    prefill(n);
    rxOnlyStockDump(n);
    if (tMidUs) *tMidUs = esp_timer_get_time() - n / 160;
    ++st.stock;
    return complete(n);
}

// The same register sequence the stock call uses for selector 0, without
// its setup (left as the seed dump programmed it) and without its ~1.65 ms
// of other overhead. Returns false on a poll timeout.
bool direct(int n, int64_t* tMidUs) {
    bool done = false;
    portENTER_CRITICAL(&mux);
    uint32_t savedOwner = reg(kRegOwner);
    reg(kRegOwner) = (savedOwner & ~kOwnerField) | kOwnerModem | kOwnerBit;
    uint32_t ctrl = (reg(kRegCtrl) & ~kCtrlClear) | (uint32_t)n | kBitEnable;
    reg(kRegCtrl) = ctrl;
    int64_t t0 = esp_timer_get_time();
    reg(kRegCtrl) = ctrl | kBitTrig;
    reg(kRegCtrl) = ctrl;
    uint32_t c0 = esp_cpu_get_cycle_count();
    while (esp_cpu_get_cycle_count() - c0 < kPollCycles) {
        if (reg(kRegCtrl) & kBitDone) {
            done = true;
            break;
        }
    }
    reg(kRegCtrl) = reg(kRegCtrl) & ~kBitEnable;
    reg(kRegOwner) = reg(kRegOwner) & ~(kOwnerField | kOwnerBit);
    reg(kRegOwner) = savedOwner;
    portEXIT_CRITICAL(&mux);
    if (tMidUs) *tMidUs = t0 + n / 160;
    return done;
}

}  // namespace

bool seed(int n) {
    if (n < 1 || n > kMaxWords) return false;
    isSeeded = stock(n, nullptr);
    return isSeeded;
}

bool capture(int n, Mode mode, int64_t* tMidUs, bool* fellBack) {
    if (fellBack) *fellBack = false;
    if (n < 1 || n > kMaxWords) return false;
    if (mode == Mode::Stock || !isSeeded) {
        bool ok = stock(n, tMidUs);
        if (ok) isSeeded = true;
        else ++st.incomplete;
        return ok;
    }
    prefill(n);
    ++st.direct;
    if (!direct(n, tMidUs)) {
        ++st.timeouts;
        if (fellBack) *fellBack = true;
        bool ok = stock(n, tMidUs);
        if (!ok) ++st.incomplete;
        return ok;
    }
    if (!complete(n)) {
        ++st.incomplete;
        return false;
    }
    return true;
}

const volatile uint32_t* words() { return bank; }
const Stats& stats() { return st; }

}  // namespace dump
