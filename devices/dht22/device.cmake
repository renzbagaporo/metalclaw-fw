# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(dht22
    SYMBOL mclw_model_dht22 ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/dht22.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
