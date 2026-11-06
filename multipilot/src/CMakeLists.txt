idf_component_register(
    SRCS "main.cpp" "dump.cpp" "radio.cpp" "meter.cpp" "frame.cpp" "link.cpp" "node.cpp" "node_core.cpp" "rh_usb.cpp" "status_led.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_phy nvs_flash esp_event esp_timer heap esp_driver_usb_serial_jtag esp_driver_uart
                  app_update esp_app_format mbedtls esp_driver_gpio esp_driver_rmt)

# Per-board FPVGate link pins (platformio.ini board_build.cmake_extra_args);
# PlatformIO's build_flags don't reach these sources in an ESP-IDF build.
foreach(pin NODE_UART_TX_PIN NODE_UART_RX_PIN)
    if(DEFINED ${pin})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${pin}=${${pin}})
    endif()
endforeach()

if(NODE_BOARD STREQUAL "xiaoc5")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STATUS_LED_XIAO=1)
endif()
