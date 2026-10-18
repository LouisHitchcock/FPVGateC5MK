#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/test/linux
# Baseline differs in exactly the parser guard fixed by this change. All other
# parser/protocol source is identical, so future HEAD changes cannot hide it.
sed 's/if (byteHandler && byteHandler(c))/if (byteHandler \&\& (binaryMode || (!lineLen \&\& !lineOverflow)) \&\& byteHandler(c))/' \
    src/link.cpp > .pio/test/linux/link_baseline.cpp
flags=(-std=c++14 -O1 -Wall -Wextra -I src -I test/stubs -pthread)
g++ "${flags[@]}" src/link.cpp src/frame.cpp src/rh_usb.cpp test/linux_usb_bridge.cpp -o .pio/test/linux/linux_usb_fixed
g++ "${flags[@]}" .pio/test/linux/link_baseline.cpp src/frame.cpp src/rh_usb.cpp test/linux_usb_bridge.cpp -o .pio/test/linux/linux_usb_baseline
python3 -m unittest discover -s test -p test_linux_usb.py -v
