// Band-power meter for one I/Q dump, and the per-pilot filter.
//
// No ESP-IDF in here, so the PC tests build it too.
//
// Per dump: remove DC, mix the pilot down to 0 Hz, sum Hann-windowed blocks
// of kBlock samples (each block is one windowed DFT bin centred on the
// pilot, about 10 MHz wide at 80 MS/s), and average |block|^2. Normalised so
// an in-band tone of amplitude A reads A^2 (LSB^2). See
// docs/RADIO_INTERNALS.md.
#ifndef MP_METER_H
#define MP_METER_H

#include <stdint.h>

namespace mp {

// With the sample layout below, RF above the LO comes out at NEGATIVE
// baseband frequency (measured, docs/RADIO_INTERNALS.md). Everything that maps
// an RF offset to a baseband frequency uses this one constant: the meter,
// the capture analysis in tools/mp.py and the web page.
constexpr int kSpectrumSign = -1;

constexpr int kSampleRateHz = 80000000;   // dump rate code 0
constexpr int kBlock = 8;                 // window length L
constexpr int kMinSamples = 64;
constexpr int kMaxSamples = 8192;

// One 32-bit word per sample: bits 0-9 signed I, bits 10-19 signed Q,
// bits 20-26 the gain index in force for that sample.
inline int wordI(uint32_t w) { return (int32_t)(w << 22) >> 22; }
inline int wordQ(uint32_t w) { return (int32_t)(w << 12) >> 22; }
inline int wordGain(uint32_t w) { return (int)((w >> 20) & 0x7F); }

struct MeterResult {
    float db = 0;            // 10*log10(band power, LSB^2)
    float dcI = 0, dcQ = 0;  // mean I and Q, LSB
    int clips = 0;           // samples with I or Q at a rail
    int gainMismatch = 0;    // samples whose gain field != the forced index
};

class BandMeter {
public:
    ~BandMeter();
    // n: samples per dump, a multiple of kBlock. offsetHz: how far the
    // pilot sits above the LO. Rebuilds the tables; false on bad args or
    // out of memory (the old tables are kept then).
    bool configure(int n, int offsetHz);
    // words: n samples. expectGain < 0 skips the gain-field check.
    void measure(const volatile uint32_t* words, int expectGain, MeterResult& out) const;
    // The same band power (identical result) in one pass, without the DC,
    // clip and gain reports: the FPVGate scan's hot path. Returns dB.
    float power(const uint32_t* words) const;
    int samples() const { return n_; }

private:
    int n_ = 0;
    int16_t* tab_ = nullptr;   // n pairs (re, im), Q14: window x mixer
    int32_t* blkW_ = nullptr;  // per block, the sum of tab_ (re, im), for DC removal
    int32_t* blkSum_ = nullptr;   // power(): per block (re, im) before DC removal
    bool periodic_ = false;       // every block has the same tab_ entries

    float powerPeriodic(const uint32_t* words) const;
    float scale_ = 1;          // power normalisation
};

// Median of the last 3 raw dB values (removes single-dump Wi-Fi spikes),
// then an EMA.
class PilotFilter {
public:
    void reset() { count_ = 0; }
    float push(float rawDb, float alpha);
    float value() const { return ema_; }

private:
    float hist_[3] = {0, 0, 0};
    int count_ = 0;
    float ema_ = 0;
};

}  // namespace mp

#endif
