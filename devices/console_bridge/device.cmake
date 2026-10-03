# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(console_bridge
    SYMBOL mclw_model_console_bridge ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/console_bridge.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
