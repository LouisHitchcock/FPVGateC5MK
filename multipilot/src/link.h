// Native USB (USB-Serial-JTAG) host link: framed output, text-line input.
#ifndef MP_LINK_H
#define MP_LINK_H

#include <stddef.h>
#include <stdint.h>

namespace hostlink {

void begin();
// Sends one frame. Waits at most 2 ms for buffer space; on timeout the
// frame is dropped and counted, so a stalled host never stalls the scanner.
bool send(uint8_t type, const void* payload, uint16_t len, int waitMs = 2);
// printf as 'T' frames.
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Non-blocking: returns a complete command line (without the newline), or
// nullptr.
const char* pollLine();
uint32_t drops();

}  // namespace hostlink

#endif
