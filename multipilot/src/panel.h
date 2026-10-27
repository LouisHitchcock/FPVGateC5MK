// Optional local display. Empty on every board except the NM-CYD-C5 build
// (C5MK_PANEL). The scan task publishes readings and paints between cycles;
// the link UART is unchanged.
#ifndef MP_PANEL_H
#define MP_PANEL_H

#include <stdint.h>

#ifdef C5MK_PANEL
// mhz is the pilot frequency, rssi is 0..1023. host is true while FPVGate
// owns the radio. Safe to call on every sample: it only stores the latest.
void panelNote(uint16_t mhz, uint16_t rssi, uint8_t gain, bool host);
// SPI init and the first paint. Call once from the scan task, before the
// scan loop. A failure leaves the receiver running with the screen off.
void panelBegin();
// One small redraw when its interval has elapsed, otherwise returns at once.
void panelService();
#else
inline void panelNote(uint16_t, uint16_t, uint8_t, bool) {}
inline void panelBegin() {}
inline void panelService() {}
#endif

#endif
