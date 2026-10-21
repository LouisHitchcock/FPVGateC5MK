// Replays an RSSI trace through the RotorHazard USB endpoint (rh_usb.cpp) and
// prints the laps RH would record. Used by tools/rh_db_report.py --replay.
//
// stdin:  "enter <level> exit <level> merge <ms> smooth <ms>" on the first line,
//         then one "<ms> <level 1-254>" per line, in time order.
// stdout: one "lap <pass ms> <pass peak level>" per lap.
#include "rh_usb.h"

#include <stdio.h>
#include <string.h>

namespace {

struct Port : rhusb::Port {
    uint8_t reply[32];
    size_t size = 0;
    void write(const uint8_t* data, size_t n) override {
        size = n < sizeof reply ? n : sizeof reply;
        memcpy(reply, data, size);
    }
    void frequencies(const uint16_t*) override {}
};

void write(rhusb::Core& core, uint8_t command, uint8_t value, uint32_t now) {
    core.feed(command, now);
    core.feed(value, now);
    core.feed(value, now);  // checksum of a one-byte payload
}

}  // namespace

int main() {
    unsigned enter = 0, exit = 0, merge = 0, smooth = 0;
    if (scanf(" enter %u exit %u merge %u smooth %u", &enter, &exit, &merge, &smooth) != 4) {
        fprintf(stderr, "first line: enter <level> exit <level> merge <ms> smooth <ms>\n");
        return 2;
    }
    Port port;
    rhusb::Core core(port);
    core.begin(0);
    rhusb::Detect detect;
    detect.mergeMs = (uint16_t)merge;
    detect.smoothMs = (uint16_t)smooth;
    core.setDetect(detect);
    write(core, rhusb::WRITE_CURNODE_INDEX, 0, 0);
    write(core, rhusb::WRITE_ENTER_AT_LEVEL, (uint8_t)enter, 0);
    write(core, rhusb::WRITE_EXIT_AT_LEVEL, (uint8_t)exit, 0);

    // RH polls about every 100 ms; poll every 10 so no lap counter wrap is missed.
    unsigned ms, level;
    uint32_t nextPoll = 0;
    int lastLap = -1;
    while (scanf("%u %u", &ms, &level) == 2) {
        core.sample(0, (uint16_t)(level * 4), ms);
        if (ms < nextPoll) continue;
        nextPoll = ms + 10;
        port.size = 0;
        core.feed(rhusb::READ_LAP_STATS, ms);
        if (port.size < 16) continue;
        int lap = port.reply[0];
        unsigned age = (unsigned)port.reply[1] << 8 | port.reply[2];
        if (lastLap >= 0 && lap != lastLap) printf("lap %u %u\n", ms - age, port.reply[5]);
        lastLap = lap;
    }
    return 0;
}
