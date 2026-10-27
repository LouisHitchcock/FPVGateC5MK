// FPVGate RF-node mode: the UART link to an FPVGate S3 (receiverRadio = 2)
// or any other host, driving node::Core. See node_core.h and
// docs/LINK_PROTOCOL.md.
#ifndef MP_NODE_H
#define MP_NODE_H

#include "node_core.h"

namespace nodelink {

// UART up, gain from NVS, boot status line. radioOk false reports ERR_RF.
void begin(node::RadioPort* radio, bool radioOk);
// Read the UART. A valid command from the S3 switches node mode on.
void poll();
// Retunes, 1 kHz samples, heartbeat; and in node mode, a view of the
// readings for the USB dashboard.
void step();
bool active();
bool rhUsbActive();
// What the link UART has delivered. Host: a valid protocol session (FPVGate
// or any other speaker of it). Other: bytes that are not that protocol.
// None: the wire is quiet.
enum class UartPeer : uint8_t { None, Host, Other };
UartPeer uartPeer();
// Leaving node mode drops the frequency (status ERR_FREQ, MHz 0) so the
// scanner can use the radio; the S3's next F command takes it back.
void deactivate();
// USB console: "node ..." (args may be null).
void console(char* args);
// A new image from an update runs on trial and is rolled back unless this is
// called within a minute: done on the first valid command from the S3 or the
// USB console.
void confirmImage();
// A line for the USB status page.
void describe(char* out, size_t cap);

}  // namespace nodelink

#endif
