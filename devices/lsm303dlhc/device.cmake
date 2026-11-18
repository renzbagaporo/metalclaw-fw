# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(lsm303dlhc_magn
    SYMBOL mclw_model_lsm303dlhc_magn ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/lsm303dlhc_magn.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
