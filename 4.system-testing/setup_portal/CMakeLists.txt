# Pico 2 W — BLE + WiFi setup portal
#
# No wifi_secrets.cmake here, deliberately: credentials arrive at runtime over
# BLE or through the portal page and live in flash. Compile-time credentials
# are what caused defect #4 in the WiFi project.

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

project(setup_portal C CXX ASM)

pico_sdk_init()

# == Web assets ==
# web/ is gzipped and emitted as const arrays in .rodata, so http_portal can
# send it straight out of memory-mapped flash without copying into RAM.
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

add_executable(setup_portal
    ${ASSETS_C}
    setup_portal.c
    config_store.c
    wifi_manager.c
    dhcpserver.c
    http_portal.c
    ble_portal.c
    portal_actions.c
)

pico_set_program_name(setup_portal "setup_portal")
pico_set_program_version(setup_portal "1.0")

# Console over the Debug Probe's UART bridge, not USB.
pico_enable_stdio_uart(setup_portal 1)
pico_enable_stdio_usb(setup_portal 0)

# portal.gatt -> portal_gatt.h, via BTstack's compile_gatt.py
pico_btstack_make_gatt_header(setup_portal PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/portal_gatt.gatt)

target_link_libraries(setup_portal
    pico_stdlib
    pico_flash                                  # flash_safe_execute for settings
    pico_btstack_ble                            # BTstack LE, both roles
    pico_btstack_cyw43                          # BTstack <-> CYW43439 transport
    pico_cyw43_arch_lwip_threadsafe_background  # WiFi + lwIP on the same async context
)

# btstack_config.h and lwipopts.h live here
target_include_directories(setup_portal PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${ASSET_DIR}
)

pico_add_extra_outputs(setup_portal)
