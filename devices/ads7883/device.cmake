# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(ads7883
    SYMBOL mclw_model_ads7883 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ads7883.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
