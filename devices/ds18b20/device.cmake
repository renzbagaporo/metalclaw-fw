# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(ds18b20
    SYMBOL mclw_model_ds18b20 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ds18b20.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
