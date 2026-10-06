// PC tests for the FPVGate RF-node protocol (docs/LINK_PROTOCOL.md), driving
// node::Core through a fake radio and UART.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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

struct FakeRadio : node::RadioPort {
    int tunedMhz = 0, gain = -1, tunes = 0;
    bool failTune = false, failMeasure = false;
    float level = 0;
    bool tune(int mhz, int g) override {
        ++tunes;
        if (failTune) return false;
        tunedMhz = mhz;
        gain = g;
        return true;
    }
    void setGain(int g) override { gain = g; }
    std::vector<int> hops;   // scan mode retunes, in order
    float slotLevel[8] = {};
    bool useSlotLevel = false;
    uint32_t clock = 1000;
    void hop(int mhz, int g) override {
        hops.push_back(mhz);
        tunedMhz = mhz;
        gain = g;
    }
    bool measure(float* db, uint32_t* tUs) override {
        clock += 110;   // about one fast hop and dump
        *tUs = clock;
        if (failMeasure) return false;
        *db = level;
        if (useSlotLevel)
            for (int i = 0; i < 8; ++i)
                if (tunedMhz == 5658 + 37 * i) *db = slotLevel[i];
        return true;
    }
    int gainMax() override { return 89; }
};

// A decoded scan record.
struct Rec {
    uint8_t seq = 0, mask = 0;
    uint32_t t0 = 0;
    std::vector<uint16_t> value, dt;
};

struct FakeIo : node::Io {
    std::vector<std::string> out;   // every line written, without the newline
    std::vector<Rec> recs;          // every scan record, decoded
    int badRecs = 0;
    std::string pending;            // bytes "in the UART FIFO"
    void write(const char* l, size_t n) override {
        if (n && (uint8_t)l[0] == node::kRecordSync) return record((const uint8_t*)l, n);
        std::string s(l, n);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        out.push_back(s);
    }
    bool rxPending() override { return !pending.empty(); }
    void record(const uint8_t* b, size_t n) {
        size_t len = b[1];
        if (n != len + 3 || node::crc8(b + 1, len + 1) != b[len + 2] || len < 7 || b[2] != node::kRecordScan) {
            ++badRecs;
            return;
        }
        const uint8_t* p = b + 2;
        Rec r;
        r.seq = p[1];
        r.t0 = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
        r.mask = p[6];
        int slots = 0;
        for (int i = 0; i < 8; ++i) slots += (r.mask >> i) & 1;
        if (len != 7u + 4u * slots) {
            ++badRecs;
            return;
        }
        for (int k = 0; k < slots; ++k) {
            r.value.push_back((uint16_t)(p[7 + 4 * k] | p[8 + 4 * k] << 8));
            r.dt.push_back((uint16_t)(p[9 + 4 * k] | p[10 + 4 * k] << 8));
        }
        recs.push_back(r);
    }
};

// A framed line, as the S3 builds it.
static std::string frame(const char* payload) {
    char b[128];
    node::formatLine(payload, b, sizeof b);
    return b;
}

struct Rig {
    FakeRadio radio;
    FakeIo io;
    node::Core core{radio, io};
    uint32_t now = 0;
    Rig() { core.boot(0, 30, true); }
    // Bytes arrive in the FIFO; the main loop drains them, then steps.
    void rx(const std::string& bytes) {
        io.pending += bytes;
        drain();
    }
    void drain() {
        std::string b;
        b.swap(io.pending);
        for (char c : b) core.onByte(c, now);
    }
    void run(uint32_t us, uint32_t tick = 100) {
        for (uint32_t t = 0; t < us; t += tick) {
            now += tick;
            core.step(now);
        }
    }
    int countPrefix(const char* p, size_t from = 0) const {
        int n = 0;
        for (size_t i = from; i < io.out.size(); ++i) n += io.out[i].rfind(p, 0) == 0;
        return n;
    }
    // Strip and verify the checksum of an output line.
    static bool valid(const std::string& l) {
        size_t star = l.rfind('*');
        if (star == std::string::npos || l.size() != star + 3) return false;
        char hex[3];
        snprintf(hex, sizeof hex, "%02X", node::xorSum(l.data(), star));
        return l.compare(star + 1, 2, hex) == 0;
    }
};

int main() {
    // ---- framing ----
    // The spec's examples Q*51 and S,5732,40,OK,1*61 are right; its F,5732*60
    // and G,40*74 are not (the XORs are 69 and 6F).
    CHECK(frame("Q") == "Q*51\n");
    CHECK(frame("S,5732,40,OK,1") == "S,5732,40,OK,1*61\n");
    CHECK(frame("F,5732") == "F,5732*69\n");
    CHECK(frame("G,40") == "G,40*6F\n");
    {
        node::LineReader r;
        const char* p = nullptr;
        for (char c : frame("F,5732")) p = r.feed(c);
        CHECK(p && !strcmp(p, "F,5732"));
        std::string bad = "F,5732*00\n";   // wrong checksum
        for (char c : bad) p = r.feed(c);
        CHECK(!p && r.bad == 1);
        for (char c : std::string("F,5732\n")) p = r.feed(c);   // no checksum
        CHECK(!p && r.bad == 2);
        std::string q = "Q*51\n", ql = "Q*5a\n";
        for (char c : ql) p = r.feed(c);
        CHECK(!p && r.bad == 3);   // lower case / wrong: rejected
        std::string big(150, 'X');
        big += "\n";
        for (char c : big) p = r.feed(c);
        CHECK(!p && r.overflow == 1);
        for (char c : q) p = r.feed(c);   // resynchronised at the next line
        CHECK(p && !strcmp(p, "Q"));
        std::string crlf = frame("Q");
        crlf.insert(crlf.size() - 1, "\r");
        for (char c : crlf) p = r.feed(c);
        CHECK(p && !strcmp(p, "Q"));
    }

    // ---- boot and Q ----
    {
        Rig g;
        CHECK(g.io.out.size() == 1 && g.io.out[0].rfind("S,0,30,ERR_FREQ,2*", 0) == 0);
        size_t n = g.io.out.size();
        g.rx(frame("Q"));
        CHECK(g.io.out.size() == n + 1 && g.io.out.back().rfind("S,", 0) == 0);   // immediately
        g.run(5000);
        CHECK(g.countPrefix("R,") == 0);   // no frequency: no samples
        for (auto& l : g.io.out) CHECK(Rig::valid(l));
    }

    // ---- F, G, range ----
    {
        Rig g;
        g.rx(frame("F,5180"));
        CHECK(g.core.state() == node::State::Tuning);
        CHECK(g.io.out.back().rfind("S,5180,30,TUNING,2*", 0) == 0);
        g.run(300);
        CHECK(g.core.state() == node::State::Ok && g.radio.tunedMhz == 5180);
        g.rx(frame("F,5885"));
        g.run(300);
        CHECK(g.core.state() == node::State::Ok && g.radio.tunedMhz == 5885);
        g.rx(frame("F,5917"));
        g.run(300);
        CHECK(g.core.state() == node::State::Ok && g.radio.tunedMhz == 5917);

        size_t mark = g.io.out.size();
        g.rx(frame("F,5179"));
        CHECK(g.core.state() == node::State::ErrFreq);
        CHECK(g.io.out.back().rfind("S,5179,30,ERR_FREQ,2*", 0) == 0);
        g.run(20000);
        CHECK(g.countPrefix("R,", mark) == 0);
        g.rx(frame("F,5918"));
        CHECK(g.core.state() == node::State::ErrFreq);
        mark = g.io.out.size();
        g.run(20000);
        CHECK(g.countPrefix("R,", mark) == 0);

        for (int gain : {0, 40, 89}) {
            char cmd[16];
            snprintf(cmd, sizeof cmd, "G,%d", gain);
            g.rx(frame(cmd));
            CHECK(g.core.gain() == gain);
        }
        g.rx(frame("G,90"));   // rejected, old gain kept and reported
        CHECK(g.core.gain() == 89 && g.io.out.back().rfind("S,5918,89,ERR_FREQ", 0) == 0);
        g.rx(frame("G,abc"));
        CHECK(g.core.gain() == 89);
    }

    // ---- F then G back to back: one final OK after both, gain applied ----
    {
        Rig g;
        g.rx(frame("F,5732") + frame("G,40"));
        size_t mark = g.io.out.size();
        g.run(300);
        CHECK(g.core.state() == node::State::Ok);
        CHECK(g.radio.tunedMhz == 5732 && g.radio.gain == 40);
        CHECK(g.countPrefix("S,5732,40,OK", mark) == 1);
        // G arrives while the retune is running: OK waits for it.
        Rig h;
        h.rx(frame("F,5732"));
        h.io.pending = frame("G,44");   // in the FIFO, not read yet
        h.core.step(h.now += 100);      // retune happens, but input is waiting
        CHECK(h.core.state() == node::State::Tuning);
        h.drain();
        h.run(300);
        CHECK(h.core.state() == node::State::Ok && h.radio.gain == 44);
        CHECK(h.countPrefix("S,5732,44,OK") == 1 && h.countPrefix("S,5732,30,OK") == 0);
    }

    // ---- runtime: rate, no R during TUNING, wrap, heartbeat ----
    {
        Rig g;
        g.radio.level = -2.8f;
        g.rx(frame("F,5880"));
        // Nothing but status between F and OK.
        size_t ok = 0;
        g.run(100);
        for (size_t i = 0; i < g.io.out.size(); ++i)
            if (g.io.out[i].rfind("S,5880,30,OK", 0) == 0) ok = i;
        CHECK(ok > 0);
        for (size_t i = 0; i < ok; ++i) CHECK(g.io.out[i].rfind("R,", 0) != 0);
        size_t mark = g.io.out.size();
        g.run(1000000);   // 1 s
        int r = g.countPrefix("R,", mark);
        printf("samples in 1 s: %d\n", r);
        CHECK(r >= 990 && r <= 1001);
        CHECK(g.countPrefix("S,", mark) >= 1);   // heartbeat
        // Sequence wraps 255 -> 0.
        bool wrapped = false;
        for (size_t i = mark + 1; i < g.io.out.size(); ++i) {
            if (g.io.out[i].rfind("R,0,", 0) == 0 && i > mark && g.io.out[i - 1].rfind("R,255,", 0) == 0) wrapped = true;
        }
        CHECK(wrapped);
        // Idle level maps to a steady value: -2.8 dB -> about 116 on the
        // 10-bit wire scale.
        CHECK(g.io.out.back().rfind("R,", 0) == 0 || g.io.out.back().rfind("S,", 0) == 0);
        CHECK(g.core.rssiFromDb(-2.8f) == 116);
        // Monotonic and bounded.
        int prev = -1;
        bool mono = true;
        for (float db = -40; db < 90; db += 0.25f) {
            int v = g.core.rssiFromDb(db);
            if (v < prev || v > 1023) mono = false;
            prev = v;
        }
        CHECK(mono && g.core.rssiFromDb(-100) == 0 && g.core.rssiFromDb(100) == 1023);
        for (auto& l : g.io.out) CHECK(Rig::valid(l));

        // Heartbeat with no frequency at all.
        Rig h;
        h.run(3000000);
        CHECK(h.countPrefix("S,") >= 4);   // boot + at least one a second
    }

    // ---- a failed retune is ERR_RF, never OK; failed dumps lead to ERR_RF ----
    {
        Rig g;
        g.radio.failTune = true;
        g.rx(frame("F,5732"));
        g.run(5000);
        CHECK(g.core.state() == node::State::ErrRf);
        CHECK(g.countPrefix("S,5732,30,OK") == 0 && g.countPrefix("S,5732,30,ERR_RF") >= 1);
        CHECK(g.countPrefix("R,") == 0);
        g.radio.failTune = false;   // a new F recovers
        g.rx(frame("F,5732"));
        g.run(500);
        CHECK(g.core.state() == node::State::Ok);
        g.radio.failMeasure = true;
        g.run(20000);
        CHECK(g.core.state() == node::State::ErrRf);
    }

    // ---- a second tune cannot leak samples from the first ----
    {
        Rig g;
        g.radio.level = 30;
        g.rx(frame("F,5732"));
        g.run(5000);
        // F for a new frequency lands in the FIFO between a measurement and
        // its send: that sample must be dropped.
        uint32_t before = g.core.samples;
        g.io.pending = frame("F,5880");
        g.core.step(g.now += 1000);   // measures, sees input waiting, drops it
        CHECK(g.core.samples == before && g.core.staleDropped == 1);
        size_t mark = g.io.out.size();
        g.drain();
        CHECK(g.io.out.back().rfind("S,5880,30,TUNING", 0) == 0);
        // Everything after the TUNING line and before OK is status only.
        g.run(200);
        bool okSeen = false, leak = false;
        for (size_t i = mark; i < g.io.out.size(); ++i) {
            if (g.io.out[i].rfind("S,5880,30,OK", 0) == 0) okSeen = true;
            if (!okSeen && g.io.out[i].rfind("R,", 0) == 0) leak = true;
        }
        CHECK(okSeen && !leak);
        // Round robin over 8 channels, 20 ms each, like the S3.
        const int ch[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5880};
        int wrongAssoc = 0;
        for (int round = 0; round < 3; ++round)
            for (int k = 0; k < 8; ++k) {
                char cmd[16];
                snprintf(cmd, sizeof cmd, "F,%d", ch[k]);
                g.rx(frame(cmd) + frame("G,30"));
                size_t m2 = g.io.out.size();
                g.run(20000);
                if (g.radio.tunedMhz != ch[k]) ++wrongAssoc;
                char okl[32];
                snprintf(okl, sizeof okl, "S,%d,30,OK", ch[k]);
                CHECK(g.countPrefix(okl, m2) == 1);
                int r = g.countPrefix("R,", m2);
                CHECK(r >= 18 && r <= 20);
            }
        CHECK(wrongAssoc == 0);
    }

    // ---- scan mode: P, records, slot order, timing ----
    {
        Rig g;
        g.radio.useSlotLevel = true;
        for (int i = 0; i < 8; ++i) g.radio.slotLevel[i] = -2.8f + 4 * i;
        g.rx(frame("P,5658,5695,5732,5769,5806,5843,5880,5917"));
        CHECK(g.core.state() == node::State::Scan && g.core.scanMask() == 0xFF);
        CHECK(g.io.out.back().rfind("S,0,30,SCAN,2*", 0) == 0);
        size_t lines = g.io.out.size();
        for (int i = 0; i < 100; ++i) g.core.step(g.now += 900);
        CHECK(g.io.recs.size() == 100 && g.io.badRecs == 0);
        CHECK(g.countPrefix("R,", lines) == 0);   // records only, no R lines
        bool order = g.radio.hops.size() == 800, seq = true, dt = true, values = true;
        for (size_t i = 0; order && i < g.radio.hops.size(); ++i) order = g.radio.hops[i] == 5658 + 37 * (int)(i % 8);
        for (size_t i = 0; i < g.io.recs.size(); ++i) {
            const Rec& r = g.io.recs[i];
            seq = seq && r.seq == (uint8_t)i && r.mask == 0xFF && r.value.size() == 8;
            for (int k = 0; k < 8 && k < (int)r.value.size(); ++k) {
                dt = dt && r.dt[k] == 110 * k;
                values = values && r.value[k] == g.core.rssiFromDb(-2.8f + 4 * k);
            }
        }
        CHECK(order && seq && dt && values);
        // Consecutive records follow on in time: each t0 is one cycle on.
        CHECK(g.io.recs[1].t0 - g.io.recs[0].t0 == 8 * 110);
        CHECK(g.core.cycles == 100 && g.core.samples == 800);
        // Heartbeat continues in scan mode.
        g.run(1000000, 1000);
        CHECK(g.countPrefix("S,0,30,SCAN", lines) >= 1);
        for (auto& l : g.io.out) CHECK(Rig::valid(l));
    }

    // ---- scan mode: slots off, bad lists, commands mid-cycle, F, G, failures ----
    {
        Rig g;
        g.rx(frame("P,0,5695,0,5769"));
        CHECK(g.core.state() == node::State::Scan && g.core.scanMask() == 0x0A);
        g.core.step(g.now += 900);
        CHECK(g.io.recs.size() == 1 && g.io.recs[0].mask == 0x0A && g.io.recs[0].value.size() == 2);
        CHECK(g.radio.hops.size() == 2 && g.radio.hops[0] == 5695 && g.radio.hops[1] == 5769);
        // A bad list is rejected whole and the old slots stay.
        for (const char* bad : {"P,5658,abc", "P,5658,5179", "P,5658,,5695", "P,1,2,3,4,5,6,7,8,9", "P,"}) {
            uint32_t rej = g.core.rejected;
            g.rx(frame(bad));
            CHECK(g.core.rejected == rej + 1 && g.core.scanMask() == 0x0A);
        }
        // All slots off: no frequency, no records.
        g.rx(frame("P,0,0,0,0,0,0,0,0"));
        CHECK(g.core.state() == node::State::NoFreq && g.core.scanMask() == 0);
        size_t recs = g.io.recs.size();
        g.run(5000);
        CHECK(g.io.recs.size() == recs);
        // A command waiting in the FIFO ends the cycle unsent.
        g.rx(frame("P,5658,5695,5732"));
        g.io.pending = frame("Q");
        recs = g.io.recs.size();
        g.core.step(g.now += 900);
        CHECK(g.io.recs.size() == recs && g.core.abortedCycles == 1);
        g.drain();
        g.core.step(g.now += 900);
        CHECK(g.io.recs.size() == recs + 1);
        // G in scan mode: stays in SCAN, next hops use the new gain.
        g.rx(frame("G,44"));
        CHECK(g.core.state() == node::State::Scan && g.io.out.back().rfind("S,0,44,SCAN", 0) == 0);
        g.core.step(g.now += 900);
        CHECK(g.radio.gain == 44);
        // Failed dumps are flagged; a long run of them is ERR_RF.
        g.radio.failMeasure = true;
        g.core.step(g.now += 900);
        CHECK(!g.io.recs.empty() && (g.io.recs.back().value[0] & node::kValueFailed));
        g.run(100000, 900);
        CHECK(g.core.state() == node::State::ErrRf);
        g.radio.failMeasure = false;
        g.rx(frame("P,5658"));   // a new list recovers
        CHECK(g.core.state() == node::State::Scan);
        // F leaves scan mode for the single-frequency protocol.
        g.rx(frame("F,5732"));
        g.run(300);
        CHECK(g.core.state() == node::State::Ok && g.core.scanMask() == 0);
        recs = g.io.recs.size();
        size_t mark = g.io.out.size();
        g.run(10000);
        CHECK(g.io.recs.size() == recs && g.countPrefix("R,", mark) >= 9);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
