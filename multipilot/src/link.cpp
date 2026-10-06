#include "link.h"

#include <stdarg.h>
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"

namespace hostlink {
namespace {
constexpr size_t kMaxPayload = 1100;
uint8_t txBuf[kMaxPayload + mp::kFrameOverhead];
char line[160];
size_t lineLen = 0;
bool lineOverflow = false;
uint32_t dropCount = 0;
}  // namespace

void begin() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 8192;
    cfg.rx_buffer_size = 512;
    usb_serial_jtag_driver_install(&cfg);
}

bool send(uint8_t type, const void* payload, uint16_t len, int waitMs) {
    size_t n = mp::frameEncode(type, payload, len, txBuf, sizeof txBuf);
    if (!n) return false;
    // The driver copies the whole frame into its ring buffer or nothing.
    if (usb_serial_jtag_write_bytes(txBuf, n, pdMS_TO_TICKS(waitMs)) != (int)n) {
        ++dropCount;
        return false;
    }
    return true;
}

void say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    send(mp::kTypeText, buf, (uint16_t)n);
}

const char* pollLine() {
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {
            if (lineLen == 0 && !lineOverflow) continue;
            bool bad = lineOverflow;
            line[lineLen] = 0;
            lineLen = 0;
            lineOverflow = false;
            if (!bad) return line;
            continue;
        }
        if (lineLen + 1 < sizeof line) line[lineLen++] = (char)c;
        else lineOverflow = true;
    }
    return nullptr;
}

uint32_t drops() { return dropCount; }

}  // namespace hostlink
