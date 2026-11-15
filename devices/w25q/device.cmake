# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(w25q16jv
    SYMBOL mclw_model_w25q16jv ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/w25q.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
