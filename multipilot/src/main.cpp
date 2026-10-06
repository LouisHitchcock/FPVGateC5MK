// FPVGate C5MK: multi-pilot RSSI receiver firmware for the ESP32-C5.
//
// For each pilot: retune the LO to 10 MHz below it (PLL only), take one
// I/Q dump, measure the band power around the pilot, filter it. One 'R'
// record per scan cycle goes to the host over native USB. Receive only.
// How the radio is driven: docs/RADIO_INTERNALS.md. Host tools: tools/mp.py
// and web/index.html.
//
// FPVGate node mode (node.cpp): the first valid command from the host on the
// link UART stops the USB scanner and hands the radio to the node, which
// follows the host's F/G/P commands (docs/LINK_PROTOCOL.md).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dump.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "meter.h"
#include "node.h"
#include "radio.h"

using hostlink::say;

namespace {

constexpr int kMaxPilots = 8;
constexpr int kRaceband[kMaxPilots] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
constexpr int kMinMhz = 5100, kMaxMhz = 5950;   // integer-MHz PLL range we accept
constexpr int kYieldEvery = 50;                 // scan cycles between FreeRTOS yields
constexpr int kProfileSamples = 256;

struct Settings {
    int pilots[kMaxPilots];
    int count = kMaxPilots;
    int gain = 30;
    int offsetMhz = 10;   // pilot sits this far above the LO
    int n = 2048;
    int settleUs = 25;
    float alpha = 0.3f;
    bool fullTune = false;
    dump::Mode mode = dump::Mode::Direct;
    int bw = 1;
    bool scanning = true;
    // FPVGate scan mode (P): samples per dump and the hopNoWait skip mask.
    int nodeScanN = 256;
    int nodeHopSkip = 3;
};

Settings cfg;
mp::BandMeter meter;
mp::BandMeter nodeScanMeter;
mp::PilotFilter filt[kMaxPilots];
bool settingsChanged = true;
uint16_t seq = 0;
uint32_t lastCycleUs = 0;
char bootReport[400];
bool rxOnUsed = false;

// ---- helpers ---------------------------------------------------------------

struct Acc {
    double sum = 0, sum2 = 0;
    float mx = -1e9f;
    int n = 0;
    void add(double v) {
        sum += v;
        sum2 += v * v;
        if (v > mx) mx = (float)v;
        ++n;
    }
    double mean() const { return n ? sum / n : 0; }
    double sd() const { return n > 1 ? sqrt(fmax(0, (sum2 - sum * sum / n) / (n - 1))) : 0; }
};

int16_t centi(float db) {
    float c = roundf(db * 100);
    if (c > 32767) c = 32767;
    if (c < -32767) c = -32767;
    return (int16_t)c;
}

void resetFilters() {
    for (auto& f : filt) f.reset();
    settingsChanged = true;
}

bool applyMeter() {
    bool ok = meter.configure(cfg.n, cfg.offsetMhz * 1000000) &&
              nodeScanMeter.configure(cfg.nodeScanN, cfg.offsetMhz * 1000000);
    resetFilters();
    return ok;
}

// One pilot visit: tune the LO to loMhz, settle, dump, measure. Returns false
// if the dump failed.
bool visit(int loMhz, int64_t* tMid, mp::MeterResult& r, bool* fellBack, const mp::BandMeter& m) {
    if (cfg.fullTune) radio::fullTune(loMhz, cfg.bw);
    else radio::hop(loMhz);
    radio::forceGain(cfg.gain);
    if (cfg.settleUs > 0) esp_rom_delay_us(cfg.settleUs);
    if (!dump::capture(m.samples(), cfg.mode, tMid, fellBack)) return false;
    m.measure(dump::words(), cfg.gain, r);
    return true;
}

// The radio as the FPVGate node sees it: same LO offset, dump and meter as
// the scanner, with the S3's gain.
class NodeRadio : public node::RadioPort {
public:
    bool tune(int mhz, int gain) override {
        meter_ = &meter;
        radio::hop(mhz - cfg.offsetMhz);
        if (gain != appliedGain_) {
            radio::forceGain(gain);
            appliedGain_ = gain;
        }
        if (cfg.settleUs > 0) esp_rom_delay_us(cfg.settleUs);
        // The first measurement after OK performs the dump. A warm-up dump
        // here doubled retune work and consumed the useful dwell time without
        // improving the following measurement.
        return true;
    }
    void setGain(int gain) override {
        if (gain == appliedGain_) return;
        radio::forceGain(gain);
        appliedGain_ = gain;
        esp_rom_delay_us(20);
    }
    // Scan mode: the PLL is settled when hopNoWait returns (measured with
    // hopprof, docs/RF_RESEARCH.md), so no settle delay. Re-forcing the gain
    // each hop costs one register write.
    void hop(int mhz, int gain) override {
        meter_ = &nodeScanMeter;
        uint32_t c0 = esp_cpu_get_cycle_count();
        if (lastEnd_) {
            // A hop back down to a lower slot frequency starts a new cycle.
            bool wrap = mhz <= lastMhz_;
            (wrap ? prof.boundary : prof.between) += c0 - lastEnd_;
            prof.cycles += wrap;
        }
        lastMhz_ = mhz;
        radio::hopNoWait(mhz - cfg.offsetMhz, nullptr, cfg.nodeHopSkip);
        radio::forceGain(gain);
        appliedGain_ = gain;
        prof.hop += esp_cpu_get_cycle_count() - c0;
        ++prof.visits;
    }
    bool measure(float* db, uint32_t* tUs) override {
        uint32_t c0 = esp_cpu_get_cycle_count();
        int64_t t = esp_timer_get_time();
        bool fb;
        bool ok = dump::capture(meter_->samples(), cfg.mode, &t, &fb);
        *tUs = (uint32_t)t;
        uint32_t c1 = esp_cpu_get_cycle_count();
        prof.dump += c1 - c0;
        if (ok) {
            // The dump is complete and nothing writes the bank until the next.
            *db = meter_->power((const uint32_t*)dump::words());
            prof.power += esp_cpu_get_cycle_count() - c1;
        }
        lastEnd_ = esp_cpu_get_cycle_count();
        return ok;
    }
    int gainMax() override { return radio::gainMax(); }

    // Scan-mode CPU cycles per stage. `between` is the node's work from one
    // measure() to the next hop inside a cycle (filter, value); `boundary`
    // is from the last slot to the first of the next cycle (record, UART,
    // main loop). Assumes slots in rising frequency, as FPVGate sends them.
    struct Prof {
        uint64_t hop = 0, dump = 0, power = 0, between = 0, boundary = 0;
        uint32_t visits = 0, cycles = 0;
    } prof;
    void resetProf() {
        prof = Prof();
        lastEnd_ = 0;
    }

private:
    uint32_t lastEnd_ = 0;
    int lastMhz_ = 0;
    int appliedGain_ = -1;
    const mp::BandMeter* meter_ = &meter;
};
NodeRadio nodeRadio;

// ---- boot check: are dumps live without the extra RX-on calls? ------------

bool dumpLooksLive(const char* label, char* out, size_t cap) {
    mp::MeterResult r;
    int64_t t;
    bool fb;
    radio::forceGain(cfg.gain);
    radio::hop(cfg.pilots[0] - cfg.offsetMhz);
    esp_rom_delay_us(2000);
    bool ok = dump::capture(cfg.n, dump::Mode::Stock, &t, &fb);
    if (ok) meter.measure(dump::words(), cfg.gain, r);
    bool live = ok && r.db > -20 && r.gainMismatch == 0;
    snprintf(out, cap, "%s: complete=%d db=%.2f dc=%.2f/%.2f gainfield=%d (bad %d) -> %s\n", label, ok,
             ok ? r.db : 0.0f, r.dcI, r.dcQ, ok ? mp::wordGain(dump::words()[0]) : -1, r.gainMismatch,
             live ? "live" : "NOT live");
    return live;
}

// Returns false if the radio didn't come up (no Wi-Fi or no live dumps).
bool bringUp() {
    memcpy(cfg.pilots, kRaceband, sizeof kRaceband);
    applyMeter();
    bool wifi = radio::begin(cfg.bw);
    char* p = bootReport;
    size_t left = sizeof bootReport;
    int k = snprintf(p, left, "wifi=%d gainmax=%d xtalsel=%u trim=%d\n", wifi, radio::gainMax(),
                     radio::xtalSel(), radio::freqTrim());
    p += k;
    left -= k;
    // First without any extra calls, then with TX forced off (kept as a
    // safety measure), and only if still flat, with the RX-on calls.
    bool live = dumpLooksLive("plain", p, left);
    k = strlen(p);
    p += k;
    left -= k;
    radio::txOff();
    live = dumpLooksLive("txoff", p, left);
    k = strlen(p);
    p += k;
    left -= k;
    if (!live) {
        radio::rxOn();
        rxOnUsed = true;
        live = dumpLooksLive("rxon", p, left);
    }
    return dump::seed(cfg.n) && wifi && live;
}

// ---- console ----------------------------------------------------------------

void printStatus() {
    char pl[80];
    int k = 0;
    for (int i = 0; i < cfg.count; ++i) k += snprintf(pl + k, sizeof pl - k, "%s%d", i ? "," : "", cfg.pilots[i]);
    const dump::Stats& s = dump::stats();
    say("pilots %s\ngain %d (max %d)  offset %d MHz  n %d  settle %d us  alpha %.2f\n"
        "tune %s  dump %s  bw %d  scan %s  rxon %d\n"
        "dumps stock %lu direct %lu timeouts %lu incomplete %lu  drops %lu  cycle %lu us\n%s",
        pl, cfg.gain, radio::gainMax(), cfg.offsetMhz, cfg.n, cfg.settleUs, cfg.alpha,
        cfg.fullTune ? "full" : "pll", cfg.mode == dump::Mode::Direct ? "direct" : "stock", cfg.bw,
        cfg.scanning ? "on" : "off", rxOnUsed, (unsigned long)s.stock, (unsigned long)s.direct,
        (unsigned long)s.timeouts, (unsigned long)s.incomplete, (unsigned long)hostlink::drops(),
        (unsigned long)lastCycleUs, bootReport);
    char node[400];
    nodelink::describe(node, sizeof node);
    say("%s", node);
}

void printRegs() {
    auto rd = [](uint32_t a) { return *(volatile uint32_t*)a; };
    say("702C=%08lx (forced %lu, en %lu, max %lu)  9004=%08lx  9008=%08lx  9018=%08lx  95004=%08lx\n",
        (unsigned long)radio::gainReg(), (unsigned long)(radio::gainReg() >> 24),
        (unsigned long)((radio::gainReg() >> 23) & 1), (unsigned long)((radio::gainReg() >> 8) & 0x7F),
        (unsigned long)rd(0x600A9004), (unsigned long)rd(0x600A9008), (unsigned long)rd(0x600A9018),
        (unsigned long)rd(0x60095004));
}

// Hop, full-setup, dump and meter timings.
void cmdTimer() {
    Acc hop, full, stock, direct, met, vis;
    int bad = 0;
    int a = cfg.pilots[0] - cfg.offsetMhz, b = cfg.pilots[cfg.count > 1 ? 1 : 0] - cfg.offsetMhz + 1;
    for (int i = 0; i < 200; ++i) {
        int64_t t0 = esp_timer_get_time();
        radio::hop(i & 1 ? b : a);
        hop.add((double)(esp_timer_get_time() - t0));
    }
    for (int i = 0; i < 20; ++i) {
        int64_t t0 = esp_timer_get_time();
        radio::fullTune(i & 1 ? b : a, cfg.bw);
        full.add((double)(esp_timer_get_time() - t0));
    }
    radio::bandSetup(cfg.bw);
    radio::forceGain(cfg.gain);
    radio::hop(a);
    int64_t tm;
    bool fb;
    for (int i = 0; i < 20; ++i) {
        int64_t t0 = esp_timer_get_time();
        bad += !dump::capture(cfg.n, dump::Mode::Stock, &tm, &fb);
        stock.add((double)(esp_timer_get_time() - t0));
    }
    uint32_t to0 = dump::stats().timeouts;
    for (int i = 0; i < 500; ++i) {
        int64_t t0 = esp_timer_get_time();
        bad += !dump::capture(cfg.n, dump::Mode::Direct, &tm, &fb);
        direct.add((double)(esp_timer_get_time() - t0));
    }
    mp::MeterResult r;
    for (int i = 0; i < 200; ++i) {
        int64_t t0 = esp_timer_get_time();
        meter.measure(dump::words(), cfg.gain, r);
        met.add((double)(esp_timer_get_time() - t0));
    }
    Acc fast;
    float fastDb = 0;
    for (int i = 0; i < 200; ++i) {
        int64_t t0 = esp_timer_get_time();
        fastDb = meter.power((const uint32_t*)dump::words());
        fast.add((double)(esp_timer_get_time() - t0));
    }
    say("  meter power() %.1f / %.1f us, %.3f dB vs measure() %.3f dB\n", fast.mean(), fast.mx, fastDb, r.db);
    for (int i = 0; i < 200; ++i) {
        int64_t t0 = esp_timer_get_time();
        bad += !visit(cfg.pilots[i % cfg.count] - cfg.offsetMhz, &tm, r, &fb, meter);
        vis.add((double)(esp_timer_get_time() - t0));
    }
    say("timer (us, avg/max), n=%d settle=%d:\n"
        "  hop (pll)  %7.1f / %7.1f\n  full setup %7.1f / %7.1f\n  stock dump %7.1f / %7.1f\n"
        "  direct     %7.1f / %7.1f  (timeouts %lu)\n  meter      %7.1f / %7.1f\n"
        "  visit      %7.1f / %7.1f  -> %d pilots: %.0f updates/s each\n  failed dumps %d\n",
        cfg.n, cfg.settleUs, hop.mean(), hop.mx, full.mean(), full.mx, stock.mean(), stock.mx, direct.mean(),
        direct.mx, (unsigned long)(dump::stats().timeouts - to0), met.mean(), met.mx, vis.mean(), vis.mx,
        cfg.count, 1e6 / (vis.mean() * cfg.count), bad);
    resetFilters();
}

// Stock against direct dumps on each pilot: raw dB mean and sd.
void cmdCompare(int count) {
    say("pilot  stock mean/sd   direct mean/sd   (raw dB, %d dumps each)\n", count);
    for (int i = 0; i < cfg.count; ++i) {
        Acc s, d;
        radio::hop(cfg.pilots[i] - cfg.offsetMhz);
        radio::forceGain(cfg.gain);
        esp_rom_delay_us(500);
        mp::MeterResult r;
        int64_t tm;
        bool fb;
        for (int j = 0; j < count; ++j) {
            if (dump::capture(cfg.n, dump::Mode::Stock, &tm, &fb)) {
                meter.measure(dump::words(), cfg.gain, r);
                s.add(r.db);
            }
            if (dump::capture(cfg.n, dump::Mode::Direct, &tm, &fb)) {
                meter.measure(dump::words(), cfg.gain, r);
                d.add(r.db);
            }
        }
        say("%5d  %6.2f / %4.2f   %6.2f / %4.2f   diff %+.2f\n", cfg.pilots[i], s.mean(), s.sd(), d.mean(),
            d.sd(), d.mean() - s.mean());
    }
    resetFilters();
}

// Research: where a PLL hop's time goes, and how soon after hopNoWait() (no
// fixed 300 us wait) a short dump reads the same as after a full hop. For
// each delay, hop in from `jump` MHz below with hopNoWait, wait, read PLL
// register 11 and take a 256-sample dump. skip: as for hopNoWait.
void cmdHopProf(int mhz, int jump, int skip) {
    mp::BandMeter m;
    if (!m.configure(kProfileSamples, cfg.offsetMhz * 1000000)) return say("hopprof: no memory\n");
    int lo = mhz - cfg.offsetMhz;
    Acc stage[5], total;
    for (int i = 0; i < 100; ++i) {
        uint32_t s[5];
        radio::hop(lo - jump);
        int64_t t0 = esp_timer_get_time();
        radio::hopNoWait(lo, s, skip);
        total.add((double)(esp_timer_get_time() - t0));
        for (int k = 0; k < 5; ++k) stage[k].add(s[k]);
    }
    say("hopprof %d MHz (LO %d) from %d MHz below, skip %d; hopNoWait stages (us, avg/max):\n"
        "  pll+sdm+i2c %.1f/%.0f  restart_cal %.1f/%.0f  sdm_en %.1f/%.0f  ckgen_5g %.1f/%.0f  freq_mem %.1f/%.0f"
        "  total %.1f/%.0f\n",
        mhz, lo, jump, skip, stage[0].mean(), stage[0].mx, stage[1].mean(), stage[1].mx, stage[2].mean(), stage[2].mx,
        stage[3].mean(), stage[3].mx, stage[4].mean(), stage[4].mx, total.mean(), total.mx);

    // Steady state: full hop, 2 ms, then dumps.
    Acc ref, refReg;
    radio::hop(lo);
    radio::forceGain(cfg.gain);
    esp_rom_delay_us(2000);
    for (int i = 0; i < 40; ++i) {
        int64_t tm;
        bool fb;
        mp::MeterResult r;
        refReg.add(radio::pllReg(11));
        if (dump::capture(kProfileSamples, cfg.mode, &tm, &fb)) m.measure(dump::words(), cfg.gain, r), ref.add(r.db);
    }
    say("  steady: dB %.2f sd %.2f  reg11 %.1f (max %.0f)\n  delay_us  dB_mean  dB_sd  vs_steady  reg11\n",
        ref.mean(), ref.sd(), refReg.mean(), refReg.mx);
    constexpr int kReps = 12;
    static const int kDelays[] = {0, 5, 10, 15, 20, 30, 40, 60, 80, 100, 150, 200, 300};
    for (int d : kDelays) {
        Acc db, reg;
        for (int j = 0; j < kReps; ++j) {
            radio::hop(lo - jump);
            radio::hopNoWait(lo, nullptr, skip);
            radio::forceGain(cfg.gain);
            if (d) esp_rom_delay_us(d);
            reg.add(radio::pllReg(11));
            int64_t tm;
            bool fb;
            mp::MeterResult r;
            if (dump::capture(kProfileSamples, cfg.mode, &tm, &fb)) m.measure(dump::words(), cfg.gain, r), db.add(r.db);
        }
        say("  %6d  %7.2f  %5.2f  %+7.2f  %5.1f\n", d, db.mean(), db.sd(), db.mean() - ref.mean(), reg.mean());
    }
    resetFilters();
}

// Research: occasional high readings right after a hop. count hops from
// 37 MHz below onto mhz, then one 256-sample dump each. how: 0 stock hop,
// 1 hopNoWait, 2 hopNoWait skip 3, 3 no hop at all (stays on mhz).
// Reports dumps whose gain field strays from the forced index, that clip,
// or that read 6 dB over the median, with details of the first few.
void cmdHopSpikes(int mhz, int count, int how) {
    mp::BandMeter m;
    if (!m.configure(kProfileSamples, cfg.offsetMhz * 1000000)) return say("hopspikes: no memory\n");
    int lo = mhz - cfg.offsetMhz;
    static float db[500];
    static uint8_t gmin[500], gmax[500];
    static uint16_t clips[500], firstBad[500];
    if (count > 500) count = 500;
    radio::hop(lo);
    radio::forceGain(cfg.gain);
    for (int i = 0; i < count; ++i) {
        if (how != 3) {
            radio::hop(lo - 37);
            radio::forceGain(cfg.gain);
            if (how == 0) radio::hop(lo);
            else radio::hopNoWait(lo, nullptr, how == 2 ? 3 : 0);
            radio::forceGain(cfg.gain);
        }
        int64_t tm;
        bool fb;
        mp::MeterResult r;
        db[i] = NAN;
        if (!dump::capture(kProfileSamples, cfg.mode, &tm, &fb)) continue;
        m.measure(dump::words(), cfg.gain, r);
        db[i] = r.db;
        clips[i] = (uint16_t)r.clips;
        const volatile uint32_t* w = dump::words();
        gmin[i] = 127, gmax[i] = 0, firstBad[i] = 0xFFFF;
        for (int k = 0; k < kProfileSamples; ++k) {
            int g = mp::wordGain(w[k]);
            if (g < gmin[i]) gmin[i] = (uint8_t)g;
            if (g > gmax[i]) gmax[i] = (uint8_t)g;
            if (g != cfg.gain && firstBad[i] == 0xFFFF) firstBad[i] = (uint16_t)k;
        }
    }
    static float sorted[500];
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (!isnan(db[i])) sorted[n++] = db[i];
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && sorted[b] < sorted[b - 1]; --b) {
            float t = sorted[b];
            sorted[b] = sorted[b - 1];
            sorted[b - 1] = t;
        }
    float med = n ? sorted[n / 2] : 0;
    int high = 0, gainBad = 0, clipped = 0, shown = 0;
    for (int i = 0; i < count; ++i) {
        if (isnan(db[i])) continue;
        bool h = db[i] > med + 6, g = gmin[i] != cfg.gain || gmax[i] != cfg.gain, c = clips[i] > 0;
        high += h, gainBad += g, clipped += c;
        if ((h || g || c) && shown < 8) {
            ++shown;
            say("  #%d: %.2f dB, gain field %u..%u (first stray word %d), clips %u\n", i, db[i], gmin[i], gmax[i],
                firstBad[i] == 0xFFFF ? -1 : firstBad[i], clips[i]);
        }
    }
    static const char* names[] = {"stock hop", "hopNoWait", "hopNoWait skip 3", "no hop"};
    say("hopspikes %d MHz, %s, %d dumps (%d complete): median %.2f dB, high %d, gain strayed %d, clipped %d\n", mhz,
        names[how], count, n, med, high, gainBad, clipped);
    resetFilters();
}

// Research: the receive passband and image rejection against LO offset.
// For a carrier at mhz, put the LO at each offset below it (negative: above)
// and read the band power at the carrier and at its mirror. Says whether
// one dump can serve two pilots 37 MHz apart (LO between them, +-18.5 MHz).
void cmdBandShape(int mhz) {
    say("bandshape %d MHz, n %d, gain %d, bw %d\n  offset_MHz  signal_dB  mirror_dB  rejection\n", mhz, cfg.n,
        cfg.gain, cfg.bw);
    for (int off2 = -76; off2 <= 76; off2 += 3) {   // half-MHz units, -38..+38 MHz
        if (off2 == 0) continue;
        int offHz = off2 * 500000;
        mp::BandMeter sig, img;
        if (!sig.configure(cfg.n, offHz) || !img.configure(cfg.n, -offHz)) return say("bandshape: no memory\n");
        radio::hop(mhz - off2 / 2);
        // Integer-MHz PLL: odd half-MHz offsets put the LO 0.5 MHz off, which
        // the meter's ~10 MHz band absorbs.
        radio::forceGain(cfg.gain);
        esp_rom_delay_us(300);
        Acc s, m;
        for (int i = 0; i < 16; ++i) {
            int64_t tm;
            bool fb;
            mp::MeterResult r;
            if (!dump::capture(cfg.n, cfg.mode, &tm, &fb)) continue;
            sig.measure(dump::words(), cfg.gain, r);
            s.add(r.db);
            img.measure(dump::words(), cfg.gain, r);
            m.add(r.db);
        }
        say("  %+6.1f  %8.2f  %8.2f  %+7.2f\n", off2 / 2.0, s.mean(), m.mean(), s.mean() - m.mean());
    }
    resetFilters();
}

// Raw capture with the LO at exactly mhz, sent as 'C' chunks.
void cmdCapture(int mhz, int n) {
    static uint16_t id = 0;
    radio::hop(mhz);
    radio::forceGain(cfg.gain);
    esp_rom_delay_us(500);
    int64_t tm;
    bool fb;
    if (!dump::capture(n, cfg.mode, &tm, &fb)) return say("capture: dump incomplete\n");
    const volatile uint32_t* w = dump::words();
    int clips = 0, gmin = 127, gmax = 0;
    for (int i = 0; i < n; ++i) {
        int vi = mp::wordI(w[i]), vq = mp::wordQ(w[i]), g = mp::wordGain(w[i]);
        clips += (vi >= 511 || vi <= -512 || vq >= 511 || vq <= -512);
        if (g < gmin) gmin = g;
        if (g > gmax) gmax = g;
    }
    ++id;
    static uint8_t chunk[sizeof(mp::CaptureHeader) + 256 * 4];
    int lost = 0;
    for (int first = 0; first < n; first += 256) {
        int cnt = n - first < 256 ? n - first : 256;
        mp::CaptureHeader h = {id, (uint16_t)mhz, (uint8_t)cfg.gain, (uint8_t)cfg.bw, (uint16_t)n,
                               (uint16_t)first, (uint16_t)cnt};
        memcpy(chunk, &h, sizeof h);
        for (int i = 0; i < cnt; ++i) {
            uint32_t v = w[first + i];
            memcpy(chunk + sizeof h + 4 * i, &v, 4);
        }
        lost += !hostlink::send(mp::kTypeCapture, chunk, (uint16_t)(sizeof h + 4 * cnt), 50);
    }
    say("capture id %u: LO %d MHz, n %d, gain field %d..%d (forced %d), clips %d, chunks lost %d\n", id, mhz, n,
        gmin, gmax, cfg.gain, clips, lost);
    resetFilters();
}

// Does a dump disturb the lower half of the reserved bank?
void cmdSramTest() {
    volatile uint32_t* lo = (volatile uint32_t*)0x40820000;
    const int words = 0x10000 / 4;
    for (int i = 0; i < words; ++i) lo[i] = 0x9E3779B9u * (uint32_t)(i + 1);
    int64_t tm;
    bool fb;
    int fails = 0;
    for (int j = 0; j < 100; ++j) fails += !dump::capture(cfg.n, cfg.mode, &tm, &fb);
    int bad = 0;
    for (int i = 0; i < words; ++i) bad += lo[i] != 0x9E3779B9u * (uint32_t)(i + 1);
    say("sramtest: 100 dumps (%d failed), %d of %d words in 0x40820000-0x4082FFFF changed\n", fails, bad, words);
}

bool parsePilots(char* args) {
    int list[kMaxPilots];
    int n = 0;
    if (!strcmp(args, "rb")) {
        memcpy(list, kRaceband, sizeof kRaceband);
        n = kMaxPilots;
    } else {
        for (char* t = strtok(args, " ,"); t; t = strtok(nullptr, " ,")) {
            int v = atoi(t);
            if (n >= kMaxPilots || v < kMinMhz || v > kMaxMhz) return false;
            list[n++] = v;
        }
    }
    if (!n) return false;
    memcpy(cfg.pilots, list, sizeof(int) * n);
    cfg.count = n;
    return true;
}

void handle(char* line) {
    char* cmd = strtok(line, " ");
    if (!cmd) return;
    char* arg = strtok(nullptr, "");
    int iv = arg ? atoi(arg) : 0;
    if (!strcmp(cmd, "help")) {
        say("status | regs | scan on|off | pilots rb|<MHz,...> | gain N | offset MHz | n N | settle us |\n"
            "alpha A | tune pll|full | dump direct|stock | bw 0|1 | timer | cmp [count] |\n"
            "noden N | nodeskip 0-3 | hopprof MHz [jump] [skip] | hopspikes MHz [count] [how] | bandshape MHz |\n"
            "capture MHz [n] | sramtest | node [on|off|echo on|off|map lo hi|alpha A|cmd ...]\n");
    } else if (!strcmp(cmd, "status")) {
        printStatus();
    } else if (!strcmp(cmd, "regs")) {
        printRegs();
    } else if (!strcmp(cmd, "scan")) {
        cfg.scanning = arg && !strcmp(arg, "on");
        if (cfg.scanning) nodelink::deactivate();
        resetFilters();
        say("scan %s\n", cfg.scanning ? "on" : "off");
    } else if (!strcmp(cmd, "node")) {
        nodelink::console(arg);
    } else if (!strcmp(cmd, "pilots") && arg) {
        if (parsePilots(arg)) resetFilters(), printStatus();
        else say("pilots: 1-%d values, %d-%d MHz, or 'rb'\n", kMaxPilots, kMinMhz, kMaxMhz);
    } else if (!strcmp(cmd, "gain") && arg) {
        int mx = radio::gainMax();
        if (iv < 0 || iv > mx) return say("gain: 0-%d\n", mx);
        cfg.gain = iv;
        radio::forceGain(iv);
        resetFilters();
        say("gain %d\n", iv);
    } else if (!strcmp(cmd, "offset") && arg) {
        if (iv < -35 || iv > 35) return say("offset: -35..35 MHz\n");
        int old = cfg.offsetMhz;
        cfg.offsetMhz = iv;
        if (!applyMeter()) cfg.offsetMhz = old, applyMeter();
        say("offset %d MHz\n", cfg.offsetMhz);
    } else if (!strcmp(cmd, "n") && arg) {
        if (iv < mp::kMinSamples || iv > mp::kMaxSamples || iv % mp::kBlock) return say("n: 64-8192, multiple of 8\n");
        int old = cfg.n;
        cfg.n = iv;
        if (!applyMeter()) cfg.n = old, applyMeter();
        say("n %d\n", cfg.n);
    } else if (!strcmp(cmd, "noden") && arg) {
        if (iv < mp::kMinSamples || iv > mp::kMaxSamples || iv % mp::kBlock) return say("noden: 64-8192, multiple of 8\n");
        int old = cfg.nodeScanN;
        cfg.nodeScanN = iv;
        if (!applyMeter()) cfg.nodeScanN = old, applyMeter();
        say("node scan n %d\n", cfg.nodeScanN);
    } else if (!strcmp(cmd, "nodeprof")) {
        const auto& p = nodeRadio.prof;
        double v = p.visits ? p.visits * 240.0 : 1;
        double between = p.visits > p.cycles ? (p.visits - p.cycles) * 240.0 : 1;
        double boundary = p.cycles ? p.cycles * 240.0 : 1;
        say("node scan per visit (us), %lu visits: hop %.1f  dump %.1f  power %.1f  between %.1f  total %.1f\n"
            "  per cycle (us), %lu cycles: between slots %.1f  cycle boundary %.1f\n",
            (unsigned long)p.visits, p.hop / v, p.dump / v, p.power / v, (p.between + p.boundary) / v,
            (p.hop + p.dump + p.power + p.between + p.boundary) / v, (unsigned long)p.cycles,
            p.between / between, p.boundary / boundary);
        nodeRadio.resetProf();
    } else if (!strcmp(cmd, "nodeskip") && arg) {
        cfg.nodeHopSkip = iv & 3;
        say("node scan hop skip %d\n", cfg.nodeHopSkip);
    } else if (!strcmp(cmd, "settle") && arg) {
        if (iv < 0 || iv > 5000) return say("settle: 0-5000 us\n");
        cfg.settleUs = iv;
        resetFilters();
        say("settle %d us\n", iv);
    } else if (!strcmp(cmd, "alpha") && arg) {
        float a = strtof(arg, nullptr);
        if (!(a > 0 && a <= 1)) return say("alpha: (0, 1]\n");
        cfg.alpha = a;
        resetFilters();
        say("alpha %.3f\n", a);
    } else if (!strcmp(cmd, "tune") && arg) {
        cfg.fullTune = !strcmp(arg, "full");
        if (!cfg.fullTune) radio::bandSetup(cfg.bw);
        resetFilters();
        say("tune %s\n", cfg.fullTune ? "full" : "pll");
    } else if (!strcmp(cmd, "dump") && arg) {
        cfg.mode = !strcmp(arg, "stock") ? dump::Mode::Stock : dump::Mode::Direct;
        resetFilters();
        say("dump %s\n", cfg.mode == dump::Mode::Stock ? "stock" : "direct");
    } else if (!strcmp(cmd, "bw") && arg) {
        cfg.bw = iv ? 1 : 0;
        radio::bandSetup(cfg.bw);
        radio::forceGain(cfg.gain);
        resetFilters();
        say("bw %d (band setup redone)\n", cfg.bw);
    } else if (!strcmp(cmd, "timer")) {
        cmdTimer();
    } else if (!strcmp(cmd, "cmp")) {
        cmdCompare(iv > 0 && iv <= 1000 ? iv : 100);
    } else if (!strcmp(cmd, "hopprof") && arg) {
        int mhz = atoi(strtok(arg, " "));
        char* js = strtok(nullptr, " ");
        int jump = js ? atoi(js) : 37;
        char* ss = strtok(nullptr, " ");
        int skip = ss ? atoi(ss) & 3 : 0;
        if (mhz < kMinMhz || mhz > kMaxMhz || jump < 1 || jump > 400) return say("hopprof MHz [jump MHz] [skip]\n");
        cmdHopProf(mhz, jump, skip);
    } else if (!strcmp(cmd, "hopspikes") && arg) {
        int mhz = atoi(strtok(arg, " "));
        char* cs = strtok(nullptr, " ");
        char* hs = strtok(nullptr, " ");
        int count = cs ? atoi(cs) : 300, how = hs ? atoi(hs) : 1;
        if (mhz < kMinMhz || mhz > kMaxMhz || count < 1 || how < 0 || how > 3)
            return say("hopspikes MHz [count<=500] [0 stock|1 nowait|2 nowait skip3|3 no hop]\n");
        cmdHopSpikes(mhz, count, how);
    } else if (!strcmp(cmd, "bandshape") && arg) {
        if (iv < kMinMhz + 40 || iv > kMaxMhz) return say("bandshape MHz\n");
        cmdBandShape(iv);
    } else if (!strcmp(cmd, "capture") && arg) {
        int mhz = atoi(strtok(arg, " "));
        char* ns = strtok(nullptr, " ");
        int n = ns ? atoi(ns) : cfg.n;
        if (mhz < kMinMhz || mhz > kMaxMhz || n < 64 || n > dump::kMaxWords) return say("capture MHz [64-8192]\n");
        cmdCapture(mhz, n);
    } else if (!strcmp(cmd, "sramtest")) {
        cmdSramTest();
    } else {
        say("unknown command '%s' (try help)\n", cmd);
    }
}

// ---- scan loop ---------------------------------------------------------------

void scanCycle() {
    static uint8_t buf[sizeof(mp::RssiHeader) + kMaxPilots * sizeof(mp::RssiPilot)];
    int64_t c0 = esp_timer_get_time();
    mp::RssiPilot* out = (mp::RssiPilot*)(buf + sizeof(mp::RssiHeader));
    for (int i = 0; i < cfg.count; ++i) {
        mp::RssiPilot& p = out[i];
        memset(&p, 0, sizeof p);
        mp::MeterResult r;
        int64_t tm = 0;
        bool fb = false;
        bool ok = visit(cfg.pilots[i] - cfg.offsetMhz, &tm, r, &fb, meter);
        p.tUs = (uint32_t)tm;
        p.mhz = (uint16_t)cfg.pilots[i];
        p.gain = (uint8_t)cfg.gain;
        if (ok) {
            p.rawCdb = centi(r.db);
            p.filtCdb = centi(filt[i].push(r.db, cfg.alpha));
            p.clips = (uint8_t)(r.clips > 255 ? 255 : r.clips);
            if (r.clips) p.flags |= mp::kPilotClipped;
            if (r.gainMismatch) p.flags |= mp::kPilotGainMoved;
        } else {
            p.flags |= mp::kPilotDumpFailed;
            p.rawCdb = INT16_MIN;
            p.filtCdb = centi(filt[i].value());
        }
        if (fb) p.flags |= mp::kPilotFallback;
    }
    lastCycleUs = (uint32_t)(esp_timer_get_time() - c0);
    mp::RssiHeader h = {seq++, (uint8_t)cfg.count, (uint8_t)(settingsChanged ? mp::kRecSettingsChanged : 0),
                        hostlink::drops(), lastCycleUs};
    settingsChanged = false;
    memcpy(buf, &h, sizeof h);
    hostlink::send(mp::kTypeRssi, buf, (uint16_t)(sizeof h + cfg.count * sizeof(mp::RssiPilot)));
}

void scanTask(void*) {
    bool radioOk = bringUp();
    nodelink::begin(&nodeRadio, radioOk);
    say("FPVGate C5MK ready\n");
    printStatus();
    uint32_t cycles = 0, passes = 0;
    for (;;) {
        // In node mode a pass can be a whole ~1 ms scan cycle; the USB
        // console still answers within ~16 ms.
        bool consoleDue = !nodelink::active() || (++passes & 15) == 0;
        if (const char* l = consoleDue ? hostlink::pollLine() : nullptr) {
            static char copy[160];
            strncpy(copy, l, sizeof copy - 1);
            handle(copy);
        }
        nodelink::poll();
        if (nodelink::active()) {
            // FPVGate has the radio. The node paces itself at 1 kHz, so no
            // sleeping here; it only stops when `scan on` hands the radio back.
            if (cfg.scanning) {
                cfg.scanning = false;
                say("node mode: FPVGate S3 in control (scan paused; 'scan on' to take it back)\n");
            }
            nodelink::step();
            continue;
        }
        nodelink::step();   // heartbeat status to the S3
        if (cfg.scanning) {
            scanCycle();
            if (++cycles % kYieldEvery == 0) vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

}  // namespace

extern "C" void app_main(void) {
    hostlink::begin();
    xTaskCreate(scanTask, "scan", 8192, nullptr, 10, nullptr);
}
