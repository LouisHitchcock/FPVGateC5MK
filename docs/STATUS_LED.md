# Onboard status LED

Added in 1.2.6; the locked 1.2.5 release remains unchanged.

For colours, troubleshooting and text status codes, use the
[LED and error quick reference](LED_QUICK_REFERENCE.md).

| Waveshare colour | Pattern (also used on XIAO's yellow LED) | Meaning |
| --- | --- | --- |
| Amber | Slow pulse (200 ms each second) | Running, no USB host or recent S3 command |
| Green | Solid | USB host data connection, or S3 command received within 2 seconds |
| Cyan | Fast blink (5 Hz) | RH USB bytes or valid S3 frames received within 500 ms |
| Red | Two short flashes each 1.5 seconds | Radio failure (overrides connection/traffic) |
| Off | Off | Disabled with `node led off` (until reboot) |

`node led on` re-enables the LED. The setting is deliberately RAM-only to avoid
flash writes. A USB host connection does not mean RotorHazard has opened the
serial port: ESP-IDF checks host SOF packets, so a power bank or charge-only
cable does not count. Host suspend can also clear that indication. USB unplug
is reflected even if RH still owns the radio internally. Continuous incoming
traffic produces the fast pattern; outgoing samples alone do not.

## Board wiring

- XIAO C5: plain yellow LED, GPIO27, active low. Confirmed by
  [Seeed's example](https://wiki.seeedstudio.com/xiao_esp32c5_with_micropyhton/).
- Waveshare C5-Zero: WS2812B-1010 RGB LED, GPIO27, dim status colours. The request's GPIO29
  differs from the manufacturer's current
  [schematic](https://github.com/waveshareteam/ESP32-C5-Zero/blob/main/hardware/schematics/ESP32-C5-Zero.pdf).
  This firmware follows the schematic. Check the physical board revision before flashing.

LED output runs at priority 11, separately from the priority 10 scan task, at
20 updates/second. This lets it run during the single-core scanner's busy loop;
it blocks between updates and while RMT transmits. The scan path only publishes atomic state and incoming
activity timestamps. RGB output uses hardware RMT with a whole pixel in memory;
there is no bit-banging or LED wait in the scan path. The USB connection monitor
adds ESP-IDF's SOF check to the FreeRTOS tick.

The Waveshare passed a [25-minute A/B test](STATUS_LED_BENCH_20261106.md)
against 1.2.5 with unchanged scanner sample cadence, zero errors and about 0.4%
lower RH scan throughput.

## Additional hardware qualification

On both boards check: power-only cable, good cable without RH, RH connected,
S3 link, unplug/replug, disconnect S3 while separately powered, radio failure,
and LED disable/re-enable. Compare RF sampling and crossing timing with the
LED enabled/disabled. Portable tests and successful builds cannot establish
physical wiring or timing performance.
