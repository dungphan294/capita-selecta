# Pico 2 W — HTTP server

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

project(webserver C CXX ASM)

pico_sdk_init()

add_executable(webserver webserver.c)

pico_set_program_name(webserver "webserver")
pico_set_program_version(webserver "0.1")

pico_enable_stdio_uart(webserver 1)
pico_enable_stdio_usb(webserver 0)

# Local Wi-Fi credentials (gitignored). Copy wifi_secrets.cmake.example to
# wifi_secrets.cmake and fill it in. OPTIONAL: configuring still works without it.
include(${CMAKE_CURRENT_LIST_DIR}/wifi_secrets.cmake OPTIONAL)

target_compile_definitions(webserver PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

target_link_libraries(webserver
    pico_stdlib
    hardware_adc                              # on-die temperature sensor
    pico_cyw43_arch_lwip_threadsafe_background # WiFi + lwIP on a background IRQ
)

# lwipopts.h lives here
target_include_directories(webserver PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(webserver)
