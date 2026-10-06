# Provenance

Where each technical fact in this firmware came from, so the clean-room claim
in [LICENSING.md](LICENSING.md) can be checked.

## Sources

| Tag | Source |
|---|---|
| D | Our own disassembly of Espressif's **Apache-2.0** prebuilt libraries for the ESP32-C5 (`libphy.a`, `librftest.a`, from esp-phy-lib as shipped with ESP-IDF 5.5), with `riscv32-esp-elf-objdump -d -r` |
| E | Espressif ESP-IDF source, Kconfig and documentation (Apache-2.0) |
| M | Our own bench measurements on a Waveshare ESP32-C5-Zero, chip v1.0, October 2026 |
| R | Public README statements of other projects (facts only) |
| G | A fact first learned by the research side from reading GPL code, passed on as a fact with no code or pseudo-code, then checked independently as described |
| O | Our own design |

## Facts used in the firmware

| Fact | Source |
|---|---|
| The dump trigger `adctrig` is in `librftest.a` (object `mac_common.o`), not `libphy.a`; ESP-IDF links that library when `CONFIG_ESP_PHY_ENABLE_CERT_TEST=y` | D, E |
| `librftest.a` refers to `cmd_parse`, which only Espressif's RF test console defines; a stub returning -1 satisfies the linker | D |
| `CONFIG_ESP_PHY_DISABLE_PLL_TRACK` stops the periodic PLL-tracking timer that would otherwise retune the radio under the scanner | E |
| `adctrig`'s nine arguments; which argument values lead to a tone transmit (`mode` 1), a frame transmit (`selector` 5 or 6, or `mode` 2) or the BLE receiver (`selector` 12); that `selector` 0, `mode` 0, wait style 0 is a pure receive dump | D |
| Rate code 0 is 80 MS/s | R (ESP-SDR's README lists the C5's capture rates), confirmed M (a carrier 10 MHz from the LO lands at exactly 10 MHz when analysed at 80 MS/s) |
| The dump engine's registers: control `0x600A9004` (count bits 0-16, done 18, trigger 19, enable 31), setup `0x600A9008`, SRAM ownership `0x60095004` (bits 8-11 and 16), the order `adctrig` writes them in, and that the engine writes from `0x40830000` | D |
| The stock `adctrig` call costs ~1.65 ms of fixed overhead; driving the same register sequence directly takes ~10 us for 256 samples and gives the same readings | M |
| Sentinel and guard words around the capture to detect short or long dumps | O |
| The modem owns the whole 128 KiB bank `0x40820000`-`0x4083FFFF` during a dump, not just the words it writes | G. Not relied on: the firmware reserves the whole bank anyway, which is harmless if the fact is wrong. The `sramtest` console command checks it directly. |
| Sample word layout: bits 0-9 signed I, bits 10-19 signed Q, bits 20-26 the gain index in force | G, with R (ESP-SDR's README: signed 10-bit I/Q on the C5). Checked M: a VTX 10 MHz from the LO gives one clean peak with 25-36 dB less at the mirror, and the gain field equals the forced index in every sample. A wrong layout gives neither. |
| The spectrum is inverted: RF above the LO appears at negative baseband frequency | M |
| `phy_set_chanfreq(5180, mode)` calibrates the 5 GHz band through Espressif's `phy_chip_set_chan`; `mode` selects the analog bandwidth (1 wide, 0 narrow) | D for the call chain; G for using it as a one-off band setup and for the meaning of `mode`. Checked M: band setup then PLL-only hops give live, stable dumps across 5180-5917 MHz; the passband in mode 1 was measured (RF_RESEARCH.md). Mode 0 is not used. |
| A PLL-only retune with `phy_set_rf_freq_offset(phy_param[49], MHz, trim)`; byte 49 of `phy_param` is what Espressif's own channel code passes; byte 42 is the 5G-band flag | D |
| The steps inside `phy_set_rf_freq_offset`, and that it ends in a fixed 300 us wait; calling the same exported steps without the wait (`hopNoWait`) | D, and M for the settling: readings match steady state from 0 us after the call |
| `force_rx_gain(1, index, 0)` writes the index to bits 24-31 of `0x600A702C` and the force bit 23, with no delay; a nonzero third argument takes a Bluetooth path | D |
| The highest calibrated gain index is in bits 8-14 of `0x600A702C` | G. Checked M: it reads 89 on chip v1.0, and indexes up to it are accepted and appear in the sample words. |
| Bring-up: Wi-Fi started in NULL mode with promiscuous receive on keeps the PHY powered for dumps | E for the APIs; G that this is enough. Checked at every boot: the firmware tests that dumps are live and reports it. |
| `phy_pbus_workmode`, `phy_pbus_xpd_tx_off`, `phy_pbus_xpd_rx_on`, `phy_set_rxclk_en` as the TX-off / RX-on calls | D for the functions; G for the set and order. The firmware calls the TX-off pair as a safety measure and only uses the RX-on pair if dumps are flat without it, which a boot check decides. |
| A WARN log level silences `adctrig`'s `phy_printf` | E (`components/esp_phy/src/lib_printf.c`) |
| Band-power meter: LO 10 MHz below the pilot, DC removal, Hann-windowed 8-sample blocks mixed to 0 Hz, median-of-3 and EMA filter | O |
| The pilot list, binary scan records, CRC-8, C5 timestamps and the UART protocol | O |
| Passband, image rejection, retune timing and post-hop spikes | M (RF_RESEARCH.md) |

## Not taken

- The research side also noted an optional analog filter adjustment. It isn't
  used.
- Nothing about the native AGC is relied on: the firmware forces a fixed gain.

If anything is ever found to trace back to a GPL project's code, it will be
taken out and rewritten. Nothing currently does.
