# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(spi_page_target
    SYMBOL mclw_model_spi_page_target ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/spi_page.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(spi_page_controller
    SYMBOL mclw_model_spi_page_controller ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/spi_page.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
