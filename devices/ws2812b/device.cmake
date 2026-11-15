# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(ws2812b
    SYMBOL mclw_model_ws2812b ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ws2812b.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
