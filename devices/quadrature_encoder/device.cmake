# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(quadrature_encoder
    SYMBOL mclw_model_quadrature_encoder ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/quadrature_encoder.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
