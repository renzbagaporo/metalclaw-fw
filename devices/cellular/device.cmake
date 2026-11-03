# SPDX-License-Identifier: Apache-2.0 OR MIT

set(_cellular_engine
    ${CMAKE_CURRENT_LIST_DIR}/at.c
    ${CMAKE_CURRENT_LIST_DIR}/cmux.c
    ${CMAKE_CURRENT_LIST_DIR}/dns.c
    ${CMAKE_CURRENT_LIST_DIR}/ip.c
    ${CMAKE_CURRENT_LIST_DIR}/link.c
    ${CMAKE_CURRENT_LIST_DIR}/modem.c
    ${CMAKE_CURRENT_LIST_DIR}/net.c
    ${CMAKE_CURRENT_LIST_DIR}/parts.c
    ${CMAKE_CURRENT_LIST_DIR}/ppp.c
    ${CMAKE_CURRENT_LIST_DIR}/sms.c
    ${CMAKE_CURRENT_LIST_DIR}/standard.c
    ${CMAKE_CURRENT_LIST_DIR}/tcp.c
    ${CMAKE_CURRENT_LIST_DIR}/text.c
    ${CMAKE_CURRENT_LIST_DIR}/udp.c
)

metalclaw_device(sim7080g
    SYMBOL mclw_model_sim7080g ABI 1
    SOURCES
        ${_cellular_engine}
        ${CMAKE_CURRENT_LIST_DIR}/sim7080g.c
        ${CMAKE_CURRENT_LIST_DIR}/sim7080g_sockets.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)

metalclaw_device(sim7000
    SYMBOL mclw_model_sim7000 ABI 1
    SOURCES
        ${_cellular_engine}
        ${CMAKE_CURRENT_LIST_DIR}/sim7000.c
        ${CMAKE_CURRENT_LIST_DIR}/sim7000_cip.c
        ${CMAKE_CURRENT_LIST_DIR}/sim7000_ca.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)

metalclaw_device(a7672x
    SYMBOL mclw_model_a7672x ABI 1
    SOURCES
        ${_cellular_engine}
        ${CMAKE_CURRENT_LIST_DIR}/a7672x.c
        ${CMAKE_CURRENT_LIST_DIR}/a7672x_sockets.c
        ${CMAKE_CURRENT_LIST_DIR}/a7672x_ssl.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)

metalclaw_device(sequans_monarch
    SYMBOL mclw_model_sequans_monarch ABI 1
    SOURCES
        ${_cellular_engine}
        ${CMAKE_CURRENT_LIST_DIR}/sequans_monarch.c
        ${CMAKE_CURRENT_LIST_DIR}/sequans_monarch_sockets.c
    INCLUDE ${CMAKE_CURRENT_LIST_DIR}
)

unset(_cellular_engine)
