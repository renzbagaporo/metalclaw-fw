# SPDX-License-Identifier: Apache-2.0 OR MIT
# Included from the usermod cmake, inside ports/rp2's directory scope.
#
# Port sources are firmware sources, not module sources: they go on the
# firmware target itself, which does not exist yet at this point, so the
# call is deferred to the end of the directory. That also keeps them out
# of the qstr scan, which cannot preprocess SDK headers.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict.cmake)

set(METALCLAW_PORT_RP2_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/usb/descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/usb/interfaces.c
    ${CMAKE_CURRENT_LIST_DIR}/usb/owner.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/platform.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/pins.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/swd_pio.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/i2c_target.c
)
metalclaw_strict_sources(${METALCLAW_PORT_RP2_SOURCES})

cmake_language(DEFER CALL target_sources ${MICROPY_TARGET} PRIVATE ${METALCLAW_PORT_RP2_SOURCES})
# pico-sdk libraries the port needs beyond what MicroPython links; on the
# firmware target directly so the qstr scan never sees their sources.
cmake_language(DEFER CALL target_link_libraries ${MICROPY_TARGET} pico_i2c_slave)

add_library(metalclaw_port_rp2 INTERFACE)

target_include_directories(metalclaw_port_rp2 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/usb
)

target_compile_definitions(metalclaw_port_rp2 INTERFACE
    CFG_TUSB_CONFIG_FILE="metalclaw_tusb_config.h"
)

target_link_libraries(usermod INTERFACE metalclaw_port_rp2)
