idf_component_register(
    SRCS "main.cpp" "dump.cpp" "radio.cpp" "meter.cpp" "frame.cpp" "link.cpp" "node.cpp" "node_core.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_phy nvs_flash esp_event esp_timer heap esp_driver_usb_serial_jtag esp_driver_uart)

# Per-board FPVGate link pins (platformio.ini board_build.cmake_extra_args);
# PlatformIO's build_flags don't reach these sources in an ESP-IDF build.
foreach(pin NODE_UART_TX_PIN NODE_UART_RX_PIN)
    if(DEFINED ${pin})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${pin}=${${pin}})
    endif()
endforeach()
