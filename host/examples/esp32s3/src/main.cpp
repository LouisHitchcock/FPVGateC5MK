// Minimal ESP32-S3 host for a C5MK receiver.
//
// Scans the eight Raceband channels, prints each pilot's latest RSSI and
// reading rate once a second, and prints a line for every gate pass. Start
// your own timer from here: see docs/HOST_IMPLEMENTATION.md.
#include <Arduino.h>

#include "c5host.h"

namespace {

constexpr int kRxPin = 5;   // from the C5's TX
constexpr int kTxPin = 4;   // to the C5's RX
constexpr size_t kRxBuffer = 4096;   // ~95 ms of data at 8 pilots

const uint16_t kRaceband[c5host::kSlots] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};

// Readings from the link task to loop(). One queue for all slots keeps the
// example short; FPVGate keeps one per slot.
QueueHandle_t samples;

c5host::GateDetector gates[c5host::kSlots];
uint16_t latest[c5host::kSlots];
uint32_t counts[c5host::kSlots];

void writeToC5(void*, const uint8_t* data, size_t len) { Serial1.write(data, len); }

void onSample(void*, const c5host::Sample& s) {
    // Called from linkTask. Don't block here: if loop() falls behind, the
    // reading is dropped rather than stalling the UART.
    xQueueSend(samples, &s, 0);
}

c5host::Link c5(writeToC5, onSample, nullptr);

// Services the UART in its own task, above loop()'s priority, so a busy
// loop() can't let the UART buffer overflow. Bulk reads: at ~43 KB/s a call
// per byte would use most of a core.
void linkTask(void*) {
    uint8_t buf[256];
    for (;;) {
        int avail = Serial1.available();
        while (avail > 0) {
            size_t got = Serial1.read(buf, avail < (int)sizeof buf ? avail : sizeof buf);
            if (!got) break;
            c5.feed(buf, got, micros());
            avail -= (int)got;
        }
        c5.tick(micros());
        vTaskDelay(1);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    samples = xQueueCreate(1024, sizeof(c5host::Sample));
    for (auto& g : gates) g.setThresholds(600, 500);   // calibrate these per venue

    Serial1.setRxBufferSize(kRxBuffer);   // before begin()
    Serial1.begin(c5host::kBaud, SERIAL_8N1, kRxPin, kTxPin);
    c5.setSlots(kRaceband);
    c5.setGain(30);
    c5.begin(micros());
    xTaskCreatePinnedToCore(linkTask, "c5link", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
    c5host::Sample s;
    while (xQueueReceive(samples, &s, 0) == pdTRUE) {
        latest[s.slot] = s.rssi;
        ++counts[s.slot];
        uint32_t passUs;
        if (gates[s.slot].push(s.rssi, s.timeUs, &passUs))
            Serial.printf("pass: slot %u (%u MHz) at %lu us\n", s.slot, kRaceband[s.slot], (unsigned long)passUs);
    }

    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        const c5host::Counters& n = c5.counters();
        Serial.printf("C5 %s, fw %d, %s, gain %d | records %lu gaps %lu bad %lu\n",
                      c5.online(micros()) ? "online" : "OFFLINE", c5.firmware(), c5.state(),
                      c5.reportedGain(), (unsigned long)n.records, (unsigned long)n.seqGaps,
                      (unsigned long)n.badRecords);
        for (int i = 0; i < c5host::kSlots; ++i) {
            Serial.printf("  R%d %4u (%lu/s)%s", i + 1, latest[i], (unsigned long)counts[i], i == 3 ? "\n" : "");
            counts[i] = 0;
        }
        Serial.println();
    }
    delay(1);
}
