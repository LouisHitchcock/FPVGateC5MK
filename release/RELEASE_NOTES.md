# C5MK v1.2.6

Adds onboard receiver status LEDs to the 1.2.5 receiver runtime:

- Waveshare C5-Zero: amber waiting pulse, solid green connection, cyan traffic blink, red double flash for radio failure.
- XIAO C5: the same patterns on its single yellow LED.
- `node led off` / `node led on` control the LED until reboot.
- USB host detection distinguishes a live data connection from power-only cables without requiring RH to open the port. S3 commands also indicate a connection, with a 2-second timeout.

Uses the manufacturer's GPIO27 WS2812B wiring on C5-Zero and GPIO27 active-low LED on XIAO C5. No RF tuning, gain, filtering, crossing calculation, protocol format or partition layout changes. Unrelated raw-RSSI diagnostics are excluded.

## Validation

Both board builds, SRAM guards and portable tests are recorded in QUALIFICATION.json; source and artifact hashes are in SOURCE_MANIFEST.json.

A 25-minute physical Waveshare A/B test compared 1.2.5 with the same LED runtime under its development version. Each image ran 6.25 minutes scanning eight frequencies and 6.25 minutes polling eight native RH seats. Both had zero scanner drops, CRC errors or failed captures; RH had zero reply errors, timeouts or seat mismatches. Scanner sample cadence was unchanged at 1.020 ms. Mean scanner work increased by 1.67 us (0.178%), and RH scan throughput was about 0.4% lower. This is low overhead, not zero overhead.

See the [bench report](../docs/STATUS_LED_BENCH_20261106.md) and [LED guide](../docs/STATUS_LED.md). The test used an RH wire-protocol harness, not a full RH race or controlled RF input. It does not establish sensitivity or absolute crossing accuracy. Physical XIAO LED/cable checks, S3 LED interaction, error-pattern injection and repeated power-cycle tests remain pending. Previous 1.2.5 validation applies to unchanged RF/protocol functions, not the new LED scheduler.

## Installation

Use your matching board package and INSTALL.md. Firmware 1.1.0+ retains the same two OTA slots and supports FPVGate application updates. This release does not automatically replace the receiver version locked for FPVGate 1.9.0 qualification.
