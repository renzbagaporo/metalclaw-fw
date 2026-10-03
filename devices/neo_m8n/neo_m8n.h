// SPDX-License-Identifier: Apache-2.0 OR MIT
// u-blox NEO-M8N: the UBX protocol subset the part answers on its UART and
// the NMEA output it starts with, from the u-blox 8 / M8 receiver
// description (protocol 18, firmware SPG 3.01).

#ifndef MCLW_DEVICE_NEO_M8N_H
#define MCLW_DEVICE_NEO_M8N_H

#include <stdint.h>

#define UBX_SYNC_1 0xb5
#define UBX_SYNC_2 0x62
#define UBX_HEADER_BYTES 6
#define UBX_TRAILER_BYTES 2
#define UBX_PAYLOAD_MAX 64

#define UBX_CLASS_NAV 0x01
#define UBX_CLASS_RXM 0x02
#define UBX_CLASS_ACK 0x05
#define UBX_CLASS_CFG 0x06
#define UBX_CLASS_MON 0x0a
#define UBX_CLASS_NMEA 0xf0

#define UBX_NAV_PVT 0x07
#define UBX_RXM_PMREQ 0x41
#define UBX_ACK_NAK 0x00
#define UBX_ACK_ACK 0x01
#define UBX_CFG_PRT 0x00
#define UBX_CFG_MSG 0x01
#define UBX_CFG_INF 0x02
#define UBX_CFG_RST 0x04
#define UBX_CFG_RATE 0x08
#define UBX_CFG_CFG 0x09
#define UBX_CFG_RXM 0x11
#define UBX_CFG_NMEA 0x17
#define UBX_CFG_NAVX5 0x23
#define UBX_CFG_NAV5 0x24
#define UBX_CFG_ITFM 0x39
#define UBX_CFG_PM2 0x3b
#define UBX_CFG_GNSS 0x3e
#define UBX_CFG_PMS 0x86
#define UBX_MON_VER 0x04
#define UBX_MON_GNSS 0x28

#define UBX_CFG_PRT_LEN 20
#define UBX_CFG_PRT_PORT_UART1 1
#define UBX_CFG_PRT_MODE_8N1 0x000008c0
#define UBX_CFG_MSG_POLL_LEN 2
#define UBX_CFG_MSG_LEN 3
#define UBX_CFG_MSG_PORTS_LEN 8
#define UBX_CFG_MSG_PORT_UART1 1
#define UBX_CFG_INF_BLOCK_LEN 10
#define UBX_CFG_RST_LEN 4
#define UBX_CFG_RST_STOP 0x08
#define UBX_CFG_RST_START 0x09
#define UBX_CFG_RATE_LEN 6
#define UBX_CFG_CFG_LEN 12
#define UBX_CFG_CFG_DEVICE_LEN 13
#define UBX_CFG_RXM_LEN 2
#define UBX_CFG_NMEA_V0_SHORT_LEN 4
#define UBX_CFG_NMEA_V0_LEN 12
#define UBX_CFG_NMEA_V1_LEN 20
#define UBX_CFG_NAVX5_LEN 40
#define UBX_CFG_NAV5_LEN 36
#define UBX_CFG_NAV5_APPLY_DYN 0x0001
#define UBX_CFG_ITFM_LEN 8
#define UBX_CFG_PM2_LEN 44
#define UBX_CFG_GNSS_HEADER_LEN 4
#define UBX_CFG_GNSS_BLOCK_LEN 8
#define UBX_CFG_GNSS_ENABLE 0x01
#define UBX_CFG_PMS_LEN 8
#define UBX_RXM_PMREQ_V0_LEN 8
#define UBX_RXM_PMREQ_V1_LEN 16
#define UBX_RXM_PMREQ_BACKUP 0x02
#define UBX_RXM_PMREQ_WAKE_UARTRX 0x08
#define UBX_MON_VER_LEN 40
#define UBX_MON_VER_EXTENSION_LEN 30
#define UBX_MON_GNSS_LEN 8
#define UBX_NAV_PVT_LEN 92

#define UBX_PROTO_UBX 0x0001
#define UBX_PROTO_NMEA 0x0002
#define UBX_PROTO_RTCM 0x0004

#define UBX_NAV_PVT_VALID_DATE 0x01
#define UBX_NAV_PVT_VALID_TIME 0x02
#define UBX_NAV_PVT_VALID_FULLY_RESOLVED 0x04
#define UBX_NAV_PVT_FLAGS_GNSS_FIX_OK 0x01
#define UBX_NAV_PVT_FIX_NONE 0
#define UBX_NAV_PVT_FIX_DEAD_RECKONING 1
#define UBX_NAV_PVT_FIX_3D 3
#define UBX_NAV_PVT_FIX_TIME_ONLY 5

// MON-GNSS bits, which the model also keeps the enabled set in.
#define UBX_GNSS_GPS 0x01
#define UBX_GNSS_GLONASS 0x02
#define UBX_GNSS_BEIDOU 0x04
#define UBX_GNSS_GALILEO 0x08

// CFG-GNSS gnssId.
#define UBX_GNSS_ID_GPS 0
#define UBX_GNSS_ID_GALILEO 2
#define UBX_GNSS_ID_BEIDOU 3
#define UBX_GNSS_ID_GLONASS 6

// NMEA message ids, the UBX class 0xF0 ids CFG-MSG addresses them by.
#define NMEA_GGA 0
#define NMEA_GLL 1
#define NMEA_GSA 2
#define NMEA_GSV 3
#define NMEA_RMC 4
#define NMEA_VTG 5
#define NMEA_STANDARD_COUNT 6

#define NMEA_VERSION_21 0x21
#define NMEA_VERSION_23 0x23
#define NMEA_VERSION_40 0x40
#define NMEA_VERSION_41 0x41

// An input line, $ to the last checksum digit; NMEA 0183 caps it at 82
// with CR LF.
#define NMEA_LINE_MAX 80
#define NMEA_SENTENCE_MAX 112
#define NMEA_GSA_SVS 12
#define NMEA_GSV_SVS_PER_PAGE 4
#define NMEA_DOP_UNKNOWN 9999
#define NMEA_GLONASS_SVID_BASE 65

#define NEO_M8N_SW_VERSION "ROM CORE 3.01 (107888)"
#define NEO_M8N_HW_VERSION "00080000"

#define NEO_M8N_DEFAULT_BAUD 9600
#define NEO_M8N_DEFAULT_RATE_MS 1000
#define NEO_M8N_DEFAULT_NAV_RATE 1
#define NEO_M8N_DEFAULT_TIME_REF 1
#define NEO_M8N_DEFAULT_DYN_MODEL 0
#define NEO_M8N_DEFAULT_PDOP 250
#define NEO_M8N_DEFAULT_HDOP 150
#define NEO_M8N_DEFAULT_VDOP 200
#define NEO_M8N_DEFAULT_IN_PROTO (UBX_PROTO_UBX | UBX_PROTO_NMEA | UBX_PROTO_RTCM)
#define NEO_M8N_DEFAULT_OUT_PROTO (UBX_PROTO_UBX | UBX_PROTO_NMEA)
#define NEO_M8N_DEFAULT_NMEA_VERSION NMEA_VERSION_40
#define NEO_M8N_DEFAULT_GNSS (UBX_GNSS_GPS | UBX_GNSS_GLONASS)

// Bytes the model lets wait in the engine's transmit queue before it holds
// the next NMEA sentence back. Small, so a UBX reply never sits long behind
// an epoch's NMEA and a baud change after CFG-PRT drains quickly.
#define NEO_M8N_TX_BUDGET 256

#define NEO_M8N_RATE_MS_MIN 25
#define NEO_M8N_RATE_MS_MAX 65535
#define NEO_M8N_BAUD_MIN 4800
#define NEO_M8N_BAUD_MAX 921600
#define NEO_M8N_LATITUDE_MAX 900000000
#define NEO_M8N_LONGITUDE_MAX 1800000000
#define NEO_M8N_HEADING_MAX 35999999
#define NEO_M8N_SATELLITES_MAX 32

#endif
