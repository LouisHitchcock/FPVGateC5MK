// Function names and arguments from our own disassembly of Espressif's
// Apache-2.0 libphy.a / librftest.a (esp32c5); see docs/PROVENANCE.md.
#include "radio.h"

#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

extern "C" {
// libphy
extern uint8_t phy_param[];
int phy_set_chanfreq(uint32_t mhz, uint8_t bwMode);
void phy_set_rf_freq_offset(uint8_t xtalSel, uint32_t mhz, int16_t trim);
void phy_pbus_workmode(void);
void phy_pbus_xpd_tx_off(void);
void phy_pbus_xpd_rx_on(int on);
void phy_set_rxclk_en(int on);
// The steps inside phy_set_rf_freq_offset (phy_set_rfpll_freq, then the 5G
// clock and frequency-memory updates). phy_set_rfpll_freq ends with
// ets_delay_us(300).
void phy_rfpll_set_freq(uint32_t mhz, uint8_t xtalSel, int16_t trim, void* sdm);
void phy_write_rfpll_sdm(void* sdm);
void phy_set_freq_i2c_new(uint32_t mhz);
void phy_restart_cal(void);
void phy_i2c_sdm_en(uint8_t is5g);
void phy_ckgen_5g_cal(uint32_t mhz);
void phy_freq_mem_change_5g_(void* sdm);
uint8_t phy_i2c_readReg(uint8_t block, uint8_t host, uint8_t reg);
// librftest
void force_rx_gain(int enable, int index, int bt);

// librftest.a's wifi.o refers to cmd_parse, which only Espressif's RF test
// console defines. Nothing here uses that console.
int cmd_parse(void* arg) {
    (void)arg;
    return -1;
}
}

namespace radio {
namespace {
constexpr uint32_t kRegGain = 0x600A702C;   // forced index 24-31, force bit 23, max 8-14
constexpr int kParamXtalSel = 49;           // the byte phy_chip_set_chan forwards to the PLL setter
constexpr int kParamTrim = 30;              // the int16 it forwards as the third argument
constexpr int kBandSetupMhz = 5180;
constexpr int kParamMhz = 714;              // uint16 phy_set_rfpll_freq stores the MHz in
constexpr int kParam5g = 42;                // the 5G-band flag
constexpr uint8_t kI2cPll = 0x63;           // RF PLL internal I2C block
}  // namespace

bool begin(int bwMode) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    bandSetup(bwMode);
    return true;
}

void bandSetup(int bwMode) { phy_set_chanfreq(kBandSetupMhz, (uint8_t)(bwMode ? 1 : 0)); }

void txOff() {
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
}

void rxOn() {
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
}

uint8_t xtalSel() { return phy_param[kParamXtalSel]; }

int16_t freqTrim() {
    int16_t t;
    memcpy(&t, phy_param + kParamTrim, sizeof t);
    return t;
}

void hop(int mhz) { phy_set_rf_freq_offset(xtalSel(), (uint32_t)mhz, freqTrim()); }

void hopNoWait(int mhz, uint32_t* stageUs, int skip) {
    uint32_t sdm[4] = {};   // phy_set_rf_freq_offset passes a 12-byte stack area
    uint32_t t = (uint32_t)esp_timer_get_time();
    auto mark = [&](int i) {
        uint32_t n = (uint32_t)esp_timer_get_time();
        if (stageUs) stageUs[i] = n - t;
        t = n;
    };
    uint16_t m16 = (uint16_t)mhz;
    memcpy(phy_param + kParamMhz, &m16, sizeof m16);
    phy_rfpll_set_freq((uint32_t)mhz, xtalSel(), freqTrim(), sdm);
    phy_write_rfpll_sdm(sdm);
    phy_set_freq_i2c_new((uint32_t)mhz);
    mark(0);
    phy_restart_cal();
    mark(1);
    bool is5g = phy_param[kParam5g] != 0;
    phy_i2c_sdm_en(is5g);
    mark(2);
    if (is5g && !(skip & 1)) phy_ckgen_5g_cal((uint32_t)mhz);
    mark(3);
    if ((is5g || mhz > 2482) && !(skip & 2)) phy_freq_mem_change_5g_(sdm);
    mark(4);
}

uint8_t pllReg(int reg) { return phy_i2c_readReg(kI2cPll, 1, (uint8_t)reg); }

void fullTune(int mhz, int bwMode) { phy_set_chanfreq((uint32_t)mhz, (uint8_t)(bwMode ? 1 : 0)); }

// Third argument 0: the direct register write, no Bluetooth path, no delay.
void forceGain(int index) { force_rx_gain(1, index, 0); }

uint32_t gainReg() { return *(volatile uint32_t*)kRegGain; }
int gainMax() { return (int)((gainReg() >> 8) & 0x7F); }

}  // namespace radio
