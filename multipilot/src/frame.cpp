#include "frame.h"

#include <string.h>

namespace mp {

uint16_t crc16x25(const uint8_t* p, size_t n, uint16_t crc, bool finish) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return finish ? (uint16_t)~crc : crc;
}

size_t frameEncode(uint8_t type, const void* payload, uint16_t len, uint8_t* out, size_t cap) {
    size_t total = (size_t)len + kFrameOverhead;
    if (cap < total) return 0;
    out[0] = kSync0;
    out[1] = kSync1;
    out[2] = type;
    out[3] = (uint8_t)(len & 0xFF);
    out[4] = (uint8_t)(len >> 8);
    if (len) memcpy(out + 5, payload, len);
    uint16_t crc = crc16x25(out + 2, 3u + len);
    out[5 + len] = (uint8_t)(crc & 0xFF);
    out[6 + len] = (uint8_t)(crc >> 8);
    return total;
}

}  // namespace mp
