# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(veml6031
    SYMBOL mclw_model_veml6031 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/veml60xx.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(veml6046
    SYMBOL mclw_model_veml6046 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/veml60xx.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
