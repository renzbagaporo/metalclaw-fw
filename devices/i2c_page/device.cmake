# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(i2c_page_controller
    SYMBOL mclw_model_i2c_page_controller ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/i2c_page.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
