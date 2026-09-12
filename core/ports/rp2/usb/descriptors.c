// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "py/mpconfig.h"
#include "pico/unique_id.h"
#include "tusb.h"

enum {
    ITF_CDC = 0,
    ITF_CDC_DATA,
    ITF_MSC,
    ITF_COUNT,
};

// 0x83/0x84 are reserved for the simulation and CMSIS-DAP interfaces that
// arrive later, so the CDC notification endpoint sits last: adding those
// interfaces must not move an address the host has already learned.
#define EP_CDC_OUT      0x01
#define EP_CDC_IN       0x81
#define EP_MSC_OUT      0x02
#define EP_MSC_IN       0x82
#define EP_CDC_NOTIFY   0x85

enum {
    STR_LANGUAGE = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_MSC,
    STR_COUNT,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = MICROPY_HW_USB_VID,
    .idProduct = MICROPY_HW_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIFY, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
};

static const char *const strings[STR_COUNT] = {
    [STR_MANUFACTURER] = MICROPY_HW_USB_MANUFACTURER_STRING,
    [STR_PRODUCT] = MICROPY_HW_USB_PRODUCT_FS_STRING,
    [STR_CDC] = MICROPY_HW_USB_CDC_INTERFACE_STRING,
    [STR_MSC] = MICROPY_HW_USB_MSC_INTERFACE_STRING,
};

#define STRING_MAX_CHARS 32

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_descriptor;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration_descriptor;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t descriptor[1 + STRING_MAX_CHARS];
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *str;
    size_t len;

    if (index == STR_LANGUAGE) {
        descriptor[1] = 0x0409;
        len = 1;
    } else {
        if (index == STR_SERIAL) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            str = serial;
        } else if (index < STR_COUNT && strings[index] != NULL) {
            str = strings[index];
        } else {
            return NULL;
        }
        len = strlen(str);
        if (len > STRING_MAX_CHARS) {
            len = STRING_MAX_CHARS;
        }
        for (size_t i = 0; i < len; i++) {
            descriptor[1 + i] = (uint8_t)str[i];
        }
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return descriptor;
}
