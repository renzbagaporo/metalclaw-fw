# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(hd44780_pcf8574
    SYMBOL mclw_model_hd44780_pcf8574 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/hd44780_pcf8574.c ${CMAKE_CURRENT_LIST_DIR}/hd44780_core.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
