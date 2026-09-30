# Pico 2 W BLE scanner

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

project(bluetooth C CXX ASM)

pico_sdk_init()

add_executable(bluetooth bluetooth.c)

pico_set_program_name(bluetooth "bluetooth")
pico_set_program_version(bluetooth "0.1")

# Console over the Debug Probe's UART bridge, not USB:
# USB stdio stops whenever a core halts at a breakpoint.
pico_enable_stdio_uart(bluetooth 1)
pico_enable_stdio_usb(bluetooth 0)

target_link_libraries(bluetooth
    pico_stdlib
    pico_btstack_ble              # BTstack Bluetooth Low Energy
    pico_btstack_cyw43            # BTstack <-> CYW43439 HCI transport
    pico_cyw43_arch_none          # CYW43 driver, no lwIP (BLE needs no TCP/IP)
)

# btstack_config.h lives here
target_include_directories(bluetooth PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(bluetooth)
