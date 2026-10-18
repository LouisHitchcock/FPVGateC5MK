// Linux PTY transport for the production line parser and RH protocol core.
// Radio, USB/JTAG driver and node.cpp ownership glue are simulated here.
#include "link.h"
#include "rh_usb.h"
#include "driver/usb_serial_jtag.h"
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <unistd.h>

static int serialFd;
static bool active = false;
static unsigned delayMs = 0;
static uint32_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*) { return 0; }
// Writes go straight to the PTY, so there is no queue to drop.
int usb_serial_jtag_driver_uninstall() { return 0; }
bool usb_serial_jtag_is_driver_installed() { return true; }
int usb_serial_jtag_read_bytes(void* data, size_t size, uint32_t) {
    int n = read(serialFd, data, size);
    return n > 0 ? n : 0;
}
int usb_serial_jtag_write_bytes(const void* data, size_t size, uint32_t) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    // Deliberately split replies to exercise real pySerial buffering/timeouts.
    for (size_t i = 0; i < size; ++i) {
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        if (write(serialFd, bytes + i, 1) != 1) return static_cast<int>(i);
    }
    return static_cast<int>(size);
}
struct Port : rhusb::Port {
    void write(const uint8_t* data, size_t size) override { hostlink::sendRaw(data, size); }
    void frequencies(const uint16_t*) override {}
} port;
static rhusb::Core rh(port);
static bool handleByte(uint8_t byte) {
    if (!active) {
        if (byte != rhusb::READ_REVISION_CODE) return false;
        active = true;
        rh.begin(nowMs());
        hostlink::setBinaryMode(true);
    }
    rh.feed(byte, nowMs());
    return true;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    serialFd = atoi(argv[1]);
    if (argc > 2) delayMs = static_cast<unsigned>(atoi(argv[2]));
    fcntl(serialFd, F_SETFL, O_NONBLOCK);
    rh.setIdentity("pty-test", "xiaoc5-sim");
    hostlink::begin();
    hostlink::setByteHandler(handleByte);
    puts("ready");
    fflush(stdout);
    for (;;) {
        hostlink::pollLine();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
