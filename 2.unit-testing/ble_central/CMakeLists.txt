# Pico 2 W — BLE central / GATT client
#
# Optional build-time target selection:
#   cmake -DTARGET_NAME="SensorTag" ..
#   cmake -DTARGET_ADDR="A4:C1:38:11:22:33" ..

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

project(ble_central C CXX ASM)

pico_sdk_init()

add_executable(ble_central
    ble_central.c
    ble_names.c
)

pico_set_program_name(ble_central "ble_central")
pico_set_program_version(ble_central "1.0")

# Console over the Debug Probe's UART bridge, not USB:
# USB stdio stops whenever a core halts at a breakpoint.
pico_enable_stdio_uart(ble_central 1)
pico_enable_stdio_usb(ble_central 0)

target_link_libraries(ble_central
    pico_stdlib
    pico_btstack_ble              # BTstack LE, including the GATT client
    pico_btstack_cyw43            # BTstack <-> CYW43439 HCI transport
    pico_cyw43_arch_none          # CYW43 driver, no lwIP (BLE needs no TCP/IP)
)

# btstack_config.h lives here
target_include_directories(ble_central PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Target selection, if given on the cmake command line.
if(TARGET_NAME)
    target_compile_definitions(ble_central PRIVATE TARGET_NAME="${TARGET_NAME}")
    message(STATUS "BLE target name: ${TARGET_NAME}")
endif()
if(TARGET_ADDR)
    target_compile_definitions(ble_central PRIVATE TARGET_ADDR="${TARGET_ADDR}")
    message(STATUS "BLE target address: ${TARGET_ADDR}")
endif()

pico_add_extra_outputs(ble_central)
