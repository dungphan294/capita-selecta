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

# == Web assets ==
# Everything under web/ is gzipped and emitted as const arrays in .rodata, so
# the server can send it straight out of memory-mapped flash with no copy into
# RAM. Editing a file under web/ is enough to trigger a regeneration.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets)
set(ASSETS_C  ${ASSET_DIR}/assets.c)
set(ASSETS_H  ${ASSET_DIR}/assets.h)

file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/web/*)

add_custom_command(
    OUTPUT  ${ASSETS_C} ${ASSETS_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASSET_DIR}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_assets.py
            ${CMAKE_CURRENT_LIST_DIR}/web ${ASSETS_C} ${ASSETS_H}
    DEPENDS ${WEB_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/tools/embed_assets.py
    COMMENT "Embedding web/ into flash"
    VERBATIM
)

add_executable(webserver
    webserver.c
    http_server.c
    wifi_scan.c
    wifi_store.c
    dhcpserver.c
    dnsserver.c
    ${ASSETS_C}
)

pico_set_program_name(webserver "webserver")
pico_set_program_version(webserver "0.1")

pico_enable_stdio_uart(webserver 1)
pico_enable_stdio_usb(webserver 0)

# Local Wi-Fi credentials (gitignored). Entirely optional — with no secrets file
# the board comes up hosting its own setup access point instead. Copy
# wifi_secrets.cmake.example to wifi_secrets.cmake to pre-seed a network.
include(${CMAKE_CURRENT_LIST_DIR}/wifi_secrets.cmake OPTIONAL)

target_compile_definitions(webserver PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

target_link_libraries(webserver
    pico_stdlib
    hardware_adc                              # on-die temperature sensor
    hardware_flash                            # stored credentials
    hardware_sync                             # interrupt lockout around flash writes
    hardware_watchdog                         # restart after "forget network"
    pico_cyw43_arch_lwip_threadsafe_background # WiFi + lwIP on a background IRQ
)

# lwipopts.h lives here; assets.h is generated into the build tree
target_include_directories(webserver PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${ASSET_DIR}
)

pico_add_extra_outputs(webserver)
