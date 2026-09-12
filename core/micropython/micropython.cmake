# SPDX-License-Identifier: Apache-2.0 OR MIT
# The `metalclaw` user C module. Passed to the MicroPython build as USER_C_MODULES.

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/strict.cmake)

add_library(usermod_metalclaw INTERFACE)

set(METALCLAW_USERMOD_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/modmetalclaw.c
    ${CMAKE_CURRENT_LIST_DIR}/console.c
)
metalclaw_strict_sources(${METALCLAW_USERMOD_SOURCES})

target_sources(usermod_metalclaw INTERFACE ${METALCLAW_USERMOD_SOURCES})

target_include_directories(usermod_metalclaw INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_metalclaw)
metalclaw_strict_third_party(usermod)

include(${CMAKE_CURRENT_LIST_DIR}/../ports/rp2/port.cmake)
