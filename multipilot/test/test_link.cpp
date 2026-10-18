// Exercise the actual USB line parser with an in-memory driver and RH endpoint.
#include "link.h"
#include "rh_usb.h"
#include <assert.h>
#include <deque>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "driver/usb_serial_jtag.h"

static std::deque<uint8_t> input;
static std::vector<uint8_t> output;
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*) { return 0; }
int usb_serial_jtag_read_bytes(void* data, size_t size, uint32_t) {
    size_t n = 0;
    while (n < size && !input.empty()) {
        static_cast<uint8_t*>(data)[n++] = input.front();
        input.pop_front();
    }
    return static_cast<int>(n);
}
int usb_serial_jtag_write_bytes(const void* data, size_t size, uint32_t) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    output.insert(output.end(), bytes, bytes + size);
    return static_cast<int>(size);
}
struct Port : rhusb::Port {
    void write(const uint8_t* data, size_t size) override { hostlink::sendRaw(data, size); }
    void frequencies(const uint16_t*) override {}
} port;
static rhusb::Core rh(port);
static bool active = false;
static bool uartOwnsRadio = false;
static bool handleByte(uint8_t byte) {
    if (!active) {
        if (byte != rhusb::READ_REVISION_CODE || uartOwnsRadio) return false;
        active = true;
        rh.begin(1000);
        hostlink::setBinaryMode(true);
    }
    rh.feed(byte, 1000);
    return true;
}
static void reset() {
    input.clear();
    output.clear();
    active = uartOwnsRadio = false;
    rh.end();
    hostlink::setBinaryMode(false);
}
static void queue(const char* text) {
    while (*text) input.push_back(static_cast<uint8_t>(*text++));
}
static void probe() {
    input.push_back(rhusb::READ_REVISION_CODE);
    assert(hostlink::pollLine() == nullptr);
    assert(active);
    // API 36, verification prefix, and additive checksum.
    assert((output == std::vector<uint8_t>{0x25, 36, 0x49}));
}
int main() {
    hostlink::begin();
    hostlink::setByteHandler(handleByte);
    reset();
    probe();

    reset();
    queue("AT"); // An external probe without a newline, in a previous poll.
    assert(hostlink::pollLine() == nullptr);
    probe();

    reset();
    queue("unfinished"); // Partial text and RH probe in the same read batch.
    probe();

    reset();
    for (int i = 0; i < 200; ++i) input.push_back('x');
    assert(hostlink::pollLine() == nullptr);
    probe(); // Even an overflowed console line must not block discovery.

    // Taking USB clears old text; returning to console accepts a fresh command.
    active = false;
    rh.end();
    hostlink::setBinaryMode(false);
    queue("status\n");
    const char* line = hostlink::pollLine();
    assert(line && !strcmp(line, "status"));

    reset();
    queue("pilots 5658 5695\r\n");
    line = hostlink::pollLine();
    assert(line && !strcmp(line, "pilots 5658 5695"));
    assert(hostlink::pollLine() == nullptr);
    assert(!active && output.empty());

    reset();
    for (int i = 0; i < 200; ++i) input.push_back('x');
    queue("\nstatus\n");
    line = hostlink::pollLine();
    assert(line && !strcmp(line, "status"));

    reset();
    uartOwnsRadio = true;
    queue("AT");
    input.push_back(rhusb::READ_REVISION_CODE);
    queue("\n");
    line = hostlink::pollLine();
    assert(line && !strcmp(line, "AT\""));
    assert(!active && output.empty()); // A rejected probe preserves console input.

    reset();
    hostlink::pushLine("scan on");
    line = hostlink::pollLine();
    assert(line && !strcmp(line, "scan on"));
    puts("USB line parser: 9 scenarios passed");
}
