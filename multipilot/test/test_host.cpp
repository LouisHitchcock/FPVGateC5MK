// PC tests for the reference host library (host/c5host.*), wired to the C5's
// own protocol code (node::Core) through a simulated UART, so the host is
// checked against what the firmware really sends.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "c5host.h"
#include "node_core.h"

static int checks = 0, fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        ++checks;                                                   \
        if (!(c)) {                                                 \
            ++fails;                                                \
            printf("  FAIL %s (%s:%d)\n", #c, __FILE__, __LINE__); \
        }                                                           \
    } while (0)

static const uint16_t kRaceband[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};

// Each Raceband channel reads a different, steady level.
struct FakeRadio : node::RadioPort {
    uint32_t clock = 1000;   // the C5's esp_timer
    int mhz = 0;
    bool tune(int m, int) override { return mhz = m, true; }
    void setGain(int) override {}
    void hop(int m, int) override { mhz = m; }
    bool measure(float* db, uint32_t* tUs) override {
        clock += 110;
        *tUs = clock;
        *db = 0;
        for (int i = 0; i < 8; ++i)
            if (mhz == kRaceband[i]) *db = 5.0f * (float)i;
        return true;
    }
    int gainMax() override { return 89; }
};

// Both directions of the UART. Bytes from the C5 reach the host after a
// latency that varies, as they would through UART FIFOs and a busy loop.
struct Wire : node::Io {
    std::string toC5;
    std::vector<uint8_t> toHost;
    void write(const char* l, size_t n) override { toHost.insert(toHost.end(), l, l + n); }
    bool rxPending() override { return !toC5.empty(); }
};

struct Bench {
    FakeRadio radio;
    Wire wire;
    node::Core* core;
    c5host::Link host;
    std::vector<c5host::Sample> got;
    std::vector<uint32_t> truth;   // C5 time of each delivered sample's record cycle
    uint32_t latencyBase = 5000;
    uint32_t step = 0;
    bool corruptNext = false;

    static void writeFn(void* ctx, const uint8_t* d, size_t n) { ((Bench*)ctx)->wire.toC5.append((const char*)d, n); }
    static void sampleFn(void* ctx, const c5host::Sample& s) { ((Bench*)ctx)->got.push_back(s); }

    Bench() : core(new node::Core(radio, wire)), host(writeFn, sampleFn, this) {
        core->boot(radio.clock, 30, true);
    }
    ~Bench() { delete core; }

    uint32_t hostNow() const { return radio.clock + latencyBase + (step * 37u) % 300u; }

    // One pass: the C5 handles waiting commands and runs, then the host reads.
    void run(int passes) {
        for (int k = 0; k < passes; ++k, ++step) {
            std::string in;
            in.swap(wire.toC5);
            for (char c : in) core->onByte(c, radio.clock);
            radio.clock += 50;
            core->step(radio.clock);
            if (corruptNext && wire.toHost.size() > 10 && wire.toHost[0] == 0xA5) {
                wire.toHost[9] ^= 0x40;
                corruptNext = false;
            }
            std::vector<uint8_t> out;
            out.swap(wire.toHost);
            host.feed(out.data(), out.size(), hostNow());
            host.tick(hostNow());
        }
    }

    void reboot() {
        delete core;
        wire.toC5.clear();
        radio.clock = 1000;   // the C5's clock restarts
        latencyBase += 10000000;   // and the host's has moved on
        core = new node::Core(radio, wire);
        core->boot(radio.clock, 30, true);
    }
};

static void testStartupAndValues() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    b.run(20);
    CHECK(b.host.firmware() == 2);
    CHECK(b.host.scanning());
    CHECK(strcmp(b.host.state(), "SCAN") == 0);
    CHECK(b.core->state() == node::State::Scan);
    CHECK(b.core->scanMask() == 0xFF);
    b.got.clear();
    b.run(200);
    int perSlot[8] = {};
    bool valuesOk = true;
    for (auto& s : b.got) {
        ++perSlot[s.slot];
        if (s.rssi != b.core->rssiFromDb(5.0f * s.slot)) valuesOk = false;
    }
    for (int i = 0; i < 8; ++i) CHECK(perSlot[i] == 200);
    CHECK(valuesOk);
    CHECK(b.host.counters().badRecords == 0 && b.host.counters().seqGaps == 0);
    CHECK(b.host.counters().failedDumps == 0);

    // Time order within each slot, one cycle apart, and slots in a cycle
    // 110 us apart (the fake radio's visit time).
    bool order = true, spacing = true;
    uint32_t last[8] = {};
    for (size_t k = 0; k < b.got.size(); ++k) {
        const auto& s = b.got[k];
        if (last[s.slot] && (int32_t)(s.timeUs - last[s.slot]) <= 0) order = false;
        last[s.slot] = s.timeUs;
        if (k && s.slot && b.got[k - 1].slot == s.slot - 1 && s.timeUs - b.got[k - 1].timeUs != 110) spacing = false;
    }
    CHECK(order);
    CHECK(spacing);
}

// A record leaves the C5 at the end of its cycle, so the mapping's offset is
// the smallest latency seen plus the time from the first slot's reading to
// the end of the cycle (7 x 110 us here). The result is a constant shift of
// about one cycle, the same for every pilot.
static void testClockMapping() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    b.run(400);
    // The latest sample is slot 7's, taken at the C5's current time.
    const auto& s = b.got.back();
    CHECK(s.slot == 7);
    int32_t err = (int32_t)(s.timeUs - (b.radio.clock + b.latencyBase));
    CHECK(err >= 770 && err < 770 + 300);
}

static void testCorruptRecord() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    b.run(50);
    uint32_t before = b.host.counters().samples;
    b.corruptNext = true;
    b.run(1);
    b.run(50);
    CHECK(b.host.counters().badRecords == 1);
    CHECK(b.host.counters().seqGaps == 1);
    CHECK(b.host.counters().samples == before + 50 * 8);
    CHECK(b.host.scanning());
}

static void testRebootRecovers() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    b.run(50);
    b.reboot();
    b.run(1);   // the C5's boot status: no list
    CHECK(!b.host.scanning());
    CHECK(strcmp(b.host.state(), "ERR_FREQ") == 0);
    b.got.clear();
    b.run(400);   // the host resends P straight away
    CHECK(b.host.scanning());
    CHECK(b.core->scanMask() == 0xFF);
    CHECK(b.got.size() > 1000);
    // After the reboot the mapping restarted: times continue on the host's
    // clock, not the C5's restarted one.
    CHECK(!b.got.empty() && b.got.back().timeUs > b.latencyBase);
}

static void testLostSlotList() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    while (b.host.firmware() != 2 && b.step < 10) b.run(1);
    CHECK(b.wire.toC5.find("P,5658") != std::string::npos);
    b.wire.toC5.clear();   // the P never arrives
    b.run(20);
    CHECK(!b.host.scanning());
    CHECK(b.core->state() != node::State::Scan);
    b.run(4000);   // 200 ms more (50 us a pass while idle): not resent yet
    CHECK(!b.host.scanning());
    b.run(2000);   // past 250 ms: resent

    CHECK(b.host.scanning());
    CHECK(b.core->scanMask() == 0xFF);
}

static void testGainAndSlotChange() {
    Bench b;
    b.host.setSlots(kRaceband);
    b.host.begin(b.hostNow());
    b.run(20);
    CHECK(b.host.setGain(40));
    b.run(5);
    CHECK(b.core->gain() == 40);
    CHECK(b.host.reportedGain() == 40);
    CHECK(!b.host.setGain(90));

    uint16_t two[8] = {5658, 0, 0, 0, 5806, 0, 0, 0};
    CHECK(b.host.setSlots(two));
    b.got.clear();
    b.run(100);
    CHECK(b.core->scanMask() == 0x11);
    bool onlyTwo = !b.got.empty();
    for (auto& s : b.got) onlyTwo &= s.slot == 0 || s.slot == 4;
    CHECK(onlyTwo);

    uint16_t bad[8] = {5100};
    CHECK(!b.host.setSlots(bad));
    CHECK(b.core->scanMask() == 0x11);
}

static void testGateDetector() {
    c5host::GateDetector g;
    g.setThresholds(600, 500);
    int passes = 0;
    uint32_t passUs = 0;
    // Floor, a rise to a peak at t = 300 ms, a fall, floor again.
    for (uint32_t t = 0; t < 600; ++t) {
        int d = (int)t - 300;
        int v = 116 + 700 - (d < 0 ? -d : d) * 4;
        if (v < 116) v = 116;
        if (g.push((uint16_t)v, t * 1000u, &passUs)) ++passes;
    }
    CHECK(passes == 1);
    // The EMA lags a little behind the true peak.
    CHECK(passUs >= 300000 && passUs <= 302000);
    CHECK(!g.inside());
}

int main() {
    testStartupAndValues();
    testClockMapping();
    testCorruptRecord();
    testRebootRecovers();
    testLostSlotList();
    testGainAndSlotChange();
    testGateDetector();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
