set(MP_SRCS "main.cpp" "dump.cpp" "radio.cpp" "meter.cpp" "frame.cpp" "link.cpp" "node.cpp" "node_core.cpp" "rh_usb.cpp")
set(MP_REQS esp_wifi esp_phy nvs_flash esp_event esp_timer heap esp_driver_usb_serial_jtag esp_driver_uart
            app_update esp_app_format mbedtls)
# Display and touchscreen are a different binary. c5zero and xiaoc5 do not
# compile panel.cpp, and they do not get C5MK_PANEL, so those images keep
# the SRAM layout check_sram.py already allows.
if(NODE_BOARD STREQUAL "cydc5")
    list(APPEND MP_SRCS "panel.cpp")
    # GPIO only. The SPI master driver puts several KB of IRAM in the same
    # SRAM the I/Q dump uses, and check_sram.py then fails.
    list(APPEND MP_REQS esp_driver_gpio)
endif()

idf_component_register(
    SRCS ${MP_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${MP_REQS})

if(NODE_BOARD STREQUAL "cydc5")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C5MK_PANEL=1)
endif()

# Per-board FPVGate link pins (platformio.ini board_build.cmake_extra_args);
# PlatformIO's build_flags don't reach these sources in an ESP-IDF build.
foreach(pin NODE_UART_TX_PIN NODE_UART_RX_PIN)
    if(DEFINED ${pin})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${pin}=${${pin}})
    endif()
endforeach()
