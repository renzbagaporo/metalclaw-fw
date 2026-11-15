# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(tsop38238
    SYMBOL mclw_model_tsop38238 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/tsop382.c ${CMAKE_CURRENT_LIST_DIR}/nec.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
