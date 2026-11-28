# SPDX-License-Identifier: Apache-2.0 OR MIT

metalclaw_device(pwm_capture
    SYMBOL mclw_model_pwm_capture ABI 1
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/pwm_capture.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)
