# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(mpu9250
    SYMBOL mclw_model_mpu9250 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/mpu9250.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
