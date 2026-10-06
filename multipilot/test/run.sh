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
./.pio/test/test_mp.exe
./.pio/test/test_node.exe
./.pio/test/test_host.exe
