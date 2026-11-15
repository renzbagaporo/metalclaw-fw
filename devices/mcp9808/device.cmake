# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(mcp9808
    SYMBOL mclw_model_mcp9808 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/mcp9808.c ${CMAKE_CURRENT_LIST_DIR}/mcp9808_temp.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
