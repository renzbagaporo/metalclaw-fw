# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(pca9633
    SYMBOL mclw_model_pca9633 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/pca9633.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
