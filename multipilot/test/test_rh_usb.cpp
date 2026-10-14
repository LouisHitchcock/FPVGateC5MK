#include "rh_usb.h"

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static int checks = 0, fails = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Port : rhusb::Port {
    std::vector<uint8_t> bytes;
    std::vector<uint16_t> slots;
    unsigned changes = 0;
    void write(const uint8_t* data, size_t size) override { bytes.insert(bytes.end(), data, data + size); }
    void frequencies(const uint16_t* mhz) override {
        slots.assign(mhz, mhz + rhusb::kSeats);
        ++changes;
    }
};

std::vector<uint8_t> read(rhusb::Core& core, Port& port, uint8_t command, uint32_t now = 1000) {
    port.bytes.clear();
    core.feed(command, now);
    CHECK(!port.bytes.empty());
    if (port.bytes.empty()) return {};
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < port.bytes.size(); ++i) sum += port.bytes[i];
    CHECK(sum == port.bytes.back());
    return {port.bytes.begin(), port.bytes.end() - 1};
}

void write(rhusb::Core& core, uint8_t command, std::initializer_list<uint8_t> data, uint32_t now = 1000) {
    core.feed(command, now);
    uint8_t sum = 0;
    for (uint8_t value : data) { core.feed(value, now); sum += value; }
    core.feed(sum, now);
}

void tests() {
    Port port;
    rhusb::Core core(port);
    core.setIdentity("1.1.0", "xiaoc5");
    core.begin(0);
    CHECK(read(core, port, 0x22) == std::vector<uint8_t>({0x25, 35}));
    CHECK(read(core, port, 0x39) == std::vector<uint8_t>({8}));
    CHECK(read(core, port, 0x3D).size() == 16);
    CHECK(read(core, port, 0x40).size() == 16);
    CHECK(read(core, port, 0x3E).size() == 16);
    CHECK(read(core, port, 0x3F).size() == 16);
    CHECK(read(core, port, 0x11) == std::vector<uint8_t>({0, 0}));
    CHECK(read(core, port, 0x33, 0x12345678) == std::vector<uint8_t>({0x12, 0x34, 0x56, 0x78}));
    for (uint8_t i = 0; i < 8; ++i) {
        write(core, 0x7A, {i});
        CHECK(read(core, port, 0x3A)[0] == i);
        CHECK(read(core, port, 0x3C)[0] == i);
        uint16_t expected[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
        auto frequency = read(core, port, 0x03);
        CHECK((frequency[0] * 256 + frequency[1]) == expected[i]);
    }
    write(core, 0x7A, {8});
    CHECK(read(core, port, 0x3A)[0] == 7);
    write(core, 0x7A, {5});
    write(core, 0x71, {100});
    write(core, 0x72, {80});
    CHECK(read(core, port, 0x31)[0] == 100);
    CHECK(read(core, port, 0x32)[0] == 80);
    core.sample(5, 200, 1000);
    core.sample(5, 440, 1001);
    auto pass = read(core, port, 0x0D, 1002);
    auto extremum = read(core, port, 0x0E, 1002);
    CHECK(pass.size() == 8 && extremum.size() == 8);
    CHECK((extremum[0] & 1) == 1);
    core.sample(5, 600, 1003);
    core.sample(5, 600, 1005);
    core.sample(5, 600, 1007);
    core.sample(5, 300, 1008);
    auto stats = read(core, port, 0x05, 1010);
    CHECK(stats.size() == 16);
    CHECK(stats[0] == 1 && stats[1] == 0 && stats[2] == 5);  // plateau middle at 1005
    CHECK(stats[3] == 75 && stats[5] == 150 && !(stats[8] & 1));
    // Split reads are latched, even if samples or a new pass arrive in between.
    read(core, port, 0x0D, 1010);
    core.sample(5, 800, 1011);
    CHECK(!(read(core, port, 0x0E, 1012)[0] & 1));
    write(core, 0x78, {0});
    CHECK(!(read(core, port, 0x05, 1012)[8] & 1));
    // Failed writes, unknown commands and partial frames do not modify settings.
    core.feed(0x71, 1020); core.feed(150, 1020); core.feed(149, 1020);
    CHECK(read(core, port, 0x31, 1020)[0] == 100);
    core.feed(0x51, 1020); core.feed(0xFF, 1020);
    CHECK(read(core, port, 0x22, 1120).size() == 2);
    write(core, 0x51, {0x17, 0x38});  // 5944 unsupported: previous R6 remains
    CHECK(read(core, port, 0x03) == std::vector<uint8_t>({0x16, 0xD3}));
    write(core, 0x51, {0, 0});
    CHECK(port.slots[5] == 0 && port.slots[6] == 5880);
    core.sample(5, 1023, 1200);
    CHECK(read(core, port, 0x05, 1200)[3] == 1);
    write(core, 0x51, {0x16, 0xD3});
    core.sample(5, 1023, 1201);
    CHECK(read(core, port, 0x05, 1201)[3] == 254);
    // A missing capture run cannot turn the old crossing into a lap.
    core.sample(5, 200, 1400);
    CHECK(read(core, port, 0x05, 1400)[0] == 1);
    core.sample(5, 600, 1500);
    CHECK(read(core, port, 0x05, 1601)[3] == 1);
    CHECK(!(read(core, port, 0x05, 1601)[8] & 1));
    CHECK(!core.expired(6600));
    CHECK(core.expired(6601));
    read(core, port, 0x22, 0xFFFFFF00);
    CHECK(!core.expired(0x100));  // millisecond rollover
    core.begin(0xFFFFFF00);
    write(core, 0x7A, {0}, 0xFFFFFF00);
    core.sample(0, 600, 0xFFFFFFFE);
    core.sample(0, 600, 2);
    core.sample(0, 100, 3);
    stats = read(core, port, 0x05, 10);
    CHECK(stats[0] == 1 && stats[2] == 10);  // plateau middle wraps to zero
    // Lap counters wrap without losing a pass, as expected by RH's API.
    core.begin(0);
    for (unsigned i = 0; i < 256; ++i) {
        core.sample(0, 600, i * 2);
        core.sample(0, 100, i * 2 + 1);
    }
    CHECK(read(core, port, 0x05, 512)[0] == 0);
}

// Line-based simulator for Python tests driving RH's actual serial-node client.
int bridge() {
    Port port;
    rhusb::Core core(port);
    core.setIdentity("usb-dev", "C5MK");
    core.begin(0);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        uint32_t now;
        input >> command >> now;
        port.bytes.clear();
        if (command == "feed") {
            unsigned value;
            while (input >> std::hex >> value) core.feed((uint8_t)value, now);
        } else if (command == "sample") {
            unsigned seat, value;
            input >> seat >> value;
            core.sample(seat, (uint16_t)value, now);
        }
        for (uint8_t value : port.bytes) printf("%02x", value);
        printf("\n");
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--bridge")) return bridge();
    tests();
    printf("RH USB: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
