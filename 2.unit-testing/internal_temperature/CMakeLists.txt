# Pico 2 W — RP2350 internal temperature sensor

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

project(internal_temperature C CXX ASM)

pico_sdk_init()

add_executable(internal_temperature internal_temperature.c)

pico_set_program_name(internal_temperature "internal_temperature")
pico_set_program_version(internal_temperature "0.1")

# Console over the Debug Probe's UART bridge, not USB:
# USB stdio stops whenever a core halts at a breakpoint.
pico_enable_stdio_uart(internal_temperature 1)
pico_enable_stdio_usb(internal_temperature 0)

target_link_libraries(internal_temperature
    pico_stdlib
    hardware_adc      # the on-chip ADC, which the temperature sensor feeds
)

target_include_directories(internal_temperature PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(internal_temperature)
