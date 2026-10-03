# SPDX-License-Identifier: Apache-2.0 OR MIT

foreach(model sx1262 sx1261 sx1268 llcc68)
    metalclaw_device(${model}
        SYMBOL mclw_model_${model} ABI 1
        SOURCES ${CMAKE_CURRENT_LIST_DIR}/sx126x.c
        INCLUDE ${CMAKE_CURRENT_LIST_DIR}
    )
endforeach()
