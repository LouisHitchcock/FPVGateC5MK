#pragma once
#include <stddef.h>
#include <stdint.h>

struct usb_serial_jtag_driver_config_t {
    size_t tx_buffer_size;
    size_t rx_buffer_size;
};
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() usb_serial_jtag_driver_config_t{256, 256}
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*);
int usb_serial_jtag_read_bytes(void*, size_t, uint32_t);
int usb_serial_jtag_write_bytes(const void*, size_t, uint32_t);
