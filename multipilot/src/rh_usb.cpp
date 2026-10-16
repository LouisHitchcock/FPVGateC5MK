#include "rh_usb.h"

#include <string.h>

namespace rhusb {
namespace {
const char kBuildDate[16] = __DATE__;
const char kBuildTime[16] = __TIME__;
uint16_t boundedAge(uint32_t now, uint32_t then) {
    uint32_t age = now - then;
    return age > 65535 ? 65535 : (uint16_t)age;
}
void put16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}
// RH turns a seat off by writing this "power down" frequency (API 24 and
// later), or 0, and reads it back to check the write.
constexpr uint16_t kSeatOffMhz = 1111;
// A frequency the C5 tunes to; anything else (0 or kSeatOffMhz) is a seat that is off.
bool tuned(uint16_t mhz) { return mhz >= 5180 && mhz <= 5917; }
}  // namespace

void Core::begin(uint32_t nowMs) {
    selected_ = command_ = needed_ = received_ = 0;
    lastCommandMs_ = lastByteMs_ = nowMs;
    for (auto& seat : seats_) seat = Seat{};
    const uint16_t raceband[kSeats] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    for (unsigned i = 0; i < kSeats; ++i) seats_[i].frequency = raceband[i];
    // RH requires nonzero frequencies while discovering its hardware interface.
    port_.frequencies(raceband);
    settleAll(nowMs);
    memset(stats_, 0, sizeof stats_);
}

void Core::settleAll(uint32_t nowMs) {
    // Every frequency change restarts the scan of all seats with empty filters.
    for (auto& seat : seats_) {
        stopCrossing(seat);
        seat.quiet = seat.quietSeen = false;
        seat.settleUntilMs = nowMs + kSettleMs;
    }
}

uint8_t Core::shownRssi(const Seat& seat, uint32_t nowMs) {
    uint32_t now = nowMs / kShowBucketMs;
    uint8_t shown = 0;
    for (unsigned i = 0; i < kShowBuckets; ++i)
        if (seat.bucketUsed[i] && now - seat.bucketId[i] < kShowBuckets && seat.bucketMax[i] > shown)
            shown = seat.bucketMax[i];
    return shown ? shown : seat.rssi;
}

void Core::end() {
    needed_ = received_ = 0;
    for (auto& seat : seats_) stopCrossing(seat);
}

void Core::setIdentity(const char* version, const char* board) {
    memset(version_, 0, sizeof version_);
    memset(board_, 0, sizeof board_);
    // RH firmware strings are fixed 16-byte blocks, with optional trailing NULs.
    strncpy(version_, version, sizeof version_ - 1);
    strncpy(board_, board, sizeof board_ - 1);
}

bool Core::expired(uint32_t nowMs) const {
    return nowMs - lastCommandMs_ >= kIdleTimeoutMs;
}

void Core::feed(uint8_t byte, uint32_t nowMs) {
    if (needed_ && nowMs - lastByteMs_ >= kCommandTimeoutMs) needed_ = received_ = 0;
    lastByteMs_ = nowMs;
    if (needed_) {
        input_[received_++] = byte;
        if (received_ == needed_) {
            uint8_t sum = 0;
            for (unsigned i = 0; i + 1 < needed_; ++i) sum += input_[i];
            if (sum == input_[needed_ - 1]) applyWrite(nowMs);
            needed_ = received_ = 0;
        }
        return;
    }
    command_ = byte;
    switch (byte) {
        case WRITE_FREQUENCY: case SEND_STATUS_MESSAGE: needed_ = 3; break;
        case WRITE_ENTER_AT_LEVEL: case WRITE_EXIT_AT_LEVEL:
        case WRITE_CURNODE_INDEX: case FORCE_END_CROSSING: needed_ = 2; break;
        default: read(byte, nowMs); break;
    }
}

void Core::reply(const uint8_t* data, size_t size) {
    uint8_t buffer[17], sum = 0;
    if (size > sizeof buffer - 1) return;
    for (size_t i = 0; i < size; ++i) sum += buffer[i] = data[i];
    buffer[size] = sum;
    port_.write(buffer, size + 1);
}

void Core::latchStats(uint32_t nowMs) {
    Seat& seat = seats_[selected_];
    bool stale = seat.sampled && nowMs - seat.sampleMs > 100;
    if (stale) {
        stopCrossing(seat);
        seat.rssi = 1;
        seat.windowed = false;
    }
    stats_[0] = seat.lap;
    put16(stats_ + 1, boundedAge(nowMs, seat.passMs));
    // RH's current RSSI (its live graph and Capture): the rolling maximum,
    // or 1 once the samples have stopped.
    stats_[3] = stale ? 1 : shownRssi(seat, nowMs);
    stats_[4] = seat.peak;
    stats_[5] = seat.passPeak;
    put16(stats_ + 6, seat.loopUs);
    if (seat.shownCrossing && (int32_t)(nowMs - seat.shownUntilMs) >= 0) seat.shownCrossing = false;
    stats_[8] = seat.crossing || seat.shownCrossing ? 1 : 0;
    stats_[9] = seat.passNadir;
    stats_[10] = seat.nadir;
    // History: one extremum of the samples since the last poll, at its actual
    // sample times, as RH's own nodes report it: the peak if it rose further
    // from the extremum shown last than the nadir fell, else the nadir. A
    // short pass between polls then reaches RH's graph at its true height.
    if (seat.windowed) {
        bool peak = seat.windowMax - seat.shown >= seat.shown - seat.windowMin;
        uint32_t firstMs = peak ? seat.maxFirstMs : seat.minFirstMs;
        uint32_t lastMs = peak ? seat.maxLastMs : seat.minLastMs;
        seat.shown = peak ? seat.windowMax : seat.windowMin;
        if (peak) stats_[8] |= 2;
        stats_[11] = seat.shown;
        put16(stats_ + 12, boundedAge(nowMs, firstMs));
        put16(stats_ + 14, boundedAge(lastMs, firstMs));  // how long it held
        seat.windowed = false;
    } else {
        // No samples since the last poll: the latest (or stale) level.
        stats_[8] |= 2;
        stats_[11] = seat.rssi;
        put16(stats_ + 12, boundedAge(nowMs, seat.sampleMs));
        put16(stats_ + 14, 0);
    }
}

void Core::read(uint8_t command, uint32_t nowMs) {
    const Seat& seat = seats_[selected_];
    uint8_t out[4] = {};
    const uint8_t* data = out;
    size_t size = 1;
    switch (command) {
        case READ_REVISION_CODE: out[0] = 0x25; out[1] = 36; size = 2; break;
        case READ_MULTINODE_COUNT: out[0] = kSeats; break;
        case READ_CURNODE_INDEX: case READ_NODE_SLOTIDX: out[0] = selected_; break;
        case READ_FREQUENCY: put16(out, seat.frequency); size = 2; break;
        // API 36: after a frequency write, nonzero means the receiver took it.
        case TEST_RX_REGISTER: out[0] = tuned(seat.frequency) ? 1 : 0; break;
        case READ_RHFEAT_FLAGS: put16(out, 0); size = 2; break;  // no STM32/bootloader features
        case READ_NODE_RSSI_PEAK: out[0] = seat.peak; break;
        case READ_NODE_RSSI_NADIR: out[0] = seat.nadir; break;
        case READ_ENTER_AT_LEVEL: out[0] = seat.enter; break;
        case READ_EXIT_AT_LEVEL: out[0] = seat.exit; break;
        case READ_TIME_MILLIS:
            out[0] = (uint8_t)(nowMs >> 24); out[1] = (uint8_t)(nowMs >> 16);
            out[2] = (uint8_t)(nowMs >> 8); out[3] = (uint8_t)nowMs; size = 4; break;
        case READ_LAP_STATS: case READ_LAP_PASS_STATS:
            latchStats(nowMs);
            data = stats_; size = command == READ_LAP_STATS ? 16 : 8; break;
        case READ_LAP_EXTREMUMS:
            // The API 35 host reads PASS_STATS then EXTREMUMS. Keep both halves coherent.
            data = stats_ + 8; size = 8; break;
        case READ_FW_VERSION: data = (const uint8_t*)version_; size = 16; break;
        case READ_FW_PROCTYPE: data = (const uint8_t*)board_; size = 16; break;
        case READ_FW_BUILDDATE: data = (const uint8_t*)kBuildDate; size = 16; break;
        case READ_FW_BUILDTIME: data = (const uint8_t*)kBuildTime; size = 16; break;
        default: return;
    }
    reply(data, size);
    lastCommandMs_ = nowMs;
}

void Core::applyWrite(uint32_t nowMs) {
    Seat& seat = seats_[selected_];
    switch (command_) {
        case WRITE_CURNODE_INDEX:
            if (input_[0] >= kSeats) return;
            selected_ = input_[0];
            memset(stats_, 0, sizeof stats_);
            break;
        case WRITE_FREQUENCY: {
            uint16_t frequency = (uint16_t)((input_[0] << 8) | input_[1]);
            if (frequency && frequency != kSeatOffMhz && !tuned(frequency)) return;
            if (frequency != seat.frequency) {
                uint8_t lap = seat.lap, enter = seat.enter, exit = seat.exit;
                seat = Seat{};
                seat.lap = lap; seat.enter = enter; seat.exit = exit;
                seat.frequency = frequency;  // kept as written, for RH's read-back
                uint16_t slots[kSeats];
                for (unsigned i = 0; i < kSeats; ++i) slots[i] = tuned(seats_[i].frequency) ? seats_[i].frequency : 0;
                settleAll(nowMs);
                port_.frequencies(slots);
            }
            break;
        }
        case WRITE_ENTER_AT_LEVEL: case WRITE_EXIT_AT_LEVEL:
            if (input_[0] == 0 || input_[0] == 255) return;
            if (command_ == WRITE_ENTER_AT_LEVEL) seat.enter = input_[0]; else seat.exit = input_[0];
            stopCrossing(seat);
            break;
        case FORCE_END_CROSSING: stopCrossing(seat); break;
        case SEND_STATUS_MESSAGE: break;  // server status is a keepalive, not a radio setting
        default: return;
    }
    lastCommandMs_ = nowMs;
}

void Core::sample(unsigned index, uint16_t rawRssi, uint32_t sampleMs) {
    if (index >= kSeats || rawRssi > 1023) return;
    Seat& seat = seats_[index];
    if (!tuned(seat.frequency)) return;
    uint8_t level = (uint8_t)(rawRssi / 4);
    if (level < 1) level = 1;
    if (level > 254) level = 254;
    if (seat.sampled) {
        uint32_t elapsed = sampleMs - seat.sampleMs;
        seat.loopUs = elapsed > 65 ? 65535 : (uint16_t)(elapsed * 1000);
        // No crossing may span an RF outage or a missing run of captures.
        if (elapsed > 100) {
            stopCrossing(seat);
            seat.quiet = false;
        }
    }
    seat.sampled = true;
    seat.sampleMs = sampleMs;
    // Settling after a frequency change: shown, but not a pass or a node peak.
    if ((int32_t)(sampleMs - seat.settleUntilMs) < 0) {
        show(seat, level, sampleMs, sampleMs);
        stopCrossing(seat);
        return;
    }
    detect(seat, level, sampleMs);
    if (seat.jumping) return;  // not shown unless it lasts (see detect)
    show(seat, level, sampleMs, sampleMs);
    if (level > seat.peak) seat.peak = level;
}

void Core::show(Seat& seat, uint8_t level, uint32_t firstMs, uint32_t lastMs) {
    seat.rssi = level;
    if (!seat.windowed) {
        seat.windowed = true;
        seat.windowMax = seat.windowMin = level;
        seat.maxFirstMs = seat.minFirstMs = firstMs;
        seat.maxLastMs = seat.minLastMs = lastMs;
    } else {
        if (level > seat.windowMax) {
            seat.windowMax = level;
            seat.maxFirstMs = firstMs;
            seat.maxLastMs = lastMs;
        } else if (level == seat.windowMax) {
            seat.maxLastMs = lastMs;
        }
        if (level < seat.windowMin) {
            seat.windowMin = level;
            seat.minFirstMs = firstMs;
            seat.minLastMs = lastMs;
        } else if (level == seat.windowMin) {
            seat.minLastMs = lastMs;
        }
    }
    uint32_t bucket = firstMs / kShowBucketMs;
    unsigned slot = bucket % kShowBuckets;
    // A held jump is shown late, at its own time: never over a newer bucket.
    if (seat.bucketUsed[slot] && seat.bucketId[slot] > bucket) return;
    if (!seat.bucketUsed[slot] || seat.bucketId[slot] != bucket) {
        seat.bucketUsed[slot] = true;
        seat.bucketId[slot] = bucket;
        seat.bucketMax[slot] = level;
    } else if (level > seat.bucketMax[slot]) {
        seat.bucketMax[slot] = level;
    }
}

void Core::detect(Seat& seat, uint8_t level, uint32_t sampleMs) {
    if (level < seat.nadir) seat.nadir = level;
    bool floor = level <= seat.nadir + reject_.margin;
    // A jump: off a floor held for quietMs to twice the margin above it, or to
    // EnterAt if that is lower, within riseMs.
    int jumpLevel = seat.nadir + 2 * reject_.margin;
    if (seat.enter < jumpLevel) jumpLevel = seat.enter;
    if (!seat.jumping && !floor && reject_.riseMs && seat.quietSeen &&
        seat.quietLastMs - seat.quietStartMs >= reject_.quietMs &&
        sampleMs - seat.quietLastMs <= reject_.riseMs && level >= jumpLevel) {
        seat.jumping = true;
        seat.jumpDropped = false;
        seat.jumpUntilMs = sampleMs + reject_.holdMs;
        seat.jumpMax = 0;
    }
    if (floor) {
        if (!seat.quiet) seat.quietStartMs = sampleMs;
        seat.quiet = seat.quietSeen = true;
        seat.quietLastMs = sampleMs;
    } else {
        seat.quiet = false;
    }
    if (seat.jumping) {
        if (level > seat.jumpMax) {
            seat.jumpMax = level;
            seat.jumpFirstMs = seat.jumpLastMs = sampleMs;
        } else if (level == seat.jumpMax) {
            seat.jumpLastMs = sampleMs;
        }
        if (floor) {
            // Back on the floor within holdMs: a VTX starting up or changing
            // channel. RH never sees it, not even on its graph.
            seat.jumping = false;
        } else if ((int32_t)(sampleMs - seat.jumpUntilMs) >= 0) {
            // It lasted: a real signal. Show its peak, held back until now.
            seat.jumping = false;
            if (!seat.jumpDropped) {
                show(seat, seat.jumpMax, seat.jumpFirstMs, seat.jumpLastMs);
                if (seat.jumpMax > seat.peak) seat.peak = seat.jumpMax;
            }
        }
    }
    if (seat.enter <= seat.exit) { stopCrossing(seat); return; }
    if (!seat.crossing && !seat.held) {
        if (level < seat.enter) return;
        if (seat.jumping) {
            seat.held = true;
            seat.keptPeak = seat.passPeak;
            seat.keptNadir = seat.passNadir;
        } else {
            seat.crossing = true;
        }
        seat.passPeak = level;
        seat.passNadir = level;
        seat.firstPeakMs = seat.lastPeakMs = sampleMs;
    } else {
        if (level > seat.passPeak) {
            seat.passPeak = level;
            seat.firstPeakMs = seat.lastPeakMs = sampleMs;
        } else if (level == seat.passPeak) {
            seat.lastPeakMs = sampleMs;
        }
        if (level < seat.passNadir) seat.passNadir = level;
        if (level < seat.exit && seat.held) {
            // A jump that ended within holdMs is not a pass, nor shown.
            seat.held = false;
            seat.jumpDropped = true;
            seat.passPeak = seat.keptPeak;
            seat.passNadir = seat.keptNadir;
        } else if (level < seat.exit) {
            seat.crossing = false;
            seat.shownCrossing = true;
            seat.shownUntilMs = sampleMs + kShowCrossingMs;
            seat.passMs = seat.firstPeakMs + (seat.lastPeakMs - seat.firstPeakMs) / 2;
            ++seat.lap;
        } else if (seat.held && !seat.jumping) {
            // The jump lasted (and was shown above): a normal crossing.
            seat.held = false;
            seat.crossing = true;
            if (seat.jumpDropped) show(seat, seat.passPeak, seat.firstPeakMs, seat.lastPeakMs);
        }
    }
}

}  // namespace rhusb
