# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(jumper
    SYMBOL mclw_model_jumper ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/jumper.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(jumper_open_drain
    SYMBOL mclw_model_jumper_open_drain ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/jumper.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(jumper_open_drain_clock
    SYMBOL mclw_model_jumper_open_drain_clock ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/jumper.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
metalclaw_device(jumper_open_drain_clock_edge
    SYMBOL mclw_model_jumper_open_drain_clock_edge ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/jumper.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
