# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(ds3231
    SYMBOL mclw_model_ds3231 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ds3231.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
