// SPDX-License-Identifier: Apache-2.0 OR MIT
// SMS as 3GPP TS 27.005 defines it: a store the host fills with received
// PDUs, sending in text and PDU mode, listing and deleting in PDU mode.

#ifndef MCLW_DEVICES_CELLULAR_SMS_H
#define MCLW_DEVICES_CELLULAR_SMS_H

#include <stdbool.h>
#include <stdint.h>

#include "modem.h"

#define SMS_INBOX_SIZE 384
#define SMS_OUTBOX_SIZE 192
#define SMS_SLOTS 32u

struct sms {
    // What the user sets: kept by reset(), restored by reset(factory).
    char inbox[SMS_INBOX_SIZE];
    // Observed: kept across power cycles, cleared by reset().
    uint32_t sent;
    // Inbox lines deleted with CMGD; a write to the inbox undeletes them.
    uint32_t deleted;
    char outbox[SMS_OUTBOX_SIZE];
    // Cleared by every power transition.
    uint16_t out_len;
    uint8_t mr;
    bool text;
    bool pdu_ok;
};

#define SMS_PROPERTY_COUNT 1
#define SMS_PROPERTIES MODEM_PROP_U32_RO("sms_sent")

#define SMS_REGION_COUNT 2
#define SMS_REGIONS \
    { .name = "sms_inbox", .size = SMS_INBOX_SIZE, .access = MCLW_ACCESS_RW }, \
    { .name = "sms_outbox", .size = SMS_OUTBOX_SIZE, .access = MCLW_ACCESS_RO }

extern const struct modem_part sms_part;

// +CMGF, +CSCS, +CSMP, +CNMI, +CMGS, +CMGL and +CMGD.
extern const struct modem_commands sms_commands;

#endif
