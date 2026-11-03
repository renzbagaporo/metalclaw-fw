// SPDX-License-Identifier: Apache-2.0 OR MIT
// The fix a model with a GNSS receiver reports, whatever its AT form. The
// time of the fix is the modem's `time`.

#ifndef MCLW_DEVICES_CELLULAR_GNSS_H
#define MCLW_DEVICES_CELLULAR_GNSS_H

#include <stdbool.h>
#include <stdint.h>

#define GNSS_LATITUDE_MAX 90000000
#define GNSS_LONGITUDE_MAX 180000000
#define GNSS_ALTITUDE_MAX 99999999
#define GNSS_SPEED_MAX 99999
#define GNSS_COURSE_MAX 3599
#define GNSS_HDOP_MAX 999
#define GNSS_SATELLITES_MAX 99
#define GNSS_DEFAULT_HDOP 10

struct gnss {
    // What the user sets: kept by reset(), restored by reset(factory).
    int32_t latitude;
    int32_t longitude;
    int32_t altitude;
    uint32_t speed;
    uint16_t course;
    uint16_t hdop;
    uint8_t satellites;
    bool fix;
    // The receiver's power, cleared by every power transition.
    bool on;
};

#endif
