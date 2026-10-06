// Host link framing. Portable (the PC tests use it).
//
// Every byte the C5 sends over native USB is inside a frame:
//   A5 5A | type | length (u16 LE) | payload | CRC-16 (u16 LE)
// The CRC is CRC-16/X-25 (reflected 0x1021, init 0xFFFF, xorout 0xFFFF)
// over type, length and payload. The host sends plain text lines.
#ifndef MP_FRAME_H
#define MP_FRAME_H

#include <stddef.h>
#include <stdint.h>

namespace mp {

constexpr uint8_t kSync0 = 0xA5, kSync1 = 0x5A;
constexpr size_t kFrameOverhead = 7;
constexpr uint8_t kTypeText = 'T', kTypeRssi = 'R', kTypeCapture = 'C';

uint16_t crc16x25(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF, bool finish = true);

// Writes the frame into out; returns its size, or 0 if cap is too small.
size_t frameEncode(uint8_t type, const void* payload, uint16_t len, uint8_t* out, size_t cap);

// The 'R' record: one per scan cycle.
#pragma pack(push, 1)
struct RssiHeader {
    uint16_t seq;
    uint8_t count;       // pilots in this record
    uint8_t flags;       // kRecSettingsChanged
    uint32_t drops;      // frames dropped so far (host stalled)
    uint32_t cycleUs;    // this cycle's length
};
struct RssiPilot {
    uint32_t tUs;        // esp_timer at the middle of the dump
    int16_t rawCdb;      // centi-dB
    int16_t filtCdb;
    uint16_t mhz;
    uint8_t gain;
    uint8_t clips;       // saturating
    uint8_t flags;       // kPilot*
    uint8_t pad;
};
// 'C' chunk header, followed by `count` 32-bit sample words.
struct CaptureHeader {
    uint16_t id;
    uint16_t loMhz;
    uint8_t gain;
    uint8_t bw;
    uint16_t total;      // words in the whole capture
    uint16_t first;      // index of this chunk's first word
    uint16_t count;
};
#pragma pack(pop)

static_assert(sizeof(RssiHeader) == 12, "RssiHeader layout");
static_assert(sizeof(RssiPilot) == 14, "RssiPilot layout");
static_assert(sizeof(CaptureHeader) == 12, "CaptureHeader layout");

constexpr uint8_t kRecSettingsChanged = 1;
constexpr uint8_t kRecNodeMode = 2;   // FPVGate node mode: latest reading per frequency
constexpr uint8_t kPilotDumpFailed = 1, kPilotGainMoved = 2, kPilotClipped = 4, kPilotFallback = 8;

}  // namespace mp

#endif
