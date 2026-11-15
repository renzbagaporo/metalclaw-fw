# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(ssd1306
    SYMBOL mclw_model_ssd1306 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ssd130x.c ${CMAKE_CURRENT_LIST_DIR}/ssd130x_core.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(ssd1309
    SYMBOL mclw_model_ssd1309 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ssd130x.c ${CMAKE_CURRENT_LIST_DIR}/ssd130x_core.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
