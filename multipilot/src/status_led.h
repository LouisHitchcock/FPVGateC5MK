#pragma once
#include <stdint.h>

namespace statusled {
// Pure pattern logic, shared with the portable tests. Error takes priority.
inline bool level(uint32_t ms, bool connected, bool traffic, bool error, bool enabled) {
    if (!enabled) return false;
    if (error) return ms % 1500 < 100 || (ms % 1500 >= 200 && ms % 1500 < 300);
    if (traffic) return ms % 200 < 100;
    return connected || ms % 1000 < 200;
}
void begin();
// Packed GRB for the Waveshare WS2812, kept dim for indoor use.
inline uint32_t colour(bool connected, bool traffic, bool error) {
    if (error) return 0x000800;    // red
    if (traffic) return 0x080008;  // cyan
    if (connected) return 0x080000; // green
    return 0x040800;              // amber
}
void update(bool gateConnected, bool error);
void activity();
void enable(bool on);
}
