# SPDX-License-Identifier: Apache-2.0 OR MIT
# The `metalclaw` user C module. Passed to the MicroPython build as USER_C_MODULES.

add_library(usermod_metalclaw INTERFACE)

target_sources(usermod_metalclaw INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modmetalclaw.c
)

target_include_directories(usermod_metalclaw INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_metalclaw)
