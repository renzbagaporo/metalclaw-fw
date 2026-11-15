# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(mpu6050
    SYMBOL mclw_model_mpu6050 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/mpu6050.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
