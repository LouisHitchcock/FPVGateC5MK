#include "c5host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c5host {
namespace {

constexpr uint8_t kRecordSync = 0xA5;
constexpr uint32_t kOnlineUs = 2500000;
constexpr uint32_t kStatusPollUs = 1000000;
constexpr uint32_t kResendUs = 250000;

uint8_t xorSum(const char* p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) x ^= (uint8_t)p[i];
    return x;
}

// Poly 0x07, init 0.
uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool Link::setSlots(const uint16_t mhz[kSlots]) {
    uint8_t mask = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!mhz[i]) continue;
        if (mhz[i] < kMinMhz || mhz[i] > kMaxMhz) return false;
        mask |= (uint8_t)(1u << i);
    }
    if (memcmp(slots_, mhz, sizeof slots_) == 0) return true;
    memcpy(slots_, mhz, sizeof slots_);
    wantMask_ = mask;
    slotsDirty_ = true;
    return true;
}

bool Link::setGain(int gain) {
    if (gain < 0 || gain > kMaxGain) return false;
    if (gain != gain_) gain_ = gain, gainDirty_ = true;
    return true;
}

void Link::begin(uint32_t nowUs) {
    begun_ = true;
    send("Q");
    lastQUs_ = nowUs;
}

void Link::send(const char* payload) {
    char line[80];
    int n = snprintf(line, sizeof line, "%s*%02X\n", payload, xorSum(payload, strlen(payload)));
    if (n > 0 && n < (int)sizeof line) write_(ctx_, (const uint8_t*)line, (size_t)n);
}

void Link::sendSlots(uint32_t nowUs) {
    char p[4 + kSlots * 5 + 1] = "P";
    size_t k = 1;
    for (int i = 0; i < kSlots; ++i) k += (size_t)snprintf(p + k, sizeof p - k, ",%u", (unsigned)slots_[i]);
    send(p);
    slotsDirty_ = false;
    awaitingAck_ = true;
    lastPUs_ = nowUs;
    // Records for the old list may still arrive, and the C5 may have
    // rebooted: start the clock mapping and sequence check again.
    clockValid_ = false;
    seqValid_ = false;
}

void Link::tick(uint32_t nowUs) {
    if (!begun_) return;
    if (firmware_ >= 2) {
        // Send the list when it changes, and again while the C5 isn't
        // scanning it (it rebooted, or the P was lost).
        bool notScanning = wantMask_ && (!scanning_ || awaitingAck_);
        if (slotsDirty_ || (notScanning && nowUs - lastPUs_ >= kResendUs)) sendSlots(nowUs);
        if (gainDirty_) {
            char g[8];
            snprintf(g, sizeof g, "G,%d", gain_);
            send(g);
            gainDirty_ = false;
        }
    }
    // The C5 sends a status every 0.9 s on its own; asking as well finds it
    // quickly after a reconnect.
    if (nowUs - lastQUs_ >= (firmware_ >= 2 ? kStatusPollUs : kResendUs)) {
        send("Q");
        lastQUs_ = nowUs;
    }
}

bool Link::online(uint32_t nowUs) const { return heard_ && nowUs - lastHeardUs_ < kOnlineUs; }

void Link::feed(const uint8_t* data, size_t len, uint32_t nowUs) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (recState_ == 1) {
            // Length: 7 header bytes plus 4 per slot.
            if (b < 7 || b > 7 + 4 * kSlots) {
                ++n_.badRecords;
                recState_ = 0;
                continue;
            }
            recLen_ = b;
            rec_[0] = b;
            recPos_ = 0;
            recState_ = 2;
            continue;
        }
        if (recState_ == 2) {
            if (recPos_ < recLen_) {
                rec_[1 + recPos_++] = b;
                continue;
            }
            recState_ = 0;
            if (crc8(rec_, recLen_ + 1u) == b) onRecord(nowUs);
            else ++n_.badRecords;
            continue;
        }
        // 0xA5 never appears in a text line, so outside a record it always
        // starts one.
        if (b == kRecordSync) {
            recState_ = 1;
            lineLen_ = 0;
            lineOverflow_ = false;
            continue;
        }
        if (b == '\n') {
            if (!lineOverflow_ && lineLen_) onLine(nowUs);
            else if (lineOverflow_) ++n_.badLines;
            lineLen_ = 0;
            lineOverflow_ = false;
        } else if (lineLen_ < sizeof line_ - 1) {
            line_[lineLen_++] = (char)b;
        } else {
            lineOverflow_ = true;
        }
    }
}

void Link::onLine(uint32_t nowUs) {
    size_t n = lineLen_;
    if (n && line_[n - 1] == '\r') --n;
    line_[n] = 0;
    char* star = strrchr(line_, '*');
    if (!star || line_ + n - star != 3) {
        ++n_.badLines;
        return;
    }
    int h = hexDigit(star[1]), l = hexDigit(star[2]);
    if (h < 0 || l < 0 || xorSum(line_, (size_t)(star - line_)) != (uint8_t)(h * 16 + l)) {
        ++n_.badLines;
        return;
    }
    *star = 0;
    heard_ = true;
    lastHeardUs_ = nowUs;
    if (line_[0] != 'S' || line_[1] != ',') return;   // R lines are single-frequency mode only

    // S,<MHz>,<gain>,<state>,<firmware>
    char* f[5] = {};
    int k = 0;
    for (char* p = line_; p && k < 5; ++k) {
        f[k] = p;
        p = strchr(p, ',');
        if (p) *p++ = 0;
    }
    if (k < 4) {
        ++n_.badLines;
        return;
    }
    reportedGain_ = atoi(f[2]);
    snprintf(state_, sizeof state_, "%s", f[3]);
    firmware_ = k == 5 ? atoi(f[4]) : 1;
    bool wasScanning = scanning_;
    scanning_ = strcmp(state_, "SCAN") == 0;
    if (scanning_) awaitingAck_ = false;
    // A C5 that reports no list while we have one rebooted or lost it.
    if (wasScanning && !scanning_ && wantMask_) slotsDirty_ = true;
}

// rec_[0] = length, rec_[1..] = payload; the CRC is good.
void Link::onRecord(uint32_t nowUs) {
    const uint8_t* p = rec_ + 1;
    if (p[0] != 'M') return;
    heard_ = true;
    lastHeardUs_ = nowUs;
    uint8_t mask = p[6];
    int slots = 0;
    for (int i = 0; i < kSlots; ++i) slots += (mask >> i) & 1;
    if (recLen_ != 7 + 4 * slots) {
        ++n_.badRecords;
        return;
    }
    // Records from before the current slot list took effect.
    if (awaitingAck_ || mask != wantMask_) {
        ++n_.staleRecords;
        return;
    }
    ++n_.records;
    uint8_t seq = p[1];
    if (seqValid_ && seq != (uint8_t)(lastSeq_ + 1)) n_.seqGaps += (uint8_t)(seq - lastSeq_ - 1);
    lastSeq_ = seq;
    seqValid_ = true;

    // C5 clock to ours. Each record arrives some time after the C5 took it;
    // the smallest (arrival - C5 time) seen is the best estimate of the
    // offset. A smaller one replaces it at once; otherwise it creeps up 1 us
    // every 16 records (~60 us/s), faster than any crystal drift, so it
    // tracks drift both ways. 8 records in a row more than 20 ms late: the C5
    // clock jumped (it rebooted), start again.
    uint32_t t0 = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
    uint32_t candidate = nowUs - t0;
    int32_t late = (int32_t)(candidate - clockOffset_);
    if (!clockValid_ || late < 0) {
        clockOffset_ = candidate;
        clockValid_ = true;
        clockLateRun_ = 0;
    } else {
        if (++clockLeak_ >= 16) clockLeak_ = 0, ++clockOffset_;
        clockLateRun_ = late > 20000 ? (uint8_t)(clockLateRun_ + 1) : 0;
        if (clockLateRun_ >= 8) clockValid_ = false;
    }

    const uint8_t* s = p + 7;
    for (int i = 0; i < kSlots; ++i) {
        if (!(mask & (1u << i))) continue;
        uint16_t value = (uint16_t)(s[0] | s[1] << 8);
        uint16_t dt = (uint16_t)(s[2] | s[3] << 8);
        s += 4;
        if (value & 0x8000) {   // the C5's capture failed for this slot
            ++n_.failedDumps;
            continue;
        }
        ++n_.samples;
        Sample out = {(uint8_t)i, (uint16_t)(value & 0x3FF), t0 + dt + clockOffset_};
        if (onSample_) onSample_(ctx_, out);
    }
}

bool GateDetector::push(uint16_t rssi, uint32_t timeUs, uint32_t* passUs) {
    // A light EMA (3/4 old) on top of the C5's own median-of-3 and EMA.
    f_ = valid_ ? (uint16_t)((3u * f_ + rssi) / 4u) : rssi;
    valid_ = true;
    if (!inside_) {
        if (f_ < enter_) return false;
        inside_ = true;
        peak_ = f_;
        peakUs_ = timeUs;
        return false;
    }
    if (f_ > peak_) peak_ = f_, peakUs_ = timeUs;
    if (f_ > exit_) return false;
    inside_ = false;
    if (passUs) *passUs = peakUs_;
    return true;
}

}  // namespace c5host
