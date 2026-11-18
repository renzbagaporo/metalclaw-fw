# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(sht3x
    SYMBOL mclw_model_sht3x ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/sht.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(sht4x
    SYMBOL mclw_model_sht4x ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/sht.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
