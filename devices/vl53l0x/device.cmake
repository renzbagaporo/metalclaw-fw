# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(vl53l0x
    SYMBOL mclw_model_vl53l0x ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/vl53l0x.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
