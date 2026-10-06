#include "node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "frame.h"
#include "link.h"
#include "nvs.h"

// The link UART. Its own pins, so no boot or debug text ever reaches the S3.
// On a XIAO-based FPVGate the link uses the old RX5808 pins, GPIO for GPIO:
// S3 GPIO4 (D3, TX) -> C5 GPIO4, S3 GPIO5 (D4, RX) <- C5 GPIO5.
#ifndef NODE_UART_TX_PIN
#define NODE_UART_TX_PIN 5   // to the S3's RX
#endif
#ifndef NODE_UART_RX_PIN
#define NODE_UART_RX_PIN 4   // from the S3's TX
#endif

namespace nodelink {
namespace {

constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kBaud = 921600;
constexpr int kViewSlots = 8;
constexpr int64_t kViewEveryUs = 20000;

struct UartIo : node::Io {
    bool echo = false;
    void write(const char* line, size_t n) override {
        uart_write_bytes(kUart, line, n);
        if (echo && n && (uint8_t)line[0] != node::kRecordSync) hostlink::send(mp::kTypeText, line, (uint16_t)n, 5);
    }
    bool rxPending() override {
        size_t n = 0;
        uart_get_buffered_data_len(kUart, &n);
        return n > 0;
    }
};

// The latest reading per frequency, for the USB dashboard.
struct ViewSlot {
    int mhz;
    float raw, filt;
    uint32_t tUs;
    int64_t touched;
};

UartIo io;
node::Core* core = nullptr;
bool isActive = false;
bool enabled = true;     // `node off` ignores the S3
int storedGain = -1;
ViewSlot view[kViewSlots];
int viewCount = 0;
int64_t lastView = 0;
uint16_t viewSeq = 0;

uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

int loadGain() {
    nvs_handle_t h;
    int32_t g = 30;
    if (nvs_open("mpnode", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "gain", &g);
        nvs_close(h);
    }
    return g;
}

void saveGain(int g) {
    nvs_handle_t h;
    if (nvs_open("mpnode", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "gain", g);
    nvs_commit(h);
    nvs_close(h);
}

void onSample(int mhz, float rawDb, float filtDb, uint16_t, uint32_t t) {
    int i = 0;
    while (i < viewCount && view[i].mhz != mhz) ++i;
    if (i == viewCount) {
        if (viewCount < kViewSlots) {
            ++viewCount;
        } else {   // replace the one not heard from for longest
            i = 0;
            for (int k = 1; k < kViewSlots; ++k)
                if (view[k].touched < view[i].touched) i = k;
        }
        view[i].mhz = mhz;
    }
    view[i].raw = rawDb;
    view[i].filt = filtDb;
    view[i].tUs = t;
    view[i].touched = esp_timer_get_time();
}

int16_t centi(float db) {
    float c = db * 100;
    return (int16_t)(c > 32767 ? 32767 : c < -32767 ? -32767 : c);
}

// One 'R' record (the scanner's format, header flag kRecNodeMode) with the
// latest reading of each frequency the S3 is cycling through, by MHz.
void sendView() {
    if (!viewCount) return;
    ViewSlot s[kViewSlots];
    memcpy(s, view, sizeof(ViewSlot) * viewCount);
    for (int a = 1; a < viewCount; ++a)
        for (int b = a; b > 0 && s[b].mhz < s[b - 1].mhz; --b) {
            ViewSlot t = s[b];
            s[b] = s[b - 1];
            s[b - 1] = t;
        }
    uint8_t buf[sizeof(mp::RssiHeader) + kViewSlots * sizeof(mp::RssiPilot)];
    mp::RssiHeader h = {viewSeq++, (uint8_t)viewCount, mp::kRecNodeMode, hostlink::drops(), 0};
    memcpy(buf, &h, sizeof h);
    for (int k = 0; k < viewCount; ++k) {
        mp::RssiPilot p = {};
        p.tUs = s[k].tUs;
        p.rawCdb = centi(s[k].raw);
        p.filtCdb = centi(s[k].filt);
        p.mhz = (uint16_t)s[k].mhz;
        p.gain = (uint8_t)core->gain();
        memcpy(buf + sizeof h + k * sizeof p, &p, sizeof p);
    }
    // Never wait: with no dashboard reading the USB the buffer stays full, and
    // a 2 ms wait every 20 ms took 10% of the scan.
    hostlink::send(mp::kTypeRssi, buf, (uint16_t)(sizeof h + viewCount * sizeof(mp::RssiPilot)), 0);
}

}  // namespace

void begin(node::RadioPort* radio, bool radioOk) {
    uart_config_t c = {};
    c.baud_rate = kBaud;
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(kUart, 1024, 4096, 0, nullptr, 0);
    uart_param_config(kUart, &c);
    uart_set_pin(kUart, NODE_UART_TX_PIN, NODE_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    static node::Core instance(*radio, io);
    core = &instance;
    core->onSample = onSample;
    storedGain = loadGain();
    int mx = radio->gainMax();
    core->boot(nowUs(), storedGain > mx ? mx : storedGain, radioOk);
}

void poll() {
    // The length check is a lot cheaper than a read that finds nothing, and
    // this runs once per ~1 ms scan cycle.
    if (!io.rxPending()) return;
    uint8_t buf[128];
    int n;
    while ((n = uart_read_bytes(kUart, buf, sizeof buf, 0)) > 0) {
        if (!enabled) continue;
        uint32_t before = core->reader.good;
        for (int i = 0; i < n; ++i) core->onByte((char)buf[i], nowUs());
        if (core->reader.good != before) isActive = true;
    }
}

void step() {
    core->step(nowUs());
    if (core->gain() != storedGain) {
        storedGain = core->gain();
        saveGain(storedGain);
    }
    int64_t t = esp_timer_get_time();
    if (isActive && t - lastView >= kViewEveryUs) {
        lastView = t;
        sendView();
    }
}

bool active() { return isActive; }

void deactivate() {
    if (!isActive) return;
    isActive = false;
    core->handle("F,0", nowUs());
    viewCount = 0;
}

void describe(char* out, size_t cap) {
    snprintf(out, cap,
             "node %s%s: %s %d MHz gain %d  map %.1f..%.1f dB  uart tx %d rx %d\n"
             "  lines ok %lu bad %lu long %lu  cmds %lu rejected %lu  samples %lu stale-dropped %lu "
             "failed %lu  status %lu\n"
             "  scan mask %02x  cycles %lu aborted %lu\n",
             isActive ? "ACTIVE" : "idle", enabled ? "" : " (ignoring S3)", node::stateName(core->state()),
             core->mhz(), core->gain(), core->mapLo(), core->mapHi(), NODE_UART_TX_PIN, NODE_UART_RX_PIN,
             (unsigned long)core->reader.good, (unsigned long)core->reader.bad, (unsigned long)core->reader.overflow,
             (unsigned long)core->commands, (unsigned long)core->rejected, (unsigned long)core->samples,
             (unsigned long)core->staleDropped, (unsigned long)core->failedDumps, (unsigned long)core->statuses,
             core->scanMask(), (unsigned long)core->cycles, (unsigned long)core->abortedCycles);
}

void console(char* args) {
    char* sub = args ? strtok(args, " ") : nullptr;
    char* rest = sub ? strtok(nullptr, "") : nullptr;
    char line[400];
    if (!sub) {
        describe(line, sizeof line);
        hostlink::say("%s", line);
    } else if (!strcmp(sub, "on")) {
        enabled = true;
        hostlink::say("node: listening to the S3\n");
    } else if (!strcmp(sub, "off")) {
        enabled = false;
        deactivate();
        hostlink::say("node: ignoring the S3\n");
    } else if (!strcmp(sub, "echo")) {
        io.echo = rest && !strcmp(rest, "on");
        hostlink::say("node echo %s\n", io.echo ? "on" : "off");
    } else if (!strcmp(sub, "map") && rest) {
        float lo = strtof(strtok(rest, " "), nullptr);
        char* h = strtok(nullptr, " ");
        if (h) core->setMap(lo, strtof(h, nullptr));
        hostlink::say("node map %.2f..%.2f dB -> 0..1023\n", core->mapLo(), core->mapHi());
    } else if (!strcmp(sub, "alpha") && rest) {
        float a = strtof(rest, nullptr);
        if (a > 0 && a <= 1) core->setAlpha(a);
        hostlink::say("node alpha %.2f\n", a);
    } else if (!strcmp(sub, "cmd") && rest) {
        // Commands as if from the S3 (unframed payloads, ';' between them,
        // handled back to back like F then G on the UART), for bench tests.
        isActive = true;
        for (char* p = strtok(rest, ";"); p; p = strtok(nullptr, ";")) core->handle(p, nowUs());
    } else {
        hostlink::say("node [on|off|echo on|off|map <dBlo> <dBhi>|alpha A|cmd <F,MHz|G,n|Q>]\n");
    }
}

}  // namespace nodelink
