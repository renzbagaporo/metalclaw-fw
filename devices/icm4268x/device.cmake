# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(icm42688
    SYMBOL mclw_model_icm42688 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/icm4268x.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(icm42686
    SYMBOL mclw_model_icm42686 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/icm4268x.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
