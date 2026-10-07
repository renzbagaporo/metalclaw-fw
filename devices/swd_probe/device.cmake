# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(swd_probe
    SYMBOL mclw_model_swd_probe ABI 1
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/dap.c
        ${CMAKE_CURRENT_LIST_DIR}/swd_probe.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
