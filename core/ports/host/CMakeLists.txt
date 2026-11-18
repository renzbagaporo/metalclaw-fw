# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host port as a static library, and the core (runtime, registry and
# every device) beside it, both for the host's compiler. MicroPython's unix
# port links them through core/micropython/micropython.mk.

cmake_minimum_required(VERSION 3.20)
project(metalclaw_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. ${CMAKE_BINARY_DIR}/core)

set(METALCLAW_PORT_HOST_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/platform.c
    ${CMAKE_CURRENT_LIST_DIR}/pins.c
    ${CMAKE_CURRENT_LIST_DIR}/gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/i2c_target.c
    ${CMAKE_CURRENT_LIST_DIR}/bridge.c
    ${CMAKE_CURRENT_LIST_DIR}/json.c
    ${CMAKE_CURRENT_LIST_DIR}/unsupported.c
)
metalclaw_strict_sources(${METALCLAW_PORT_HOST_SOURCES})

add_library(metalclaw_port_host STATIC ${METALCLAW_PORT_HOST_SOURCES})
target_include_directories(metalclaw_port_host PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)
