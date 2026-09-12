# SPDX-License-Identifier: Apache-2.0 OR MIT
# The `metalclaw` user C module. Passed to the MicroPython build as USER_C_MODULES.

get_filename_component(METALCLAW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/strict.cmake)

add_subdirectory(${METALCLAW_ROOT}/core ${CMAKE_BINARY_DIR}/metalclaw_core)

add_library(usermod_metalclaw INTERFACE)

set(METALCLAW_USERMOD_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/modmetalclaw.c
    ${CMAKE_CURRENT_LIST_DIR}/instance.c
)
metalclaw_strict_sources(${METALCLAW_USERMOD_SOURCES})

target_sources(usermod_metalclaw INTERFACE ${METALCLAW_USERMOD_SOURCES})
target_include_directories(usermod_metalclaw INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${METALCLAW_ROOT}/core/include
)

target_link_libraries(usermod INTERFACE usermod_metalclaw)
metalclaw_strict_third_party(usermod)

# The core library is firmware, not module sources: it links to the firmware
# target (deferred until that target exists) so the qstr scan never sees it.
cmake_language(DEFER CALL target_link_libraries ${MICROPY_TARGET} metalclaw_core)

# The port implements the HAL for the chip and the MicroPython glue for it.
set(METALCLAW_PORT "rp2" CACHE STRING "which core/ports/<name>/port.cmake implements the HAL")
include(${CMAKE_CURRENT_LIST_DIR}/../ports/${METALCLAW_PORT}/port.cmake)
