#include "node.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "frame.h"
#include "link.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "rh_usb.h"

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
// A new image that hasn't heard the S3 (or the USB console) this long after
// booting restarts, and the bootloader goes back to the previous one.
constexpr int64_t kTrialUs = 60000000;
bool rhActive = false;

struct UartIo : node::Io {
    bool echo = false;
    void write(const char* line, size_t n) override {
        if (rhActive) return;  // RH reads binary snapshots, not the S3 stream.
        uart_write_bytes(kUart, line, n);
        if (echo && n && (uint8_t)line[0] != node::kRecordSync) hostlink::send(mp::kTypeText, line, (uint16_t)n, 5);
    }
    bool rxPending() override {
        if (rhActive) return false;
        size_t n = 0;
        uart_get_buffered_data_len(kUart, &n);
        return n > 0;
    }
};

// An update from the S3 goes into the spare app partition. esp_ota_end()
// also checks the image itself; the SHA-256 checks it is the one the S3 sent.
struct OtaUpdate : node::UpdatePort {
    esp_ota_handle_t handle = 0;
    const esp_partition_t* target = nullptr;
    mbedtls_sha256_context sha;
    uint8_t want[32];
    uint32_t size = 0, got = 0;

    bool begin(uint32_t s, const uint8_t sha256[32]) override {
        abort();
        target = esp_ota_get_next_update_partition(nullptr);
        if (!target || s > target->size) return false;
        if (esp_ota_begin(target, s, &handle) != ESP_OK) {   // erases s bytes
            handle = 0;
            return false;
        }
        memcpy(want, sha256, sizeof want);
        size = s;
        got = 0;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        return true;
    }
    bool write(const uint8_t* data, size_t n) override {
        if (!handle) return false;
        mbedtls_sha256_update(&sha, data, n);
        got += (uint32_t)n;
        return esp_ota_write(handle, data, n) == ESP_OK;
    }
    bool finish() override {
        if (!handle) return false;
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        if (got != size || memcmp(digest, want, sizeof digest) != 0) {
            esp_ota_abort(handle);
            handle = 0;
            return false;
        }
        esp_err_t e = esp_ota_end(handle);
        handle = 0;
        return e == ESP_OK && esp_ota_set_boot_partition(target) == ESP_OK;
    }
    void abort() override {
        if (!handle) return;
        esp_ota_abort(handle);
        mbedtls_sha256_free(&sha);
        handle = 0;
    }
    void restart() override {
        uart_wait_tx_done(kUart, pdMS_TO_TICKS(100));   // let u,done reach the S3
        esp_restart();
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
OtaUpdate ota;
node::Core* core = nullptr;

struct RhPort : rhusb::Port {
    void write(const uint8_t* data, size_t size) override { hostlink::sendRaw(data, size); }
    void frequencies(const uint16_t* mhz) override {
        char payload[64];
        size_t used = (size_t)snprintf(payload, sizeof payload, "P");
        for (unsigned i = 0; i < rhusb::kSeats; ++i)
            used += (size_t)snprintf(payload + used, sizeof payload - used, ",%u", mhz[i]);
        core->handle(payload, (uint32_t)esp_timer_get_time());
    }
} rhPort;
rhusb::Core rh(rhPort);
bool onTrial = false;   // a new image not yet confirmed
bool isActive = false;
bool enabled = true;     // `node off` ignores the S3
int storedGain = -1;
ViewSlot view[kViewSlots];
int viewCount = 0;
int64_t lastView = 0;
uint16_t viewSeq = 0;

uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }
void rhSample(unsigned slot, uint16_t value, uint32_t sampleUs);

// RH's USB session ends only when the console asks: RH starts polling only
// once a browser connects, so silence from it says nothing.
void releaseRh() {
    rh.end();
    core->handle("F,0", nowUs());
    rhActive = false;
    core->onScanSample = nullptr;
    isActive = false;
    viewCount = 0;
    hostlink::setBinaryMode(false);
}

// While RH has USB, a whole console command line (the dashboard's "status"
// or "scan on", mp.py, a person) takes it back. RH's binary traffic can't
// form one: only printable text, ended by '\n', starting with a known word.
bool consoleCommand(const char* line) {
    static const char* const kWords[] = {"help", "status", "regs", "scan", "node", "pilots", "gain", "timer"};
    for (const char* word : kWords) {
        size_t n = strlen(word);
        if (!strncmp(line, word, n) && (line[n] == 0 || line[n] == ' ')) return true;
    }
    return false;
}

bool usbByte(uint8_t byte) {
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    if (!rhActive) {
        // Do not steal an active S3's radio. Only the RH revision probe claims USB.
        if (byte != rhusb::READ_REVISION_CODE || isActive || core->state() == node::State::ErrRf) return false;
        rhActive = true;
        isActive = true;
        core->onScanSample = rhSample;
        rh.begin(nowMs);
        hostlink::setBinaryMode(true);
        confirmImage();
    }
    static char text[32];
    static size_t textLen = 0;
    if (byte == '\n') {
        text[textLen] = 0;
        // RH's single-byte reads can be printable (':', '"', '#', digits) and sit
        // just before the command: it starts at a known word not after a letter.
        const char* command = nullptr;
        for (size_t i = 0; textLen < sizeof text - 1 && i < textLen && !command; ++i)
            if ((i == 0 || !isalpha((unsigned char)text[i - 1])) && consoleCommand(text + i)) command = text + i;
        textLen = 0;
        if (command) {
            releaseRh();
            hostlink::pushLine(command);   // and run it
            return true;
        }
    } else if (byte < 0x20 || byte >= 0x7F) {
        textLen = 0;                    // RH's binary bytes: a command line starts after them
    } else if (textLen < sizeof text - 1) {
        text[textLen++] = (char)byte;
    } else {
        textLen = sizeof text - 1;      // too long: not a command until the next '\n'
    }
    rh.feed(byte, nowMs);
    return true;
}

void rhSample(unsigned slot, uint16_t value, uint32_t sampleUs) {
    if (!rhActive) return;
    // Extend the scan's 32-bit microsecond timestamp before converting to milliseconds.
    uint64_t now = (uint64_t)esp_timer_get_time();
    uint32_t age = (uint32_t)now - sampleUs;
    rh.sample(slot, value, (uint32_t)((now - age) / 1000));
}

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
    static node::Core instance(*radio, io, &ota);
    core = &instance;
    core->onSample = onSample;
    // The image's version is "<version>+<board>" (CMakeLists.txt).
    char version[32];
    snprintf(version, sizeof version, "%s", esp_app_get_description()->version);
    char* plus = strchr(version, '+');
    if (plus) *plus++ = 0;
    core->setIdentity(version, plus ? plus : "?");
    rh.setIdentity(version, plus ? plus : "ESP32-C5");
    hostlink::setByteHandler(usbByte);
    esp_ota_img_states_t state;
    onTrial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
              state == ESP_OTA_IMG_PENDING_VERIFY;
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
        if (rhActive) continue;
        if (!enabled) continue;
        uint32_t before = core->reader.good;
        for (int i = 0; i < n; ++i) core->onByte((char)buf[i], nowUs());
        if (core->reader.good != before) {
            isActive = true;
            confirmImage();
        }
    }
}

void confirmImage() {
    if (!onTrial) return;
    onTrial = false;
    esp_ota_mark_app_valid_cancel_rollback();
}

void step() {
    if (onTrial && esp_timer_get_time() >= kTrialUs) esp_restart();   // back to the previous image
    core->step(nowUs());
    if (core->gain() != storedGain) {
        storedGain = core->gain();
        saveGain(storedGain);
    }
    int64_t t = esp_timer_get_time();
    if (isActive && !rhActive && t - lastView >= kViewEveryUs) {
        lastView = t;
        sendView();
    }
}

bool active() { return isActive; }
bool rhUsbActive() { return rhActive; }

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
             "  scan mask %02x  cycles %lu aborted %lu\n"
             "  firmware %s%s  update chunks %lu bad %lu\n",
             isActive ? "ACTIVE" : "idle", enabled ? "" : " (ignoring S3)", node::stateName(core->state()),
             core->mhz(), core->gain(), core->mapLo(), core->mapHi(), NODE_UART_TX_PIN, NODE_UART_RX_PIN,
             (unsigned long)core->reader.good, (unsigned long)core->reader.bad, (unsigned long)core->reader.overflow,
             (unsigned long)core->commands, (unsigned long)core->rejected, (unsigned long)core->samples,
             (unsigned long)core->staleDropped, (unsigned long)core->failedDumps, (unsigned long)core->statuses,
             core->scanMask(), (unsigned long)core->cycles, (unsigned long)core->abortedCycles,
             esp_app_get_description()->version, onTrial ? " (on trial)" : "", (unsigned long)core->chunks,
             (unsigned long)core->badChunks);
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
    } else if (!strcmp(sub, "reject")) {
        // RH over USB: crossings that start with a jump off the floor (see rhusb::Reject).
        rhusb::Reject r = rh.reject();
        char* v = rest ? strtok(rest, " ") : nullptr;
        if (v) r.riseMs = (uint16_t)atoi(v);
        if (v && (v = strtok(nullptr, " "))) r.holdMs = (uint16_t)atoi(v);
        if (v && (v = strtok(nullptr, " "))) r.quietMs = (uint16_t)atoi(v);
        if (v && (v = strtok(nullptr, " "))) r.margin = (uint8_t)atoi(v);
        rh.setReject(r);
        hostlink::say("node reject: rise %u ms, hold %u ms, quiet %u ms, margin %u%s\n", r.riseMs, r.holdMs,
                      r.quietMs, r.margin, r.riseMs ? "" : " (off)");
    } else if (!strcmp(sub, "cmd") && rest) {
        // Commands as if from the S3 (unframed payloads, ';' between them,
        // handled back to back like F then G on the UART), for bench tests.
        isActive = true;
        for (char* p = strtok(rest, ";"); p; p = strtok(nullptr, ";")) core->handle(p, nowUs());
    } else {
        hostlink::say("node [on|off|echo on|off|map <dBlo> <dBhi>|alpha A|reject [rise hold quiet margin]|cmd <F,MHz|G,n|Q>]\n");
    }
}

}  // namespace nodelink
