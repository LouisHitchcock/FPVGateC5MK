# C5MK 1.2.6: LED and error quick reference

**Waveshare C5-Zero:** RGB colours below. **XIAO C5:** one yellow LED;
read the blink pattern instead of the colour.

| Colour | Pattern | Meaning / what to do |
| --- | --- | --- |
| Amber | Slow pulse, once a second | Running, waiting for a host. For USB, check the computer is awake and use a known data cable. For an S3 gate, check its power, UART wiring and receiver configuration. |
| Green | Solid | USB host detected, or a recent command from the S3 gate. This confirms a connection, not that a race is running or RF reception is calibrated. |
| Cyan | Fast blink, five times a second | Incoming RotorHazard USB traffic or valid S3 frames. Normal activity. |
| Red | Two short flashes, repeating every 1.5 seconds | Radio error (`ERR_RF`). Restart the receiver when no update is in progress. If it returns, capture `status` output and check the board firmware and power supply. |
| Off | Continuously dark | No power or LED disabled. If the receiver still responds, try `node led on`. |

A power-only cable can power the board while it pulses amber. Green does not
require RotorHazard to open the serial port. A sleeping USB host may cause amber.
Red overrides connection/activity patterns; disabling the LED hides all patterns.
There is **one LED error pattern** in 1.2.6, not a numbered blink-code system.

## Text status codes

These appear in the console/link status; they are not additional LED colours.

| Code | Meaning |
| --- | --- |
| `ERR_RF` | Radio startup check, tuning or repeated capture failure. No VTX signal by itself does not cause this error. |
| `ERR_FREQ` | No active valid node frequency. Check host channels (5180–5917 MHz). This can be normal when the node is idle, all seats are disabled, or the separate USB scanner owns the radio; it does not trigger the red LED. |
| `TUNING` | Applying frequency or gain; normally brief. |
| `OK` | Single-frequency node sampling. |
| `SCAN` | Multi-frequency node sampling. |
| `UPDATE` | Firmware transfer in progress; keep power and the host link connected. |

## Console commands

Close RH's serial connection before using the USB console; console commands
can take radio control back from RH.

- `status` — show firmware, radio checks and counters.
- `node led off` — disable the LED until reboot.
- `node led on` — re-enable it. Reboot also restores the default enabled setting.

See the [detailed LED guide](STATUS_LED.md) for connection timeouts and board wiring.
