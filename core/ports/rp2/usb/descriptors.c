// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "py/mpconfig.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "metalclaw/hal/usb.h"
#include "usb_internal.h"

enum {
    ITF_CDC = 0,
    ITF_CDC_DATA,
    ITF_MSC,
    ITF_VENDOR_FIRST,
};

// The CDC notification endpoint sits above every bulk pair so that vendor
// interfaces coming and going never move it.
#define EP_CDC_OUT      0x01
#define EP_CDC_IN       0x81
#define EP_MSC_OUT      0x02
#define EP_MSC_IN       0x82
#define EP_VENDOR_FIRST 0x03
#define EP_CDC_NOTIFY   (0x80 | (EP_VENDOR_FIRST + CFG_TUD_VENDOR))

enum {
    STR_LANGUAGE = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_MSC,
    STR_VENDOR_FIRST,
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

#define CONFIG_MAX_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + CFG_TUD_VENDOR * TUD_VENDOR_DESC_LEN)

static uint8_t configuration_descriptor[CONFIG_MAX_LEN];
static const char *vendor_names[CFG_TUD_VENDOR];
static size_t vendor_count;
static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static const char *const strings[STR_VENDOR_FIRST] = {
    [STR_MANUFACTURER] = MICROPY_HW_USB_MANUFACTURER_STRING,
    [STR_PRODUCT] = MICROPY_HW_USB_PRODUCT_FS_STRING,
    [STR_CDC] = MICROPY_HW_USB_CDC_INTERFACE_STRING,
    [STR_MSC] = MICROPY_HW_USB_MSC_INTERFACE_STRING,
};

#define STRING_MAX_CHARS 32

bool metalclaw_usb_descriptors_rebuild(void) {
    const char *names[CFG_TUD_VENDOR];
    size_t count = metalclaw_usb_vendor_layout(names, CFG_TUD_VENDOR);
    uint8_t itf_count = (uint8_t)(ITF_VENDOR_FIRST + count);
    uint16_t total = (uint16_t)(TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + count * TUD_VENDOR_DESC_LEN);
    uint8_t staged[CONFIG_MAX_LEN];

    const uint8_t head[] = {
        TUD_CONFIG_DESCRIPTOR(1, itf_count, 0, total, 0, 100),
        TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIFY, 8, EP_CDC_OUT, EP_CDC_IN, 64),
        TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
    };
    memset(staged, 0, sizeof(staged));
    memcpy(staged, head, sizeof(head));
    size_t at = sizeof(head);
    for (size_t i = 0; i < count; i++) {
        const uint8_t vendor[] = {
            TUD_VENDOR_DESCRIPTOR((uint8_t)(ITF_VENDOR_FIRST + i), (uint8_t)(STR_VENDOR_FIRST + i),
                (uint8_t)(EP_VENDOR_FIRST + i), (uint8_t)(0x80 | (EP_VENDOR_FIRST + i)), 64),
        };
        memcpy(&staged[at], vendor, sizeof(vendor));
        at += sizeof(vendor);
    }

    bool changed = count != vendor_count || memcmp(staged, configuration_descriptor, sizeof(staged)) != 0;
    for (size_t i = 0; i < count && !changed; i++) {
        changed = strcmp(names[i], vendor_names[i]) != 0;
    }
    memcpy(configuration_descriptor, staged, sizeof(staged));
    memcpy(vendor_names, names, count * sizeof(names[0]));
    vendor_count = count;
    return changed;
}

const char *metalclaw_hal_usb_serial(void) {
    if (serial[0] == '\0') {
        pico_get_unique_board_id_string(serial, sizeof(serial));
    }
    return serial;
}

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
    const char *str;
    size_t len;

    if (index == STR_LANGUAGE) {
        descriptor[1] = 0x0409;
        len = 1;
    } else {
        if (index == STR_SERIAL) {
            str = metalclaw_hal_usb_serial();
        } else if (index < STR_VENDOR_FIRST) {
            str = strings[index];
        } else if ((size_t)(index - STR_VENDOR_FIRST) < vendor_count) {
            str = vendor_names[index - STR_VENDOR_FIRST];
        } else {
            return NULL;
        }
        if (str == NULL) {
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
