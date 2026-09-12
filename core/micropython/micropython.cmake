# SPDX-License-Identifier: Apache-2.0 OR MIT
# The `metalclaw` user C module. Passed to the MicroPython build as USER_C_MODULES.

get_filename_component(METALCLAW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/strict.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/devices.cmake)

add_library(usermod_metalclaw INTERFACE)

set(METALCLAW_USERMOD_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/modmetalclaw.c
    ${CMAKE_CURRENT_LIST_DIR}/instance.c
    ${CMAKE_CURRENT_LIST_DIR}/console.c
)
metalclaw_strict_sources(${METALCLAW_USERMOD_SOURCES})

target_sources(usermod_metalclaw INTERFACE ${METALCLAW_USERMOD_SOURCES})

target_include_directories(usermod_metalclaw INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${METALCLAW_ROOT}/core/include
)

target_link_libraries(usermod INTERFACE usermod_metalclaw metalclaw_devices)
metalclaw_strict_third_party(usermod)

# Core and device sources are firmware sources, not module sources: they
# attach to the firmware target (deferred until it exists) so the qstr scan
# never preprocesses them.
set(METALCLAW_CORE_SOURCES
    ${METALCLAW_ROOT}/core/src/runtime.c
    ${METALCLAW_ROOT}/core/src/regmap.c
    ${METALCLAW_REGISTRY_SOURCE}
    ${METALCLAW_DEVICE_SOURCES}
)
metalclaw_strict_sources(${METALCLAW_CORE_SOURCES})
cmake_language(DEFER CALL target_sources ${MICROPY_TARGET} PRIVATE ${METALCLAW_CORE_SOURCES})

include(${CMAKE_CURRENT_LIST_DIR}/../ports/rp2/port.cmake)
