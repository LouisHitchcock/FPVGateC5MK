#include "node_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace node {

uint8_t xorSum(const char* p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) x ^= (uint8_t)p[i];
    return x;
}

uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

size_t formatLine(const char* payload, char* out, size_t cap) {
    size_t n = strlen(payload);
    if (n + 5 > cap) return 0;   // payload, '*', 2 hex, '\n', NUL
    static const char hex[] = "0123456789ABCDEF";
    uint8_t x = xorSum(payload, n);
    memcpy(out, payload, n);
    out[n] = '*';
    out[n + 1] = hex[x >> 4];
    out[n + 2] = hex[x & 15];
    out[n + 3] = '\n';
    out[n + 4] = 0;
    return n + 4;
}

static int hexDigit(char c) {
    // Upper case only, as the S3 sends; anything else rejects the line.
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char* LineReader::feed(char c) {
    if (c != '\n') {
        if (len_ < kMaxLine) buf_[len_++] = c;
        else overflowed_ = true;
        return nullptr;
    }
    size_t n = len_;
    len_ = 0;
    if (overflowed_) {
        overflowed_ = false;
        ++overflow;
        return nullptr;
    }
    if (n && buf_[n - 1] == '\r') --n;
    if (n == 0) return nullptr;
    buf_[n] = 0;
    char* star = strrchr(buf_, '*');
    if (!star || (size_t)(buf_ + n - star) != 3) {
        ++bad;
        return nullptr;
    }
    int h = hexDigit(star[1]), l = hexDigit(star[2]);
    if (h < 0 || l < 0 || xorSum(buf_, (size_t)(star - buf_)) != (uint8_t)(h * 16 + l)) {
        ++bad;
        return nullptr;
    }
    *star = 0;
    ++good;
    return buf_;
}

const char* stateName(State s) {
    switch (s) {
        case State::Tuning: return "TUNING";
        case State::Ok: return "OK";
        case State::ErrRf: return "ERR_RF";
        case State::Scan: return "SCAN";
        case State::ErrFreq:
        case State::NoFreq:
        default: return "ERR_FREQ";   // no valid frequency (MHz 0 when none was ever set)
    }
}

// Digits only, no sign or spaces; false if empty, malformed or too big.
static bool parseUint(const char* s, int* out) {
    if (!*s) return false;
    long v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
        if (v > 100000) return false;
    }
    *out = (int)v;
    return true;
}

void Core::boot(uint32_t nowUs, int gain, bool radioOk) {
    gain_ = gain;
    state_ = radioOk ? State::NoFreq : State::ErrRf;
    status(nowUs);
}

void Core::onByte(char c, uint32_t nowUs) {
    if (const char* p = reader.feed(c)) handle(p, nowUs);
}

void Core::handle(const char* p, uint32_t nowUs) {
    ++commands;
    int v;
    if (!strcmp(p, "Q")) {
        status(nowUs);
    } else if (p[0] == 'F' && p[1] == ',' && parseUint(p + 2, &v)) {
        mhz_ = v;
        scanMask_ = 0;
        needGain_ = false;
        if (v < kMinMhz || v > kMaxMhz) {
            state_ = State::ErrFreq;
            needTune_ = false;
        } else {
            state_ = State::Tuning;
            needTune_ = true;
        }
        status(nowUs);
    } else if (p[0] == 'G' && p[1] == ',' && parseUint(p + 2, &v)) {
        if (v > radio_.gainMax()) {
            ++rejected;   // keep the old gain; the status shows which is in force
        } else {
            gain_ = v;
            if (state_ == State::Ok || state_ == State::Tuning) {
                state_ = State::Tuning;
                if (!needTune_) needGain_ = true;
            }
        }
        status(nowUs);
    } else if (p[0] == 'P' && p[1] == ',' && parseSlots(p + 2)) {
        mhz_ = 0;
        needTune_ = needGain_ = false;
        failRun_ = 0;
        for (auto& f : scanFilt_) f.reset();
        state_ = scanMask_ ? State::Scan : State::NoFreq;
        status(nowUs);
    } else {
        ++rejected;
    }
}

// "5658,0,5732": one entry per slot, 0 = off. All or nothing: a bad entry
// leaves the old slots in force.
bool Core::parseSlots(const char* s) {
    uint16_t mhz[kMaxSlots] = {};
    uint8_t mask = 0;
    int n = 0;
    for (;;) {
        if (n == kMaxSlots) return false;
        char num[8];
        size_t len = 0;
        while (*s && *s != ',') {
            if (len + 1 >= sizeof num) return false;
            num[len++] = *s++;
        }
        num[len] = 0;
        int v;
        if (!parseUint(num, &v)) return false;
        if (v) {
            if (v < kMinMhz || v > kMaxMhz) return false;
            mhz[n] = (uint16_t)v;
            mask |= (uint8_t)(1u << n);
        }
        ++n;
        if (!*s) break;
        ++s;   // the comma
    }
    memcpy(scanMhz_, mhz, sizeof mhz);
    scanMask_ = mask;
    return true;
}

// One pass over the slots, one record. A command waiting at the start skips
// the cycle so it is handled first. One arriving part way through waits for
// the end of the cycle (about 1 ms): the record is still for the old slot
// list, and the S3 drops records until its new list is acknowledged.
void Core::scanCycle(uint32_t nowUs) {
    if (io_.rxPending()) {
        ++abortedCycles;
        return;
    }
    uint8_t rec[2 + 7 + 4 * kMaxSlots + 1];
    uint8_t* pl = rec + 2;
    size_t k = 7;
    uint32_t t0 = 0;
    bool first = true;
    // The USB view only shows the latest value at 50 Hz.
    const bool view = onSample && (cycleSeq_ & (kViewEvery - 1)) == 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (!(scanMask_ & (1u << i))) continue;
        radio_.hop(scanMhz_[i], gain_);
        float db = 0;
        uint32_t t = nowUs;
        uint16_t value;
        float f;
        if (radio_.measure(&db, &t)) {
            failRun_ = 0;
            f = scanFilt_[i].push(db, alpha_);
            value = rssiFromDb(f);
            ++samples;
        } else {
            ++failedDumps;
            f = scanFilt_[i].value();
            value = (uint16_t)(kValueFailed | rssiFromDb(f));
            if (++failRun_ >= kFailsForErrRf * kMaxSlots) {
                state_ = State::ErrRf;
                status(nowUs);
                return;
            }
        }
        if (first) t0 = t, first = false;
        uint32_t dt = t - t0;
        if (dt > 0xFFFF) dt = 0xFFFF;
        pl[k++] = (uint8_t)value;
        pl[k++] = (uint8_t)(value >> 8);
        pl[k++] = (uint8_t)dt;
        pl[k++] = (uint8_t)(dt >> 8);
        if (view && !(value & kValueFailed)) onSample(scanMhz_[i], db, f, value, t);
    }
    pl[0] = (uint8_t)kRecordScan;
    pl[1] = cycleSeq_++;
    memcpy(pl + 2, &t0, 4);   // little-endian on both chips and the PC
    pl[6] = scanMask_;
    rec[0] = kRecordSync;
    rec[1] = (uint8_t)k;
    rec[2 + k] = crc8(rec + 1, k + 1);
    io_.write((const char*)rec, k + 3);
    ++cycles;
}

void Core::step(uint32_t nowUs) {
    if (state_ == State::Scan) scanCycle(nowUs);
    bool becameReady = false;
    if (state_ == State::Tuning) {
        if (needTune_) {
            needTune_ = false;
            needGain_ = false;
            if (!radio_.tune(mhz_, gain_)) {
                state_ = State::ErrRf;
                status(nowUs);
                return;
            }
        }
        if (needGain_) {
            needGain_ = false;
            radio_.setGain(gain_);
        }
        // Let a command that follows straight on (the S3 sends F then G)
        // take effect before reporting OK.
        if (io_.rxPending()) return;
        state_ = State::Ok;
        filt_.reset();
        failRun_ = 0;
        nextSampleUs_ = nowUs;
        status(nowUs);
        becameReady = true;
    }
    if (state_ == State::Ok && (becameReady || (int32_t)(nowUs - nextSampleUs_) >= 0)) {
        float db;
        uint32_t tUs;
        if (!radio_.measure(&db, &tUs)) {
            ++failedDumps;
            if (++failRun_ >= kFailsForErrRf) {
                state_ = State::ErrRf;
                status(nowUs);
            }
        } else if (io_.rxPending()) {
            ++staleDropped;   // a command is waiting: it may be a retune
        } else {
            failRun_ = 0;
            float f = filt_.push(db, alpha_);
            uint16_t r = rssiFromDb(f);
            char payload[24];
            snprintf(payload, sizeof payload, "R,%u,%u", (unsigned)seq_, (unsigned)r);
            send(payload);
            ++seq_;
            ++samples;
            if (onSample) onSample(mhz_, db, f, r, nowUs);
        }
        nextSampleUs_ += kSampleUs;
        if ((int32_t)(nowUs - nextSampleUs_) > (int32_t)(2 * kSampleUs)) nextSampleUs_ = nowUs + kSampleUs;
    }
    if (nowUs - lastStatusUs_ >= kHeartbeatUs) status(nowUs);
}

void Core::setMap(float dbLo, float dbHi) {
    if (dbHi - dbLo < 1.0f) return;
    dbLo_ = dbLo;
    dbHi_ = dbHi;
}

uint16_t Core::rssiFromDb(float db) const {
    float v = (db - dbLo_) * 1023.0f / (dbHi_ - dbLo_);
    if (!(v > 0)) return 0;   // also catches NaN
    if (v > 1023) return 1023;
    return (uint16_t)lroundf(v);
}

void Core::status(uint32_t nowUs) {
    char payload[48];
    snprintf(payload, sizeof payload, "S,%d,%d,%s,%s", mhz_, gain_, stateName(state_), kFirmware);
    send(payload);
    lastStatusUs_ = nowUs;
    ++statuses;
}

void Core::send(const char* payload) {
    char line[kMaxLine + 8];
    size_t n = formatLine(payload, line, sizeof line);
    if (n) io_.write(line, n);
}

}  // namespace node
