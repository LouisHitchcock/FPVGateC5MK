# C5MK firmware releases

Ready-built firmware for each supported board, for the release this tag
marks:

| Folder | Board |
|---|---|
| `c5zero/` | Waveshare ESP32-C5-Zero |
| `xiaoc5/` | Seeed Studio XIAO ESP32-C5 |

The two boards use different link pins, so flash the folder for your board.
Each image carries its board in its version (`1.2.2-rc.3+c5zero`), and FPVGate
won't send an image to a C5 of the other board.

## First flash, over USB

From 1.2.0, the same board-specific firmware also supports direct USB timing
in RotorHazard, without an S3 or FPVGate plugin. RH's revision request selects
that mode automatically. Add the USB serial port in RH's Advanced Settings
and restart RH; see [the USB setup guide](../docs/USB_ROTORHAZARD.md).

A new C5, or one on 1.0 or earlier, needs the full set once over its USB
port. 1.1.0 adds a second app slot for updates, so the partition table
changes and the older layout can't be updated over the link.

```text
pip install esptool
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Run it from inside the board's folder. If the C5 doesn't connect, hold BOOT
while plugging it in. Older esptool versions spell the command `write_flash`
with an underscore, as above; esptool 5 also accepts `write-flash`.

Then check it over the same USB port:

```text
python ../../multipilot/tools/mp.py --port <port> cmd "status"
```

## Later updates, through FPVGate

From 1.1.0, an FPVGate on 1.9.0-Multi-Alpha-3 or later updates the C5 over
the UART link, with no USB cable: **Settings > System > C5 Receiver
Firmware**. It downloads `firmware.bin` for the C5's board from this folder
at the chosen release tag. The new image runs on trial, and if it can't
reach the gate within 60 s the C5 goes back to its previous firmware. See
[docs/LINK_PROTOCOL.md](../docs/LINK_PROTOCOL.md#firmware-update-firmware-3).

Only `firmware.bin` goes over the link. The bootloader, partition table and
OTA data are for the first USB flash.

1.2.x keeps the 1.1.0 partition layout. A C5 already running 1.1.0 or 1.2.0
can receive the application update through FPVGate. Direct USB users should
close RH's serial connection before flashing. Do not use RH's STM32 node
updater on a C5.

Both boards have built binaries. Waveshare USB discovery and local timing
have been tested; physical XIAO USB validation is pending. See
[1.2.2-rc.3 release notes](RELEASE_NOTES.md) for the validation scope.

`SHA256SUMS.txt` contains hashes using the board-prefixed GitHub asset names.
The repository's `release/<board>/` files contain the same bytes.
