#!/usr/bin/env bash
# PC tests for the portable parts of the firmware (meter, filter, framing),
# the RF-node link protocol, and the reference host library in ../host.
# No hardware needed.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/test
FLAGS="-std=c++14 -O1 -Wall -Wextra -I src -I ../host/c5host"
g++ $FLAGS src/meter.cpp src/frame.cpp test/test_mp.cpp -o .pio/test/test_mp.exe
g++ $FLAGS src/meter.cpp src/node_core.cpp test/test_node.cpp -o .pio/test/test_node.exe
g++ $FLAGS src/meter.cpp src/node_core.cpp ../host/c5host/c5host.cpp test/test_host.cpp -o .pio/test/test_host.exe
g++ $FLAGS src/rh_usb.cpp test/test_rh_usb.cpp -o .pio/test/test_rh_usb.exe
# Replays RSSI through rh_usb for tools/rh_db_report.py --replay (built, not run here).
g++ $FLAGS src/rh_usb.cpp test/rh_replay.cpp -o .pio/test/rh_replay.exe
g++ $FLAGS -I test/stubs src/link.cpp src/frame.cpp src/rh_usb.cpp test/test_link.cpp -o .pio/test/test_link.exe
./.pio/test/test_mp.exe
./.pio/test/test_node.exe
./.pio/test/test_host.exe
./.pio/test/test_rh_usb.exe
./.pio/test/test_link.exe
# The browser lap timer's pass detection (web/laps.js), if Node is installed.
if command -v node >/dev/null 2>&1; then node test/test_laps.mjs; else echo "test_laps: node not found, skipped"; fi
