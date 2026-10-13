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
        case State::Update: return "UPDATE";
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

void Core::setIdentity(const char* version, const char* board) {
    snprintf(version_, sizeof version_, "%s", version);
    snprintf(board_, sizeof board_, "%s", board);
}

void Core::boot(uint32_t nowUs, int gain, bool radioOk) {
    gain_ = gain;
    state_ = radioOk ? State::NoFreq : State::ErrRf;
    status(nowUs);
}

// Lines and update chunks share the input: a chunk starts with a byte no
// line contains, and its payload never reaches the line reader.
void Core::onByte(char ch, uint32_t nowUs) {
    const uint8_t c = (uint8_t)ch;
    if (chunkState_ == 1) {
        if (c < 5 || c > kChunkMax + 5) {
            chunkState_ = 0;
            ++badChunks;
            return;
        }
        chunk_[0] = c;   // the CRC covers the length too
        chunkLen_ = c;
        chunkPos_ = 0;
        chunkState_ = 2;
        return;
    }
    if (chunkState_ == 2) {
        if (chunkPos_ < chunkLen_) {
            chunk_[1 + chunkPos_++] = c;
            return;
        }
        chunkState_ = 0;
        if (crc8(chunk_, chunkLen_ + 1u) == c) onChunk(nowUs);
        else ++badChunks;
        return;
    }
    if (c == kChunkSync) {
        chunkState_ = 1;
        return;
    }
    if (const char* p = reader.feed(ch)) handle(p, nowUs);
}

void Core::handle(const char* p, uint32_t nowUs) {
    ++commands;
    int v;
    const bool updateCmd = (p[0] == 'U' && p[1] == ',') || !strcmp(p, "E") || !strcmp(p, "A");
    if (state_ == State::Update && !updateCmd && strcmp(p, "Q") != 0) {
        ++rejected;   // no retuning while an update is open
        status(nowUs);
        return;
    }
    if (!strcmp(p, "Q")) {
        status(nowUs);
    } else if (p[0] == 'U' && p[1] == ',') {
        startUpdate(p + 2, nowUs);
    } else if (!strcmp(p, "E")) {
        endUpdate(true, nowUs);
    } else if (!strcmp(p, "A")) {
        endUpdate(false, nowUs);
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

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "<bytes>,<sha256 as 64 hex digits>"
void Core::startUpdate(const char* s, uint32_t nowUs) {
    uint32_t size = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9' && digits < 8; ++s, ++digits) size = size * 10 + (uint32_t)(*s - '0');
    uint8_t sha[32];
    bool ok = digits > 0 && size > 0 && *s == ',';
    if (ok) {
        ++s;
        for (int i = 0; i < 32 && ok; ++i) {
            int h = hexNibble(s[2 * i]), l = h < 0 ? -1 : hexNibble(s[2 * i + 1]);
            if (l < 0) ok = false;
            else sha[i] = (uint8_t)(h * 16 + l);
        }
        ok = ok && s[64] == 0;
    }
    if (!update_ || !ok) {
        ++rejected;
        send(update_ ? "u,err,args" : "u,err,unsupported");
        return;
    }
    if (state_ == State::Update) update_->abort();   // the S3 started again
    scanMask_ = 0;
    mhz_ = 0;
    needTune_ = needGain_ = false;
    state_ = State::Update;
    status(nowUs);
    if (!update_->begin(size, sha)) {
        state_ = State::NoFreq;
        send("u,err,begin");
        status(nowUs);
        return;
    }
    updateSize_ = size;
    updateDone_ = 0;
    updateLastUs_ = nowUs;
    send("u,ready");
}

// A chunk at the next offset is written. A repeat of one already written
// (its acknowledgement was lost) is acknowledged again; any other offset is
// answered with the one wanted, and the S3 carries on from there.
void Core::onChunk(uint32_t nowUs) {
    const uint8_t* pl = chunk_ + 1;
    const size_t n = chunkLen_;
    if (state_ != State::Update || pl[0] != (uint8_t)kChunkWrite) {
        ++badChunks;
        return;
    }
    uint32_t offset;
    memcpy(&offset, pl + 1, 4);   // little-endian on both chips and the PC
    const size_t len = n - 5;
    if (offset == updateDone_ && len && len <= updateSize_ - updateDone_) {
        if (!update_->write(pl + 5, len)) {
            update_->abort();
            state_ = State::NoFreq;
            send("u,err,write");
            status(nowUs);
            return;
        }
        updateDone_ += (uint32_t)len;
        ++chunks;
    }
    updateLastUs_ = nowUs;
    char reply[24];
    snprintf(reply, sizeof reply, "u,ack,%lu", (unsigned long)updateDone_);
    send(reply);
}

void Core::endUpdate(bool finish, uint32_t nowUs) {
    if (state_ != State::Update) {
        ++rejected;
        send("u,err,idle");
        return;
    }
    state_ = State::NoFreq;
    if (!finish) {
        update_->abort();
        send("u,err,aborted");
        status(nowUs);
        return;
    }
    // finish() cleans up after itself when the image is bad.
    if (updateDone_ != updateSize_ || !update_->finish()) {
        if (updateDone_ != updateSize_) update_->abort();
        send("u,err,verify");
        status(nowUs);
        return;
    }
    send("u,done");
    update_->restart();
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
        if (onScanSample && !(value & kValueFailed)) onScanSample((unsigned)i, value, t);
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
    if (state_ == State::Update && nowUs - updateLastUs_ >= kUpdateIdleUs) {
        update_->abort();   // the S3 went away part way through
        state_ = State::NoFreq;
        send("u,err,timeout");
        status(nowUs);
    }
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
    char payload[80];
    snprintf(payload, sizeof payload, "S,%d,%d,%s,%s,%s,%s", mhz_, gain_, stateName(state_), kFirmware, version_,
             board_);
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
