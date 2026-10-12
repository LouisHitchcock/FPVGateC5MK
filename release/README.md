# C5MK firmware releases

Ready-built firmware for each supported board, for the release this tag
marks:

| Folder | Board |
|---|---|
| `c5zero/` | Waveshare ESP32-C5-Zero |
| `xiaoc5/` | Seeed Studio XIAO ESP32-C5 |

The two boards use different link pins, so flash the folder for your board.
Each image carries its board in its version (`1.1.0+c5zero`), and FPVGate
won't send an image to a C5 of the other board.

## First flash, over USB

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
