// Local RSSI screen for the RockBase NM-CYD-C5 (ST7789, 240x320).
//
// The scan task calls panelService() between cycles. One pilot row is
// redrawn every 80 ms. Pixels are bit-banged: the SPI master driver keeps
// several KB of IRAM in the SRAM bank the I/Q dump owns, and the build
// rejects that. panelService() is never called from inside a capture.
// Readings still leave on the link UART and USB; this screen is only a
// local view of the latest value per frequency.
//
// Pins are the board's, from the RockBase v1.0 schematic (BruceDevices
// nm-cyd-c5 pin map and the LCD Wizard preset): SCK 6, MOSI 7, MISO 2,
// CS 23, D/C 24, backlight 25, reset tied to the chip reset. Colour order
// is BGR and inversion is off. Touch (CS 1) and the SD card (CS 10) share
// this SPI bus and are held deselected.

#include "panel.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "link.h"
#include "node.h"

// The local scanner's pilot list (main.cpp). 0 means that slot is off.
void c5mkLocalPilotsGet(uint16_t mhz[8]);
void c5mkLocalPilotsSet(const uint16_t mhz[8]);

namespace {

constexpr gpio_num_t kSck = GPIO_NUM_6;
constexpr gpio_num_t kMosi = GPIO_NUM_7;
constexpr gpio_num_t kMiso = GPIO_NUM_2;   // panel data-out; left undriven
static_assert(kMiso == GPIO_NUM_2, "CYD MISO");
constexpr gpio_num_t kCs = GPIO_NUM_23;
constexpr gpio_num_t kDc = GPIO_NUM_24;
constexpr gpio_num_t kBl = GPIO_NUM_25;
constexpr gpio_num_t kTouchCs = GPIO_NUM_1;
constexpr gpio_num_t kSdCs = GPIO_NUM_10;

constexpr uint8_t kMadctl = 0xC8;  // portrait flipped 180, BGR (MX|MY|BGR)

constexpr int kW = 240;
constexpr int kH = 320;
constexpr int kScale = 2;
constexpr int kGap = 2;
constexpr int kBarW = 224;
constexpr int kBarH = 6;
constexpr int kRowH = 30;
constexpr int kRowY0 = 28;
constexpr int kCfgX = 28;
constexpr int kCfgY = 274;
constexpr int kCfgW = 184;
constexpr int kCfgH = 40;
constexpr int kCfgR = 12;
constexpr int kActY = 276;
constexpr int kActH = 44;
// Frequency list. The right-hand pill turns a slot on or off; the number
// opens a keypad. Local scanner accepts 5100..5950.
constexpr int kMinEdit = 5100;
constexpr int kMaxEdit = 5950;
constexpr int kEditY0 = 6;
constexpr int kEditRowH = 33;
constexpr int kFreqX = 24;
constexpr int kFreqW = 128;
constexpr int kTogX = 160;
constexpr int kTogW = 72;
constexpr int kPillH = 30;
constexpr int kKeyX0 = 2;
constexpr int kKeyY0 = 48;
constexpr int kKeyW = 76;
constexpr int kKeyH = 50;
constexpr int kKeyGap = 4;
// Bruce nm-cyd-c5: setTouch({225, 3413, 403, 3334, 1}) at TFT_eSPI
// rotation 3 (landscape 320x240, then mirrored). This panel is portrait.
constexpr int kCalX0 = 225;
constexpr int kCalX1 = 3413;
constexpr int kCalY0 = 403;
constexpr int kCalY1 = 3334;
constexpr int kHitNone = 0;
constexpr int kHitSet = 1;
constexpr int kHitOk = 3;
constexpr int kHitNo = 4;
constexpr int kHitBs = 5;
constexpr int kHitFreq = 10;
constexpr int kHitTog = 20;
constexpr int kHitDigit = 30;
constexpr int kSlots = 8;
constexpr int64_t kDrawPeriodUs = 80000;
constexpr int64_t kStaleUs = 2000000;
constexpr int kMaxChars = 4;

constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kFg = 0xFFFF;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kTrack = 0x18C3;
constexpr uint16_t kHot = 0x07E0;

struct Glyph {
    char ch;
    uint8_t row[7];
};

// 5x7, bit 4 is the left pixel. Only the characters this screen draws.
constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
};

struct Slot {
    uint16_t mhz = 0;
    uint16_t rssi = 0;
    int64_t seen = 0;
};

bool ready = false;
bool heard = false;
bool host = false;
bool headerDirty = true;
nodelink::UartPeer peer = nodelink::UartPeer::None;
uint8_t gain = 0;
int row = 0;
int64_t lastDrawUs = 0;
Slot slot[kSlots];
bool editing = false;
bool redrawMain = false;
int editPaint = -1;   // -1 idle, 0..7 one row, 8 the whole editor or keypad
int armed = kHitNone;
bool touching = false;
uint16_t editMhz[kSlots] = {};
bool editOn[kSlots] = {};
int keySlot = -1;
char keyBuf[5] = {};
int keyLen = 0;
bool keyBad = false;

uint16_t be(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

// Mode 0: data stable before the rising edge. Direct registers, so a row
// does not go through the SPI driver's IRAM.
void shiftOut(const uint8_t* data, size_t n) {
    gpio_dev_t* hw = &GPIO;
    const uint32_t mosi = 1u << kMosi;
    const uint32_t sck = 1u << kSck;
    for (size_t i = 0; i < n; ++i) {
        uint8_t v = data[i];
        for (int b = 0; b < 8; ++b) {
            if (v & 0x80) hw->out_w1ts.val = mosi;
            else hw->out_w1tc.val = mosi;
            hw->out_w1ts.val = sck;
            hw->out_w1tc.val = sck;
            v = (uint8_t)(v << 1);
        }
    }
}

void writeBytes(bool dataMode, const void* bytes, size_t n) {
    gpio_ll_set_level(&GPIO, kDc, dataMode ? 1 : 0);
    gpio_ll_set_level(&GPIO, kCs, 0);
    if (n) shiftOut((const uint8_t*)bytes, n);
    gpio_ll_set_level(&GPIO, kCs, 1);
}

// RAMWR is already sent. `line` is one row; it is clocked out `repeat` times.
void writeRows(const uint16_t* line, int width, int repeat) {
    gpio_ll_set_level(&GPIO, kDc, 1);
    gpio_ll_set_level(&GPIO, kCs, 0);
    for (int r = 0; r < repeat; ++r) shiftOut((const uint8_t*)line, (size_t)width * 2);
    gpio_ll_set_level(&GPIO, kCs, 1);
}

const uint8_t* glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (const Glyph& g : kFont)
        if (g.ch == ch) return g.row;
    return nullptr;
}

void holdHigh(gpio_num_t pin) {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode = GPIO_MODE_OUTPUT;
    gpio_config(&c);
    gpio_set_level(pin, 1);
}

void writeCmd(uint8_t cmd, const uint8_t* data, size_t n) {
    writeBytes(false, &cmd, 1);
    if (n) writeBytes(true, data, n);
}

void setAddr(int x, int y, int w, int h) {
    uint8_t c[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
    writeCmd(0x2A, c, 4);
    uint8_t r[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
    writeCmd(0x2B, r, 4);
    writeCmd(0x2C, nullptr, 0);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
    if (w < 1 || h < 1 || w > kW) return;
    uint16_t line[kW];
    uint16_t pix = be(color);
    for (int i = 0; i < w; ++i) line[i] = pix;
    setAddr(x, y, w, h);
    writeRows(line, w, h);
}

// Draws up to 4 characters at 2x. Returns the box width.
int drawText(int x, int y, const char* s, uint16_t fg, uint16_t bg) {
    int n = 0;
    while (s[n] && n < kMaxChars) ++n;
    if (!n) return 0;
    const int cw = 5 * kScale + kGap;
    const int w = n * cw - kGap;
    const int h = 7 * kScale;
    uint16_t buf[kMaxChars * (5 * kScale + kGap) * (7 * kScale)];
    uint16_t fbe = be(fg);
    uint16_t bbe = be(bg);
    for (int i = 0; i < w * h; ++i) buf[i] = bbe;
    for (int ci = 0; ci < n; ++ci) {
        const uint8_t* g = glyph(s[ci]);
        if (!g) continue;
        int ox = ci * cw;
        for (int rowi = 0; rowi < 7; ++rowi) {
            uint8_t bits = g[rowi];
            for (int col = 0; col < 5; ++col) {
                if (!(bits & (uint8_t)(0x10 >> col))) continue;
                for (int sy = 0; sy < kScale; ++sy) {
                    for (int sx = 0; sx < kScale; ++sx) {
                        int i = (rowi * kScale + sy) * w + (ox + col * kScale + sx);
                        buf[i] = fbe;
                    }
                }
            }
        }
    }
    setAddr(x, y, w, h);
    writeBytes(true, buf, (size_t)w * h * 2);
    return w;
}

void drawBar(int x, int y, uint16_t rssi) {
    int fill = (int)rssi * kBarW / 1023;
    if (rssi && fill < 1) fill = 1;
    if (fill > kBarW) fill = kBarW;
    uint16_t on = be(rssi >= 640 ? kHot : kYellow);
    uint16_t off = be(kTrack);
    uint16_t line[kBarW];
    for (int c = 0; c < kBarW; ++c) line[c] = c < fill ? on : off;
    setAddr(x, y, kBarW, kBarH);
    writeRows(line, kBarW, kBarH);
}

// Two joined links, 8x8, bit 7 on the left. Green: a UART host session
// (FPVGate or any other speaker of the link protocol). Yellow: bytes that
// are not that protocol. Dark: the wire is quiet.
constexpr uint8_t kLink[8] = {0x70, 0xC8, 0x84, 0x82, 0x41, 0x21, 0x13, 0x0E};

void drawIcon(int x, int y, uint16_t fg) {
    constexpr int n = 8;
    constexpr int s = 2;
    uint16_t buf[n * s * n * s];
    uint16_t fbe = be(fg);
    uint16_t bbe = be(kBg);
    for (int i = 0; i < n * s * n * s; ++i) buf[i] = bbe;
    for (int rowi = 0; rowi < n; ++rowi) {
        uint8_t bits = kLink[rowi];
        for (int col = 0; col < n; ++col) {
            if (!(bits & (uint8_t)(0x80 >> col))) continue;
            for (int sy = 0; sy < s; ++sy)
                for (int sx = 0; sx < s; ++sx)
                    buf[(rowi * s + sy) * (n * s) + (col * s + sx)] = fbe;
        }
    }
    setAddr(x, y, n * s, n * s);
    writeBytes(true, buf, sizeof buf);
}

void drawHeader() {
    drawText(8, 6, "C5MK", kYellow, kBg);
    uint16_t link = kTrack;
    if (peer == nodelink::UartPeer::Host) link = kHot;
    else if (peer == nodelink::UartPeer::Other) link = kYellow;
    drawIcon(62, 4, link);
    const char* mode = "WAIT";
    if (heard) mode = host ? "NODE" : "USB ";
    drawText(96, 6, mode, kFg, kBg);
    char g[8];
    if (heard) snprintf(g, sizeof g, "G%02u", gain);
    else snprintf(g, sizeof g, "G--");
    drawText(176, 6, g, kFg, kBg);
    headerDirty = false;
}

void drawRow(int i) {
    int y = kRowY0 + i * kRowH;
    char freq[8];
    char num[8];
    if (slot[i].mhz) {
        snprintf(freq, sizeof freq, "%u", slot[i].mhz);
        snprintf(num, sizeof num, "%4u", slot[i].rssi);
    } else {
        memcpy(freq, "----", 5);
        memcpy(num, "    ", 5);
    }
    drawText(8, y, freq, kFg, kBg);
    drawText(176, y, num, kYellow, kBg);
    drawBar(8, y + 16, slot[i].mhz ? slot[i].rssi : 0);
}

int textWidth(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    if (!n) return 0;
    return n * (5 * kScale + kGap) - kGap;
}

// Draws past the 4-character text helper, one chunk at a time.
void drawString(int x, int y, const char* s, uint16_t fg, uint16_t bg) {
    char chunk[kMaxChars + 1];
    while (*s) {
        int n = 0;
        while (s[n] && n < kMaxChars) {
            chunk[n] = s[n];
            ++n;
        }
        chunk[n] = 0;
        x += drawText(x, y, chunk, fg, bg) + kGap;
        s += n;
    }
}

void fillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    fillRect(x + r, y, w - 2 * r, h, color);
    fillRect(x, y + r, r, h - 2 * r, color);
    fillRect(x + w - r, y + r, r, h - 2 * r, color);
    int rad = r - 1;
    for (int dy = 0; dy < r; ++dy) {
        int ry = rad - dy;
        int dx = 0;
        while (dx < r && ry * ry + (rad - dx) * (rad - dx) > rad * rad) ++dx;
        int span = r - dx;
        if (span < 1) continue;
        fillRect(x + dx, y + dy, span, 1, color);
        fillRect(x + w - r, y + dy, span, 1, color);
        fillRect(x + dx, y + h - 1 - dy, span, 1, color);
        fillRect(x + w - r, y + h - 1 - dy, span, 1, color);
    }
}

void drawSetButton(bool pressed) {
    fillRect(0, kCfgY - 4, kW, kH - (kCfgY - 4), kBg);
    uint16_t bg = pressed ? kHot : kTrack;
    uint16_t fg = pressed ? kBg : kYellow;
    fillRoundRect(kCfgX, kCfgY, kCfgW, kCfgH, kCfgR, bg);
    const char* label = "CONFIGURE";
    int tw = textWidth(label);
    drawString(kCfgX + (kCfgW - tw) / 2, kCfgY + (kCfgH - 7 * kScale) / 2, label, fg, bg);
}

void drawBars() {
    fillRect(0, 0, kW, kH, kBg);
    fillRect(0, 26, kW, 2, kYellow);
    headerDirty = true;
    drawHeader();
    for (int i = 0; i < kSlots; ++i) drawRow(i);
    drawSetButton(false);
}

void drawCentered(int x, int y, int w, int h, const char* s, uint16_t fg, uint16_t bg) {
    int tw = textWidth(s);
    int th = 7 * kScale;
    drawText(x + (w - tw) / 2, y + (h - th) / 2, s, fg, bg);
}

void drawEditRow(int i) {
    int y = kEditY0 + i * kEditRowH;
    fillRect(0, y, kW, kEditRowH, kBg);
    char idx[2] = {(char)('1' + i), 0};
    drawText(4, y + (kEditRowH - 7 * kScale) / 2, idx, kYellow, kBg);
    int pillY = y + (kEditRowH - kPillH) / 2;
    uint16_t fbg = editOn[i] ? kTrack : kBg;
    fillRoundRect(kFreqX, pillY, kFreqW, kPillH, 8, fbg);
    if (!editOn[i]) {
        fillRect(kFreqX, pillY, kFreqW, 2, kTrack);
        fillRect(kFreqX, pillY + kPillH - 2, kFreqW, 2, kTrack);
        fillRect(kFreqX, pillY, 2, kPillH, kTrack);
        fillRect(kFreqX + kFreqW - 2, pillY, 2, kPillH, kTrack);
    }
    char label[5];
    if (editMhz[i]) snprintf(label, sizeof label, "%u", editMhz[i]);
    else memcpy(label, "----", 5);
    drawCentered(kFreqX, pillY, kFreqW, kPillH, label, editOn[i] ? kYellow : kFg, fbg);
    uint16_t tbg = editOn[i] ? kHot : kTrack;
    uint16_t tfg = editOn[i] ? kBg : kFg;
    fillRoundRect(kTogX, pillY, kTogW, kPillH, 8, tbg);
    drawCentered(kTogX, pillY, kTogW, kPillH, editOn[i] ? "ON" : "OFF", tfg, tbg);
}

void drawActions() {
    fillRect(0, kActY, kW / 2, kActH, kTrack);
    fillRect(kW / 2, kActY, kW / 2, kActH, kHot);
    drawText(49, kActY + 15, "NO", kFg, kTrack);
    drawText(169, kActY + 15, "OK", kBg, kHot);
}

void drawEditor() {
    fillRect(0, 0, kW, kH, kBg);
    for (int i = 0; i < kSlots; ++i) drawEditRow(i);
    drawActions();
}

void drawKey(int col, int row, const char* label) {
    int x = kKeyX0 + col * (kKeyW + kKeyGap);
    int y = kKeyY0 + row * (kKeyH + kKeyGap);
    fillRoundRect(x, y, kKeyW, kKeyH, 10, kTrack);
    drawCentered(x, y, kKeyW, kKeyH, label, kFg, kTrack);
}

void drawKeyEntry() {
    fillRect(70, 8, kW - 78, 32, kBg);
    char shown[5];
    for (int i = 0; i < 4; ++i) shown[i] = i < keyLen ? keyBuf[i] : '-';
    shown[4] = 0;
    int tw = textWidth(shown);
    drawText((kW - tw) / 2, 16, shown, keyBad ? kYellow : kFg, kBg);
}

void drawKeypad() {
    fillRect(0, 0, kW, kH, kBg);
    drawText(8, 16, "MHZ", kYellow, kBg);
    drawKeyEntry();
    const char* label[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"DEL", "0", ""}};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 3; ++col)
            if (label[row][col][0]) drawKey(col, row, label[row][col]);
    drawActions();
}

void loadEditor() {
    uint16_t mhz[kSlots] = {};
    c5mkLocalPilotsGet(mhz);
    for (int i = 0; i < kSlots; ++i) {
        editMhz[i] = mhz[i];
        editOn[i] = mhz[i] != 0;
    }
    keySlot = -1;
    keyLen = 0;
    keyBad = false;
}

void openKey(int i) {
    keySlot = i;
    keyLen = 0;
    keyBad = false;
    keyBuf[0] = 0;
    if (editMhz[i] >= kMinEdit && editMhz[i] <= kMaxEdit) {
        snprintf(keyBuf, sizeof keyBuf, "%u", editMhz[i]);
        keyLen = 4;
    }
    editPaint = 8;
}

bool yFlip = false;
bool xFlip = false;

int dist(int a, int b) { return a > b ? a - b : b - a; }

// The CONFIGURE press is a known spot. Pick the mirror that lands on it
// and keep that choice for the editor.
void considerOri(int x, int y) {
    int buttonY = kCfgY + kCfgH / 2;
    int flippedY = kH - 1 - y;
    yFlip = dist(flippedY, buttonY) < dist(y, buttonY);
    int yUse = yFlip ? flippedY : y;
    if (dist(yUse, buttonY) > kCfgH) return;
    int xUse = xFlip ? (kW - 1 - x) : x;
    bool inside = xUse >= kCfgX - 8 && xUse < kCfgX + kCfgW + 8;
    int xMir = kW - 1 - xUse;
    bool mirInside = xMir >= kCfgX - 8 && xMir < kCfgX + kCfgW + 8;
    if (!inside && mirInside) xFlip = !xFlip;
}

int hitKey(int x, int y) {
    if (y < kKeyY0) return kHitNone;
    int col = (x - kKeyX0) / (kKeyW + kKeyGap);
    int row = (y - kKeyY0) / (kKeyH + kKeyGap);
    if (col < 0 || col > 2 || row < 0 || row > 3) return kHitNone;
    int lx = (x - kKeyX0) % (kKeyW + kKeyGap);
    int ly = (y - kKeyY0) % (kKeyH + kKeyGap);
    if (lx >= kKeyW || ly >= kKeyH) return kHitNone;
    if (row < 3) return kHitDigit + row * 3 + col + 1;
    if (col == 0) return kHitBs;
    if (col == 1) return kHitDigit;
    return kHitNone;
}

int hitTest(int x, int y) {
    if (xFlip) x = kW - 1 - x;
    if (yFlip) y = kH - 1 - y;
    // The RSSI screen has one control. A recognized press opens it; the
    // button paint is only the visible target.
    if (!editing) return kHitSet;
    if (y >= kActY) return x < kW / 2 ? kHitNo : kHitOk;
    if (keySlot >= 0) return hitKey(x, y);
    if (y < kEditY0) return kHitNone;
    int i = (y - kEditY0) / kEditRowH;
    if (i < 0 || i >= kSlots) return kHitNone;
    if (x >= kTogX) return kHitTog + i;
    if (x >= kFreqX) return kHitFreq + i;
    return kHitNone;
}

// The XPT2046 tops out near 2.5 MHz. The display bit-bang is much faster
// than that, so touch clocks are paced. Data is sampled while the clock is
// high, which is the edge this chip's usual bit-bang drivers use.
uint8_t xfer(uint8_t out) {
    gpio_dev_t* hw = &GPIO;
    const uint32_t mosi = 1u << kMosi;
    const uint32_t sck = 1u << kSck;
    uint8_t in = 0;
    for (int b = 0; b < 8; ++b) {
        if (out & 0x80) hw->out_w1ts.val = mosi;
        else hw->out_w1tc.val = mosi;
        esp_rom_delay_us(1);
        hw->out_w1ts.val = sck;
        esp_rom_delay_us(1);
        in = (uint8_t)((in << 1) | (gpio_ll_get_level(hw, kMiso) ? 1 : 0));
        hw->out_w1tc.val = sck;
        out = (uint8_t)(out << 1);
    }
    return in;
}

// 12-bit conversion. The first of the 16 clocks is the busy bit. Chip
// select stays down across the whole sample so the converter stays awake.
uint16_t read12(uint8_t cmd) {
    xfer(cmd);
    uint16_t v = (uint16_t)(((uint16_t)xfer(0) << 8) | xfer(0));
    return (uint16_t)(v >> 3);
}

bool sense(int* sx, int* sy) {
    gpio_set_level(kCs, 1);
    gpio_set_level(kSdCs, 1);
    gpio_set_level(kTouchCs, 0);
    esp_rom_delay_us(2);
    (void)read12(0xB0);
    uint16_t z = read12(0xB0);
    (void)read12(0xD0);
    uint16_t rawX = read12(0xD0);
    (void)read12(0x90);
    uint16_t rawY = read12(0x90);
    xfer(0x00);
    gpio_set_level(kTouchCs, 1);
    gpio_set_level(kSck, 0);
    gpio_set_level(kMosi, 0);
    if (z < 80 || rawX < 50 || rawY < 50 || rawX > 4000 || rawY > 4000) return false;
    // Rotation-3 getTouch, mirrored, folded into this portrait.
    int gx = ((int)rawY - kCalX0) * 320 / kCalX1;
    int gy = ((int)rawX - kCalY0) * 240 / kCalY1;
    int landX = 320 - gx;
    int landY = 240 - gy;
    // Panel is rotated 180 from the portrait this calibration was built for.
    int x = (kW - 1) - landY;
    int y = landX;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= kW) x = kW - 1;
    if (y >= kH) y = kH - 1;
    *sx = x;
    *sy = y;
    return true;
}

void commitKey() {
    if (keyLen != 4) {
        keyBad = true;
        editPaint = 9;
        return;
    }
    int v = 0;
    for (int i = 0; i < keyLen; ++i) v = v * 10 + (keyBuf[i] - '0');
    if (v < kMinEdit || v > kMaxEdit) {
        keyBad = true;
        editPaint = 9;
        return;
    }
    editMhz[keySlot] = (uint16_t)v;
    editOn[keySlot] = true;
    keySlot = -1;
    editPaint = 8;
}

void onTap(int id) {
    if (id == kHitSet && !editing) {
        loadEditor();
        editing = true;
        editPaint = 8;
        return;
    }
    if (!editing) return;
    if (id == kHitNo) {
        if (keySlot >= 0) {
            keySlot = -1;
            editPaint = 8;
        } else {
            editing = false;
            redrawMain = true;
        }
        return;
    }
    if (id == kHitOk) {
        if (keySlot >= 0) {
            commitKey();
            return;
        }
        uint16_t mhz[kSlots] = {};
        int n = 0;
        for (int i = 0; i < kSlots; ++i)
            if (editOn[i] && editMhz[i]) mhz[n++] = editMhz[i];
        if (!n) return;
        for (int i = 0; i < kSlots; ++i) {
            bool keep = false;
            for (int j = 0; j < n; ++j)
                if (slot[i].mhz == mhz[j]) keep = true;
            if (!keep) slot[i] = {};
        }
        c5mkLocalPilotsSet(mhz);
        editing = false;
        redrawMain = true;
        return;
    }
    if (id >= kHitDigit && id < kHitDigit + 10) {
        if (keySlot < 0 || keyLen >= 4) return;
        keyBuf[keyLen++] = (char)('0' + (id - kHitDigit));
        keyBuf[keyLen] = 0;
        keyBad = false;
        editPaint = 9;
        return;
    }
    if (id == kHitBs) {
        if (keySlot < 0 || keyLen < 1) return;
        keyBuf[--keyLen] = 0;
        keyBad = false;
        editPaint = 9;
        return;
    }
    if (id >= kHitFreq && id < kHitFreq + kSlots) {
        openKey(id - kHitFreq);
        return;
    }
    if (id >= kHitTog && id < kHitTog + kSlots) {
        int i = id - kHitTog;
        if (editOn[i]) {
            editOn[i] = false;
            editPaint = i;
        } else if (editMhz[i] >= kMinEdit && editMhz[i] <= kMaxEdit) {
            editOn[i] = true;
            editPaint = i;
        } else {
            openKey(i);
        }
    }
}

void pollTouch() {
    int x = 0, y = 0;
    bool down = sense(&x, &y);
    if (down) {
        if (!editing) considerOri(x, y);
        int id = hitTest(x, y);
        if (!touching) {
            armed = id;
            if (!editing) drawSetButton(true);
        } else if (armed == kHitNone) {
            armed = id;
        } else if (id != kHitNone && id != armed) {
            armed = kHitNone;
        }
        touching = true;
        return;
    }
    if (touching && armed) onTap(armed);
    bool was = touching;
    touching = false;
    armed = kHitNone;
    if (was && !editing) drawSetButton(false);
}

bool initPanel() {
    holdHigh(kTouchCs);
    holdHigh(kSdCs);
    holdHigh(kDc);
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << kBl;
    bl.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl);
    gpio_set_level(kBl, 0);
    holdHigh(kCs);
    gpio_config_t clk = {};
    clk.pin_bit_mask = (1ULL << kSck) | (1ULL << kMosi);
    clk.mode = GPIO_MODE_OUTPUT;
    gpio_config(&clk);
    gpio_set_level(kSck, 0);
    gpio_set_level(kMosi, 0);
    gpio_config_t miso = {};
    miso.pin_bit_mask = 1ULL << kMiso;
    miso.mode = GPIO_MODE_INPUT;
    gpio_config(&miso);
    gpio_input_enable(kMiso);

    writeCmd(0x01, nullptr, 0);   // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    writeCmd(0x11, nullptr, 0);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t madctl[] = {kMadctl};
    writeCmd(0x36, madctl, 1);
    const uint8_t colmod[] = {0x55};
    writeCmd(0x3A, colmod, 1);
    const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    writeCmd(0xB2, porch, 5);
    const uint8_t gate[] = {0x35};
    writeCmd(0xB7, gate, 1);
    const uint8_t vcom[] = {0x19};
    writeCmd(0xBB, vcom, 1);
    const uint8_t lcm[] = {0x2C};
    writeCmd(0xC0, lcm, 1);
    const uint8_t vdvEn[] = {0x01};
    writeCmd(0xC2, vdvEn, 1);
    const uint8_t vrhs[] = {0x12};
    writeCmd(0xC3, vrhs, 1);
    const uint8_t vdvs[] = {0x20};
    writeCmd(0xC4, vdvs, 1);
    const uint8_t frame[] = {0x0F};
    writeCmd(0xC6, frame, 1);
    const uint8_t power[] = {0xA4, 0xA1};
    writeCmd(0xD0, power, 2);
    const uint8_t gammaP[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    writeCmd(0xE0, gammaP, sizeof gammaP);
    const uint8_t gammaN[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    writeCmd(0xE1, gammaN, sizeof gammaN);
    writeCmd(0x20, nullptr, 0);   // INVOFF
    writeCmd(0x13, nullptr, 0);   // NORON
    writeCmd(0x29, nullptr, 0);   // DISPON
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(kBl, 1);
    return true;
}

}  // namespace

void panelNote(uint16_t mhz, uint16_t rssi, uint8_t gainNow, bool hostNow) {
    if (mhz < 1000 || mhz > 7000) return;
    rssi &= 0x03FF;
    int found = -1;
    int freeSlot = -1;
    int oldest = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (slot[i].mhz == mhz) {
            found = i;
            break;
        }
        if (!slot[i].mhz && freeSlot < 0) freeSlot = i;
        if (slot[i].seen < slot[oldest].seen) oldest = i;
    }
    int i = found >= 0 ? found : (freeSlot >= 0 ? freeSlot : oldest);
    slot[i].mhz = mhz;
    slot[i].rssi = rssi;
    slot[i].seen = esp_timer_get_time();
    if (!heard || gain != gainNow || host != hostNow) {
        heard = true;
        gain = gainNow;
        host = hostNow;
        headerDirty = true;
    }
}

void panelBegin() {
    if (!initPanel()) {
        hostlink::say("panel: ST7789 init failed\n");
        return;
    }
    ready = true;
    drawBars();
    hostlink::say("panel: CYD ST7789 ready\n");
}

void panelService() {
    if (!ready) return;
    nodelink::UartPeer nowPeer = nodelink::uartPeer();
    if (nowPeer != peer) {
        peer = nowPeer;
        headerDirty = true;
    }
    int64_t now = esp_timer_get_time();
    if (now - lastDrawUs < kDrawPeriodUs) return;
    lastDrawUs = now;
    pollTouch();
    if (redrawMain) {
        redrawMain = false;
        drawBars();
        return;
    }
    if (editing) {
        if (editPaint == 8) {
            if (keySlot >= 0) drawKeypad();
            else drawEditor();
        } else if (editPaint == 9 && keySlot >= 0) {
            drawKeyEntry();
        } else if (keySlot < 0 && editPaint >= 0 && editPaint < kSlots) {
            drawEditRow(editPaint);
        }
        editPaint = -1;
        return;
    }
    if (headerDirty) {
        drawHeader();
        return;
    }
    for (int i = 0; i < kSlots; ++i) {
        if (slot[i].mhz && now - slot[i].seen > kStaleUs) {
            slot[i].mhz = 0;
            slot[i].rssi = 0;
            slot[i].seen = 0;
        }
    }
    drawRow(row);
    if (++row >= kSlots) row = 0;
}

