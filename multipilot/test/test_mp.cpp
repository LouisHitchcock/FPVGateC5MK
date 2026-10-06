// PC tests: meter, filter, framing.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <vector>

#include "frame.h"
#include "meter.h"

static int checks = 0, fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        ++checks;                                                   \
        if (!(c)) {                                                 \
            ++fails;                                                \
            printf("  FAIL %s (%s:%d)\n", #c, __FILE__, __LINE__); \
        }                                                           \
    } while (0)

static uint32_t pack(int i, int q, int g) {
    return ((uint32_t)i & 0x3FF) | (((uint32_t)q & 0x3FF) << 10) | ((uint32_t)g << 20);
}

static int clampAdc(double v) {
    long r = lround(v);
    return r > 511 ? 511 : r < -512 ? -512 : (int)r;
}

// A tone at RF offset rfHz above the LO, through the inverted spectrum.
static std::vector<uint32_t> tone(int n, double rfHz, double amp, double noise, int gain, unsigned seed = 1,
                                  double dcI = 0, double dcQ = 0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0, noise > 0 ? noise : 1);
    std::vector<uint32_t> w(n);
    double fbb = mp::kSpectrumSign * rfHz;
    for (int k = 0; k < n; ++k) {
        double ph = 2 * 3.141592653589793 * fbb * k / mp::kSampleRateHz + 0.3;
        double i = amp * cos(ph) + dcI + (noise > 0 ? nd(rng) : 0);
        double q = amp * sin(ph) + dcQ + (noise > 0 ? nd(rng) : 0);
        w[k] = pack(clampAdc(i), clampAdc(q), gain);
    }
    return w;
}

static float measure(const mp::BandMeter& m, const std::vector<uint32_t>& w, int gain, mp::MeterResult* out = nullptr) {
    mp::MeterResult r;
    m.measure(w.data(), gain, r);
    if (out) *out = r;
    return r.db;
}

int main() {
    // CRC-16/X-25 check value.
    const char* s = "123456789";
    CHECK(mp::crc16x25((const uint8_t*)s, 9) == 0x906E);

    // Frame layout.
    uint8_t f[32];
    size_t n = mp::frameEncode('T', "hi", 2, f, sizeof f);
    CHECK(n == 9);
    CHECK(f[0] == 0xA5 && f[1] == 0x5A && f[2] == 'T' && f[3] == 2 && f[4] == 0 && f[5] == 'h' && f[6] == 'i');
    uint16_t crc = mp::crc16x25(f + 2, 5);
    CHECK(f[7] == (crc & 0xFF) && f[8] == (crc >> 8));
    CHECK(mp::frameEncode('T', "hi", 2, f, 8) == 0);

    // Word decoding.
    uint32_t w = pack(-512, 511, 30);
    CHECK(mp::wordI(w) == -512 && mp::wordQ(w) == 511 && mp::wordGain(w) == 30);
    w = pack(-1, -2, 89);
    CHECK(mp::wordI(w) == -1 && mp::wordQ(w) == -2 && mp::wordGain(w) == 89);

    mp::BandMeter m;
    CHECK(!m.configure(100, 10000000));   // not a multiple of 8
    CHECK(m.configure(2048, 10000000));

    // In-band tone of amplitude A reads A^2.
    float db = measure(m, tone(2048, 10e6, 100, 0, 30), 30);
    printf("tone 100 LSB at +10 MHz: %.2f dB (want 40)\n", db);
    CHECK(fabs(db - 40) < 0.2);
    db = measure(m, tone(2048, 10e6, 10, 0, 30), 30);
    CHECK(fabs(db - 20) < 0.5);

    // The mirror (RF 10 MHz below the LO) is in a Hann null.
    float mir = measure(m, tone(2048, -10e6, 100, 0, 30), 30);
    printf("mirror: %.2f dB\n", mir);
    CHECK(mir < 40 - 35);

    // Raceband neighbours (pilot +-37 MHz: RF +47 and -27 above the LO).
    float up = measure(m, tone(2048, 47e6, 100, 0, 30), 30);
    float dn = measure(m, tone(2048, -27e6, 100, 0, 30), 30);
    printf("neighbours: +37 -> %.2f dB, -37 -> %.2f dB\n", up, dn);
    CHECK(up < 40 - 25 && dn < 40 - 25);

    // DC is removed exactly, including sub-LSB means.
    mp::MeterResult r;
    db = measure(m, tone(2048, 10e6, 0, 0, 30, 1, -2, -1.6), 30, &r);
    CHECK(db < -30);
    db = measure(m, tone(2048, 10e6, 100, 0, 30, 1, -2.4, 3.3), 30, &r);
    CHECK(fabs(db - 40) < 0.2);

    // White noise, sigma 1 on I and Q: band power = 2 * sum(w^2) / sum(w)^2
    // = 2 * 3 / 16 for the half-sample Hann (L = 8), -4.26 dB.
    CHECK(m.configure(8192, 10000000));
    db = measure(m, tone(8192, 10e6, 0, 4, 30, 7), 30);
    printf("noise sigma 4: %.2f dB (want %.2f)\n", db, 10 * log10(2 * 16 * 3.0 / 16));
    CHECK(fabs(db - 10 * log10(2 * 16 * 3.0 / 16)) < 0.3);
    CHECK(m.configure(2048, 10000000));

    // Clip and gain-field counts.
    std::vector<uint32_t> t = tone(2048, 10e6, 600, 0, 30);
    t[5] = pack(0, 0, 31);
    measure(m, t, 30, &r);
    CHECK(r.clips > 100);
    CHECK(r.gainMismatch == 1);

    // Filter: a single spike is removed by the median.
    mp::PilotFilter pf;
    float v = 0;
    const float in[] = {0, 0, 0, 27, 0, 0, 0};
    float peak = 0;
    for (float x : in) {
        v = pf.push(x, 0.3f);
        peak = fmaxf(peak, v);
    }
    CHECK(peak < 0.01f);
    // A step passes through.
    for (int i = 0; i < 40; ++i) v = pf.push(30, 0.3f);
    CHECK(fabs(v - 30) < 0.01f);
    // Long runs keep the median ring consistent.
    for (int i = 0; i < 1000; ++i) v = pf.push(i % 2 ? 10 : 10, 0.3f);
    CHECK(fabs(v - 10) < 0.01f);

    // power() gives exactly measure()'s dB: tones, noise, DC, clipping, the
    // scan's n=256 and the scanner's 2048, both offsets (the block-periodic
    // path) and 7 MHz (the general path).
    {
        int mismatch = 0, cases = 0;
        for (int n : {256, 2048})
            for (int off : {10000000, -10000000, 7000000})
                for (unsigned seed = 1; seed <= 6; ++seed) {
                    mp::BandMeter m;
                    CHECK(m.configure(n, off));
                    double amp = seed * 90.0, rf = off + (seed % 3) * 2e6;
                    auto w = tone(n, rf, amp, 3.0 * seed, 30, seed, seed * 1.5, -2.0 * seed);
                    mp::MeterResult r;
                    m.measure(w.data(), -1, r);
                    ++cases;
                    if (m.power(w.data()) != r.db) ++mismatch;
                }
        printf("power() vs measure(): %d of %d differ\n", mismatch, cases);
        CHECK(cases == 36 && mismatch == 0);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
