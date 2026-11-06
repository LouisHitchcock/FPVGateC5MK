#include "status_led.h"
#include <assert.h>
#include <stdio.h>

int main() {
    using statusled::level;
    assert(statusled::colour(false, false, false) == 0x040800); // amber
    assert(statusled::colour(true, false, false) == 0x080000); // green
    assert(statusled::colour(true, true, false) == 0x080008); // cyan
    assert(statusled::colour(true, true, true) == 0x000800); // red overrides traffic
    assert(level(0, false, false, false, true));
    assert(!level(200, false, false, false, true));
    assert(level(1000, false, false, false, true));
    assert(level(750, true, false, false, true));
    assert(level(50, true, true, false, true));
    assert(!level(150, true, true, false, true));
    assert(level(250, true, true, true, true));
    assert(!level(350, true, true, true, true));
    for (unsigned t = 0; t < 3000; ++t) {
        assert(!level(t, true, true, true, false));
        assert(level(t, true, false, false, true));
    }
    puts("status LED patterns: PASS");
}
