#include "status_led.h"
#include <atomic>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace statusled {
namespace {
std::atomic<unsigned> state{0};
std::atomic<uint32_t> lastActivity{0};
std::atomic<bool> hasActivity{false}, enabled{true};
uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

void task(void*) {
#ifdef STATUS_LED_XIAO
    // XIAO: plain yellow LED, active low.
    gpio_set_level(GPIO_NUM_27, 1);
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);
#else
    // Waveshare's schematic shows WS2812B-1010 on GPIO27 (not GPIO29).
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    rmt_tx_channel_config_t c = {};
    c.gpio_num = GPIO_NUM_27;
    c.clk_src = RMT_CLK_SRC_DEFAULT;
    c.resolution_hz = 10000000;
    c.mem_block_symbols = 48;  // entire pixel fits; no mid-frame refill
    c.trans_queue_depth = 1;
    rmt_bytes_encoder_config_t e = {};
    e.bit0.level0 = e.bit1.level0 = 1;
    e.bit0.duration0 = 3; e.bit0.duration1 = 9;
    e.bit1.duration0 = 9; e.bit1.duration1 = 3;
    e.flags.msb_first = 1;
    if (rmt_new_tx_channel(&c, &channel) != ESP_OK ||
        rmt_new_bytes_encoder(&e, &encoder) != ESP_OK || rmt_enable(channel) != ESP_OK) {
        ESP_LOGW("status_led", "LED unavailable");
        if (encoder) rmt_del_encoder(encoder);
        if (channel) rmt_del_channel(channel);
        vTaskDelete(nullptr);
        return;
    }
    rmt_transmit_config_t tx = {};
    // Low end level plus the 50 ms task delay supplies the pixel reset time.
    uint8_t pixel[3] = {};
#endif
    int previous = -1;
    for (;;) {
        uint32_t ms = millis();
        unsigned flags = state.load(std::memory_order_relaxed);
        bool usb = usb_serial_jtag_is_connected();
        bool connected = usb || (flags & 1);
        bool traffic = connected && hasActivity.load(std::memory_order_relaxed) &&
                       ms - lastActivity.load(std::memory_order_relaxed) < 500;
        bool on = level(ms, connected, traffic, flags & 2, enabled.load(std::memory_order_relaxed));
#ifdef STATUS_LED_XIAO
        int output = on;
#else
        int output = on ? (int)colour(connected, traffic, flags & 2) : 0;
#endif
        if (previous != output) {
#ifdef STATUS_LED_XIAO
            gpio_set_level(GPIO_NUM_27, !on);
#else
            pixel[0] = (uint8_t)(output >> 16); // green
            pixel[1] = (uint8_t)(output >> 8);  // red
            pixel[2] = (uint8_t)output;         // blue
            if (rmt_transmit(channel, encoder, pixel, sizeof pixel, &tx) == ESP_OK)
                rmt_tx_wait_all_done(channel, -1); // blocks only the LED task
#endif
            previous = output;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}
// The single-core scanner can busy-loop indefinitely in node mode. Wake just
// above it so status still changes; block between updates and during RMT TX.
void begin() { xTaskCreate(task, "status_led", 2048, nullptr, 11, nullptr); }
void update(bool gate, bool error) { state.store((gate ? 1u : 0u) | (error ? 2u : 0u), std::memory_order_relaxed); }
void activity() {
    lastActivity.store(millis(), std::memory_order_relaxed);
    hasActivity.store(true, std::memory_order_relaxed);
}
void enable(bool on) { enabled.store(on, std::memory_order_relaxed); }
}
