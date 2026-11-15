# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(lis3dh
    SYMBOL mclw_model_lis3dh ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/lis3dh.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
