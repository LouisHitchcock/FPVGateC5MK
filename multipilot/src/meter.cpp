#include "meter.h"

#include <math.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"   // power() runs from IRAM: no flash-cache stalls
#else
#define IRAM_ATTR
#endif

namespace mp {

static const float kPi = 3.14159265358979f;

BandMeter::~BandMeter() {
    free(tab_);
    free(blkW_);
    free(blkSum_);
}

bool BandMeter::configure(int n, int offsetHz) {
    if (n < kMinSamples || n > kMaxSamples || n % kBlock) return false;
    if (offsetHz <= -kSampleRateHz / 2 || offsetHz >= kSampleRateHz / 2) return false;
    int blocks = n / kBlock;
    int16_t* tab = (int16_t*)malloc(sizeof(int16_t) * 2 * n);
    int32_t* blkW = (int32_t*)malloc(sizeof(int32_t) * 2 * blocks);
    int32_t* blkSum = (int32_t*)malloc(sizeof(int32_t) * 2 * blocks);
    if (!tab || !blkW || !blkSum) {
        free(tab);
        free(blkW);
        free(blkSum);
        return false;
    }

    // Hann window offset by half a sample, so none of the L samples gets a
    // zero weight. Its sum is L/2.
    float win[kBlock];
    float winSum = 0;
    for (int k = 0; k < kBlock; ++k) {
        win[k] = 0.5f - 0.5f * cosf(2 * kPi * (k + 0.5f) / kBlock);
        winSum += win[k];
    }

    // The pilot is at baseband frequency fbb; multiply by exp(-j 2 pi fbb n / fs)
    // to bring it to 0 Hz. The phase is reduced to one cycle in double
    // precision so long dumps don't drift.
    double fbb = (double)kSpectrumSign * offsetHz;
    for (int b = 0; b < blocks; ++b) {
        int32_t sre = 0, sim = 0;
        for (int k = 0; k < kBlock; ++k) {
            int i = b * kBlock + k;
            double cyc = -fbb * i / kSampleRateHz;
            cyc -= floor(cyc);
            float ph = (float)(2 * 3.141592653589793 * cyc);
            int16_t re = (int16_t)lrintf(16384.0f * win[k] * cosf(ph));
            int16_t im = (int16_t)lrintf(16384.0f * win[k] * sinf(ph));
            tab[2 * i] = re;
            tab[2 * i + 1] = im;
            sre += re;
            sim += im;
        }
        blkW[2 * b] = sre;
        blkW[2 * b + 1] = sim;
    }

    free(tab_);
    free(blkW_);
    free(blkSum_);
    tab_ = tab;
    blkW_ = blkW;
    blkSum_ = blkSum;
    n_ = n;
    // At 10 MHz offset and 80 MS/s the mixer repeats every 8 samples, one
    // block: every block then uses the same coefficients.
    periodic_ = true;
    for (int i = 2 * kBlock; i < 2 * n && periodic_; ++i) periodic_ = tab[i] == tab[i % (2 * kBlock)];
    // Blocks are shifted right by 4 before squaring (headroom), so the
    // power is in units of (2^14 / 2^4)^2 per LSB^2 per unit window sum.
    float unit = 16384.0f / 16.0f * winSum;
    scale_ = 1.0f / (unit * unit * blocks);
    return true;
}

void BandMeter::measure(const volatile uint32_t* words, int expectGain, MeterResult& out) const {
    const int blocks = n_ / kBlock;
    int32_t sumI = 0, sumQ = 0;
    int clips = 0, gainBad = 0;
    int64_t power = 0;
    const int16_t* t = tab_;
    const uint32_t g = (uint32_t)expectGain;

    // First pass: DC (needs the whole dump), clip and gain checks.
    for (int i = 0; i < n_; ++i) {
        uint32_t w = words[i];
        int vi = wordI(w), vq = wordQ(w);
        sumI += vi;
        sumQ += vq;
        clips += ((unsigned)(vi + 511) > 1021u) | ((unsigned)(vq + 511) > 1021u);
        gainBad += (expectGain >= 0) & (((w >> 20) & 0x7F) != g);
    }
    // Mean in Q8; the DC error left over is under 1/256 LSB.
    int32_t mI = (int32_t)(((int64_t)sumI * 256) / n_);
    int32_t mQ = (int32_t)(((int64_t)sumQ * 256) / n_);

    // Second pass: windowed, mixed block sums; subtract each block's DC
    // response (mean x the block's tab sum), then |.|^2.
    for (int b = 0; b < blocks; ++b) {
        int32_t re = 0, im = 0;
        const volatile uint32_t* p = words + b * kBlock;
        for (int k = 0; k < kBlock; ++k) {
            uint32_t w = p[k];
            int32_t vi = wordI(w), vq = wordQ(w);
            int32_t cr = t[0], ci = t[1];
            t += 2;
            re += vi * cr - vq * ci;
            im += vi * ci + vq * cr;
        }
        int32_t wr = blkW_[2 * b], wi = blkW_[2 * b + 1];
        int32_t dre = (int32_t)(((int64_t)mI * wr - (int64_t)mQ * wi) >> 8);
        int32_t dim = (int32_t)(((int64_t)mI * wi + (int64_t)mQ * wr) >> 8);
        re = (re - dre) >> 4;
        im = (im - dim) >> 4;
        power += (int64_t)re * re + (int64_t)im * im;
    }

    float p = (float)power * scale_;
    out.db = p > 1e-6f ? 10.0f * log10f(p) : -60.0f;
    out.dcI = sumI / (float)n_;
    out.dcQ = sumQ / (float)n_;
    out.clips = clips;
    out.gainMismatch = gainBad;
}

// measure()'s two passes folded into one: the windowed, mixed block sums
// don't depend on the DC, so they are kept per block and the DC response is
// subtracted afterwards, once the mean is known.
__attribute__((optimize("O3", "unroll-loops"))) IRAM_ATTR float BandMeter::power(const uint32_t* words) const {
    if (periodic_) return powerPeriodic(words);
    const int blocks = n_ / kBlock;
    int32_t sumI = 0, sumQ = 0;
    const int16_t* t = tab_;
    const uint32_t* p = words;
    int32_t* bs = blkSum_;
    for (int b = 0; b < blocks; ++b) {
        int32_t re = 0, im = 0;
        for (int k = 0; k < kBlock; ++k) {
            uint32_t w = *p++;
            int32_t vi = wordI(w), vq = wordQ(w);
            int32_t cr = t[0], ci = t[1];
            t += 2;
            sumI += vi;
            sumQ += vq;
            re += vi * cr - vq * ci;
            im += vi * ci + vq * cr;
        }
        bs[0] = re;
        bs[1] = im;
        bs += 2;
    }
    int32_t mI = (int32_t)(((int64_t)sumI * 256) / n_);
    int32_t mQ = (int32_t)(((int64_t)sumQ * 256) / n_);
    int64_t pw = 0;
    for (int b = 0; b < blocks; ++b) {
        int32_t wr = blkW_[2 * b], wi = blkW_[2 * b + 1];
        int32_t dre = (int32_t)(((int64_t)mI * wr - (int64_t)mQ * wi) >> 8);
        int32_t dim = (int32_t)(((int64_t)mI * wi + (int64_t)mQ * wr) >> 8);
        int32_t re = (blkSum_[2 * b] - dre) >> 4;
        int32_t im = (blkSum_[2 * b + 1] - dim) >> 4;
        pw += (int64_t)re * re + (int64_t)im * im;
    }
    float v = (float)pw * scale_;
    return v > 1e-6f ? 10.0f * log10f(v) : -60.0f;
}

// power() when every block has the same coefficients: they stay in
// registers, and the DC response is one value for all blocks.
__attribute__((optimize("O3", "unroll-loops"))) IRAM_ATTR float BandMeter::powerPeriodic(
    const uint32_t* words) const {
    const int blocks = n_ / kBlock;
    int32_t c[2 * kBlock];
    for (int k = 0; k < 2 * kBlock; ++k) c[k] = tab_[k];
    int32_t sumI = 0, sumQ = 0;
    const uint32_t* p = words;
    int32_t* bs = blkSum_;
    for (int b = 0; b < blocks; ++b) {
        int32_t re = 0, im = 0;
        for (int k = 0; k < kBlock; ++k) {
            uint32_t w = p[k];
            int32_t vi = wordI(w), vq = wordQ(w);
            sumI += vi;
            sumQ += vq;
            re += vi * c[2 * k] - vq * c[2 * k + 1];
            im += vi * c[2 * k + 1] + vq * c[2 * k];
        }
        p += kBlock;
        bs[0] = re;
        bs[1] = im;
        bs += 2;
    }
    int32_t mI = (int32_t)(((int64_t)sumI * 256) / n_);
    int32_t mQ = (int32_t)(((int64_t)sumQ * 256) / n_);
    int32_t wr = blkW_[0], wi = blkW_[1];
    int32_t dre = (int32_t)(((int64_t)mI * wr - (int64_t)mQ * wi) >> 8);
    int32_t dim = (int32_t)(((int64_t)mI * wi + (int64_t)mQ * wr) >> 8);
    int64_t pw = 0;
    for (int b = 0; b < blocks; ++b) {
        int32_t re = (blkSum_[2 * b] - dre) >> 4;
        int32_t im = (blkSum_[2 * b + 1] - dim) >> 4;
        pw += (int64_t)re * re + (int64_t)im * im;
    }
    float v = (float)pw * scale_;
    return v > 1e-6f ? 10.0f * log10f(v) : -60.0f;
}

float PilotFilter::push(float rawDb, float alpha) {
    hist_[count_ % 3] = rawDb;
    ++count_;
    float med;
    if (count_ < 3) {
        med = rawDb;
    } else {
        float a = hist_[0], b = hist_[1], c = hist_[2];
        med = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
    }
    if (count_ == 1) ema_ = med;
    else ema_ += alpha * (med - ema_);
    if (count_ >= 300) count_ = 3 + count_ % 3;  // keep the ring index, avoid overflow
    return ema_;
}

}  // namespace mp
