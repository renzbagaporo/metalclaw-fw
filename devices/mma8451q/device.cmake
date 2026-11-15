# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(mma8451q
    SYMBOL mclw_model_mma8451q ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/mma8451q.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
