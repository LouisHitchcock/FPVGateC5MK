// Radio bring-up, tuning and gain for the dump scanner. Receive only.
#ifndef MP_RADIO_H
#define MP_RADIO_H

#include <stdint.h>

namespace radio {

// Wi-Fi up (promiscuous, which keeps the PHY receiving), then the 5 GHz
// band setup. Returns false if Wi-Fi fails.
bool begin(int bwMode);
// phy_set_chanfreq at 5180 MHz: calibrates the 5 GHz band and sets the
// analog bandwidth (1 wide, 0 narrow).
void bandSetup(int bwMode);
// Power down TX through the PHY bus (a safety measure; nothing here
// transmits anyway).
void txOff();
// The extra "RX on" calls, only used if dumps come back flat without them.
void rxOn();
// PLL-only retune, integer MHz.
void hop(int mhz);
// The same libphy steps as hop() without its fixed 300 us wait at the end;
// the PLL is already settled when it returns (docs/RF_RESEARCH.md).
// stageUs, if given, gets the time of each of its 5 steps. skip bit 0
// leaves out the 5G clock update, bit 1 the frequency-memory update.
void hopNoWait(int mhz, uint32_t* stageUs = nullptr, int skip = 0);
// One register of the RF PLL's internal I2C block (0x63, host 1).
uint8_t pllReg(int reg);
// Full retune through phy_set_chanfreq (calibrates; ~1.2 ms).
void fullTune(int mhz, int bwMode);
void forceGain(int index);
// Highest calibrated gain index (bits 8-14 of 0x600A702C).
int gainMax();
uint32_t gainReg();
// The values passed to phy_set_rf_freq_offset by each hop.
uint8_t xtalSel();
int16_t freqTrim();

}  // namespace radio

#endif
