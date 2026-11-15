// SPDX-License-Identifier: Apache-2.0 OR MIT
// CDTop PA1010D GNSS module (MediaTek MT3333) on its I2C port: NMEA 0183
// out and PMTK commands in, through the I2C buffer GlobalTop's "NMEA over
// I2C Software Guide" v1.0 describes.

#ifndef MCLW_DEVICE_PA1010D_H
#define MCLW_DEVICE_PA1010D_H

#include "metalclaw/model.h"

// Software guide 3.1: a packet holds at most 254 NMEA bytes and the end
// character, and reads as 255 bytes; what is not NMEA is <LF>.
#define PA1010D_PACKET_NMEA_MAX 254
#define PA1010D_PACKET_SIZE 255
#define PA1010D_GARBAGE 0x0a

// PMTK 314's sentence slots the model outputs, in the command's order.
#define PA1010D_GLL 0
#define PA1010D_RMC 1
#define PA1010D_VTG 2
#define PA1010D_GGA 3
#define PA1010D_GSA 4
#define PA1010D_GSV 5
#define PA1010D_SENTENCES 6
#define PA1010D_PMTK314_FIELDS 19

// Datasheet 2.6: 1 Hz by default, up to 10 Hz; PMTK 220 takes 100 to 10000.
#define PA1010D_RATE_MS_DEFAULT 1000
#define PA1010D_RATE_MS_MIN 100
#define PA1010D_RATE_MS_MAX 10000

// Bits of the `gnss` property and PMTK 353's first two fields.
#define PA1010D_GNSS_GPS 0x01
#define PA1010D_GNSS_GLONASS 0x02

#define PA1010D_SATELLITES_MAX 32

extern const struct mclw_model mclw_model_pa1010d;

#endif
