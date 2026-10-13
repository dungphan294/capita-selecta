# Pico 2 W — reference BLE sensor
#
# The known-good peripheral used to verify setup_portal (reads its
# advertisements) and ble_central (connects and reads its GATT database).

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(ble_sensor C CXX ASM)

pico_sdk_init()

add_executable(ble_sensor ble_sensor.c)

pico_set_program_name(ble_sensor "ble_sensor")
pico_set_program_version(ble_sensor "1.0")

# Console over the Debug Probe's UART bridge, not USB.
pico_enable_stdio_uart(ble_sensor 1)
pico_enable_stdio_usb(ble_sensor 0)

# portal.gatt -> portal_gatt.h, via BTstack's compile_gatt.py
pico_btstack_make_gatt_header(ble_sensor PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/sensor_gatt.gatt)

target_link_libraries(ble_sensor
    pico_stdlib
    hardware_adc                  # on-die temperature sensor
    pico_btstack_ble              # BTstack LE, peripheral role
    pico_btstack_cyw43            # BTstack <-> CYW43439 transport
    pico_cyw43_arch_none          # CYW43 driver, no lwIP (this one has no WiFi)
)

# btstack_config.h lives here
target_include_directories(ble_sensor PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(ble_sensor)
