# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(bmp280
    SYMBOL mclw_model_bmp280 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmx280.c ${CMAKE_CURRENT_LIST_DIR}/bmx280_comp.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(bme280
    SYMBOL mclw_model_bme280 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmx280.c ${CMAKE_CURRENT_LIST_DIR}/bmx280_comp.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
