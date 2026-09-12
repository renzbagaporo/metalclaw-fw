# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness core as one static library: the runtime, the image helpers,
# the generated registry, and every device. Its only include directory is
# core/include, so a device or core source that reaches for an SDK, a port
# or MicroPython header fails to compile. Ports and bindings link it.

cmake_minimum_required(VERSION 3.20)
project(metalclaw_core C)

set(METALCLAW_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(METALCLAW_ROOT "${METALCLAW_ROOT}" ABSOLUTE)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/strict.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/devices.cmake)

set(METALCLAW_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/src/regmap.c
    ${CMAKE_CURRENT_LIST_DIR}/src/stream.c
    ${METALCLAW_REGISTRY_SOURCE}
    ${METALCLAW_DEVICE_SOURCES}
)
metalclaw_strict_sources(${METALCLAW_CORE_SOURCES})

add_library(metalclaw_core STATIC ${METALCLAW_CORE_SOURCES})
target_include_directories(metalclaw_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(metalclaw_core PRIVATE ${METALCLAW_DEVICE_INCLUDE_DIRS})
