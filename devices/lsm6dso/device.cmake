# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(lsm6dso
    SYMBOL mclw_model_lsm6dso ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/lsm6dso.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(lsm6dsl
    SYMBOL mclw_model_lsm6dsl ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/lsm6dso.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
