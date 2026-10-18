#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
bool binaryMode = false;
// Nothing goes out until the host has sent a byte. On Linux opening the port
// resets the C5, and RH then waits 2 s without reading: output sent then is
// held by the host's USB serial driver, which clearing the port's input does
// not empty, and RH reads it as its revision code. The dashboard and mp.py
// send a command first, and RH's first byte is its revision request.
bool hostSpoke = false;
bool (*byteHandler)(uint8_t) = nullptr;
char pushed[160];
bool linePushed = false;
}  // namespace

void begin() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 8192;
    cfg.rx_buffer_size = 512;
    usb_serial_jtag_driver_install(&cfg);
}

bool send(uint8_t type, const void* payload, uint16_t len, int waitMs) {
    if (binaryMode || !hostSpoke) return false;
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
    if (linePushed) {
        linePushed = false;
        strncpy(line, pushed, sizeof line - 1);
        line[sizeof line - 1] = 0;
        return line;
    }
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        hostSpoke = true;
        // A host probe (or a Linux tty echo) can leave an unterminated console
        // line behind. Always offer bytes to the binary endpoint: it recognises
        // RH's revision request and clears the console buffer when it takes USB.
        // Otherwise every RH retry would extend that line forever, even across
        // server restarts. The endpoint rejects ordinary console bytes.
        if (byteHandler && byteHandler(c)) {
            if (linePushed) return pollLine();   // the handler ended its session with a command
            continue;
        }
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

void setByteHandler(bool (*handler)(uint8_t)) { byteHandler = handler; }

void setBinaryMode(bool enabled) {
    // The session's first bytes out must be its replies. Scanner output still
    // queued for a host that was already reading (no reset on open, as on
    // Windows) would reach RH first and read as its revision code. The driver
    // can't drop queued bytes, so it is reinstalled; only this task uses it
    // (the console is the UART). Up to one packet already in the hardware
    // FIFO still goes out; RH's next retry clears it.
    if (enabled && !binaryMode && usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_uninstall();
        begin();
    }
    binaryMode = enabled;
    lineLen = 0;
    lineOverflow = false;
}

void pushLine(const char* text) {
    strncpy(pushed, text, sizeof pushed - 1);
    pushed[sizeof pushed - 1] = 0;
    linePushed = true;
}

bool sendRaw(const uint8_t* data, size_t size) {
    if (usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(20)) == (int)size) return true;
    ++dropCount;
    return false;
}

}  // namespace hostlink
