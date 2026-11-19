# SPDX-License-Identifier: Apache-2.0 OR MIT
# Built like a pico-examples program: cmake -DPICO_SDK_PATH=... -DPICO_BOARD=pico|pico2.
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(i2c_target_page C CXX ASM)
pico_sdk_init()
add_executable(i2c_target_page i2c_target_page.c)
target_link_libraries(i2c_target_page pico_stdlib pico_i2c_slave hardware_i2c)
pico_enable_stdio_uart(i2c_target_page 1)
pico_add_extra_outputs(i2c_target_page)
