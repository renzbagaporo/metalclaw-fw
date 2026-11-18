// SPDX-License-Identifier: Apache-2.0 OR MIT
// ZhianTec ZFM-20 optical fingerprint module on a UART: the packet protocol
// and its instructions over a finger the host puts on the sensor. A finger
// is a number; an image, a character file and a template hold the number of
// the finger they came from, so two match when the numbers do. ZFM-20
// user manual V1.5 (sections IV-VI); VfyPwd (13h) and ReadIndexTable (1Fh),
// which the V1.5 manual lacks, from Grow's R503 manual V1.4.1, the same
// protocol.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

// 5.1: header EF01h, a 4-byte address, the package identifier, a 2-byte
// length (content plus checksum, at most 256), then the checksum: the sum
// of identifier, length and content. All big-endian.
#define START_HI 0xef
#define START_LO 0x01
#define HEADER_BYTES 9u
#define CONTENT_MAX 256u
#define PID_COMMAND 0x01
#define PID_ACK 0x07

// 6.4.2, and the R503 manual for 13h and 1Fh.
#define CMD_GEN_IMG 0x01
#define CMD_IMG_2_TZ 0x02
#define CMD_MATCH 0x03
#define CMD_SEARCH 0x04
#define CMD_REG_MODEL 0x05
#define CMD_STORE 0x06
#define CMD_LOAD_CHAR 0x07
#define CMD_DELET_CHAR 0x0c
#define CMD_EMPTY 0x0d
#define CMD_SET_SYS_PARA 0x0e
#define CMD_READ_SYS_PARA 0x0f
#define CMD_VFY_PWD 0x13
#define CMD_GET_RANDOM 0x14
#define CMD_HANDSHAKE 0x17
#define CMD_TEMPLETE_NUM 0x1d
#define CMD_READ_INDEX 0x1f

// 5.2's confirmation codes.
#define OK 0x00
#define ERR_RECEIVE 0x01
#define ERR_NO_FINGER 0x02
#define ERR_NO_MATCH 0x08
#define ERR_NOT_FOUND 0x09
#define ERR_MERGE 0x0a
#define ERR_BAD_PAGE 0x0b
#define ERR_BAD_TEMPLATE 0x0c
#define ERR_DELETE 0x10
#define ERR_NO_IMAGE 0x15
#define ERR_BAD_REGISTER 0x1a
#define ERR_PASSWORD 0x13

// 1.2: the ZFM-20's library holds 120 templates. 4.4: baud 9600 * N, N 6
// (57600) from the factory; security level 1-5; data package size code
// 0-3 (32-256 bytes). 6.1.4: system identifier code 0009h.
#define LIBRARY 120u
#define FACTORY_BAUD_N 6u
#define FACTORY_SECURITY 3u
#define FACTORY_PACKAGE_SIZE 2u
#define SYSTEM_ID 0x0009u
#define STATUS_PASS 0x0002u
#define STATUS_PWD 0x0004u
#define STATUS_IMG_BUF 0x0008u

enum {
    PROP_FINGER,
    PROP_SCORE,
    PROP_TEMPLATES,
    PROP_COMMANDS,
};

enum {
    BINDING_UART,
};

struct zfm20_state {
    struct mclw_uart *uart;
    uint8_t rx[HEADER_BYTES + CONTENT_MAX];
    size_t rx_len;
    // What the host has on the sensor (0: nothing), and the score a match
    // reports.
    uint32_t finger;
    uint32_t score;
    // Volatile: the image buffer and CharBuffer1-2 (4.2), each the finger
    // it came from, 0 for none.
    uint32_t image;
    uint32_t chars[2];
    bool pass;
    bool password_ok;
    // Kept across power (4.3, 4.4, 4.6).
    uint32_t library[LIBRARY];
    uint32_t address;
    uint32_t password;
    uint8_t baud_n;
    uint8_t security;
    uint8_t package_size;
    uint32_t random;
    uint32_t commands;
};

static const struct mclw_property_desc zfm20_properties[] = {
    { .name = "finger", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "score", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = UINT16_MAX },
    { .name = "templates", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = LIBRARY },
    { .name = "commands", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc zfm20_bindings[] = {
    {
        .name = "uart",
        .kind = MCLW_BINDING_UART,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = {
            .default_baud = 9600u * FACTORY_BAUD_N,
            .format = { .data_bits = 8, .stop_bits = 1, .parity = MCLW_UART_PARITY_NONE },
        },
    },
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}

static uint32_t count_templates(const struct zfm20_state *s) {
    uint32_t n = 0;

    for (size_t i = 0; i < LIBRARY; i++) {
        n += s->library[i] != 0;
    }
    return n;
}

// An acknowledge package: the confirmation code, then `len` parameter bytes.
static void ack(struct zfm20_state *s, uint8_t code, const uint8_t *params, size_t len) {
    uint8_t out[HEADER_BYTES + 1 + 32 + 2];
    uint16_t length = (uint16_t)(1 + len + 2);
    uint16_t sum = PID_ACK + (length >> 8) + (length & 0xff) + code;

    out[0] = START_HI;
    out[1] = START_LO;
    put_be32(&out[2], s->address);
    out[6] = PID_ACK;
    put_be16(&out[7], length);
    out[9] = code;
    for (size_t i = 0; i < len; i++) {
        out[10 + i] = params[i];
        sum = (uint16_t)(sum + params[i]);
    }
    put_be16(&out[10 + len], sum);
    (void)mclw_hal_uart_send(s->uart, out, HEADER_BYTES + length);
}

static void ack_code(struct zfm20_state *s, uint8_t code) {
    ack(s, code, NULL, 0);
}

static uint32_t *buffer(struct zfm20_state *s, uint8_t id) {
    // 6.2: BufferID 1 is CharBuffer1; any other value is CharBuffer2.
    return &s->chars[id == 1 ? 0 : 1];
}

static void read_sys_para(struct zfm20_state *s) {
    uint8_t p[16];
    uint16_t status = (uint16_t)((s->pass ? STATUS_PASS : 0) | (s->password_ok ? STATUS_PWD : 0)
        | (s->image != 0 ? STATUS_IMG_BUF : 0));

    put_be16(&p[0], status);
    put_be16(&p[2], SYSTEM_ID);
    put_be16(&p[4], LIBRARY);
    put_be16(&p[6], s->security);
    put_be32(&p[8], s->address);
    put_be16(&p[12], s->package_size);
    put_be16(&p[14], s->baud_n);
    ack(s, OK, p, sizeof(p));
}

// 4.4: the module answers at the old setting, then changes.
static void set_sys_para(struct zfm20_state *s, uint8_t reg, uint8_t value) {
    switch (reg) {
        case 4:
            if (value < 1 || value > 12) {
                break;
            }
            ack_code(s, OK);
            s->baud_n = value;
            (void)mclw_hal_uart_set_baud(s->uart, 9600u * value);
            return;
        case 5:
            if (value < 1 || value > 5) {
                break;
            }
            s->security = value;
            ack_code(s, OK);
            return;
        case 6:
            if (value > 3) {
                break;
            }
            s->package_size = value;
            ack_code(s, OK);
            return;
        default:
            ack_code(s, ERR_BAD_REGISTER);
            return;
    }
    ack_code(s, 0x1b);
}

static void search(struct zfm20_state *s, const uint8_t *c) {
    uint32_t want = *buffer(s, c[1]);
    uint32_t start = be16(&c[2]);
    uint32_t end = start + be16(&c[4]);
    uint8_t p[4] = { 0 };

    if (end > LIBRARY) {
        end = LIBRARY;
    }
    s->pass = false;
    for (uint32_t page = start; want != 0 && page < end; page++) {
        if (s->library[page] == want) {
            s->pass = true;
            put_be16(&p[0], (uint16_t)page);
            put_be16(&p[2], (uint16_t)s->score);
            ack(s, OK, p, sizeof(p));
            return;
        }
    }
    ack(s, ERR_NOT_FOUND, p, sizeof(p));
}

static void index_table(struct zfm20_state *s, uint8_t page) {
    uint8_t t[32] = { 0 };

    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t n = (uint32_t)page * 256u + i;

        if (n < LIBRARY && s->library[n] != 0) {
            t[i / 8u] |= (uint8_t)(1u << (i % 8u));
        }
    }
    ack(s, OK, t, sizeof(t));
}

// One command package's content: the instruction code and its parameters.
static void command(struct zfm20_state *s, const uint8_t *c, size_t len) {
    uint8_t p[4];

    s->commands++;
    switch (c[0]) {
        case CMD_HANDSHAKE:
            ack_code(s, OK);
            return;
        case CMD_VFY_PWD:
            if (len < 5) {
                break;
            }
            s->password_ok = ((uint32_t)be16(&c[1]) << 16 | be16(&c[3])) == s->password;
            ack_code(s, s->password_ok ? OK : ERR_PASSWORD);
            return;
        case CMD_READ_SYS_PARA:
            read_sys_para(s);
            return;
        case CMD_SET_SYS_PARA:
            if (len < 3) {
                break;
            }
            set_sys_para(s, c[1], c[2]);
            return;
        case CMD_TEMPLETE_NUM:
            put_be16(p, (uint16_t)count_templates(s));
            ack(s, OK, p, 2);
            return;
        case CMD_READ_INDEX:
            if (len < 2) {
                break;
            }
            index_table(s, c[1]);
            return;
        case CMD_GET_RANDOM:
            s->random = s->random * 1664525u + 1013904223u;
            put_be32(p, s->random);
            ack(s, OK, p, 4);
            return;
        case CMD_GEN_IMG:
            if (s->finger == 0) {
                ack_code(s, ERR_NO_FINGER);
                return;
            }
            s->image = s->finger;
            ack_code(s, OK);
            return;
        case CMD_IMG_2_TZ:
            if (len < 2) {
                break;
            }
            if (s->image == 0) {
                ack_code(s, ERR_NO_IMAGE);
                return;
            }
            *buffer(s, c[1]) = s->image;
            ack_code(s, OK);
            return;
        case CMD_REG_MODEL:
            // The two character files combine into one template, left in
            // both buffers, when they came from one finger.
            if (s->chars[0] == 0 || s->chars[0] != s->chars[1]) {
                ack_code(s, ERR_MERGE);
                return;
            }
            ack_code(s, OK);
            return;
        case CMD_MATCH:
            s->pass = s->chars[0] != 0 && s->chars[0] == s->chars[1];
            put_be16(p, s->pass ? (uint16_t)s->score : 0);
            ack(s, s->pass ? OK : ERR_NO_MATCH, p, 2);
            return;
        case CMD_SEARCH:
            if (len < 6) {
                break;
            }
            search(s, c);
            return;
        case CMD_STORE:
        case CMD_LOAD_CHAR: {
            if (len < 4) {
                break;
            }
            uint16_t page = be16(&c[2]);
            uint32_t *buf = buffer(s, c[1]);

            if (page >= LIBRARY) {
                ack_code(s, ERR_BAD_PAGE);
                return;
            }
            if (c[0] == CMD_STORE) {
                s->library[page] = *buf;
            } else if (s->library[page] == 0) {
                ack_code(s, ERR_BAD_TEMPLATE);
                return;
            } else {
                *buf = s->library[page];
            }
            ack_code(s, OK);
            return;
        }
        case CMD_DELET_CHAR: {
            if (len < 5) {
                break;
            }
            uint32_t page = be16(&c[1]);
            uint32_t n = be16(&c[3]);

            if (page + n > LIBRARY) {
                ack_code(s, ERR_DELETE);
                return;
            }
            memset(&s->library[page], 0, n * sizeof(s->library[0]));
            ack_code(s, OK);
            return;
        }
        case CMD_EMPTY:
            memset(s->library, 0, sizeof(s->library));
            ack_code(s, OK);
            return;
        default:
            break;
    }
    ack_code(s, ERR_RECEIVE);
}

// Packages addressed elsewhere, and anything but a command package, get no
// answer; a bad checksum gets 01h.
static void package(struct zfm20_state *s) {
    uint16_t length = be16(&s->rx[7]);
    uint16_t sum = (uint16_t)(s->rx[6] + s->rx[7] + s->rx[8]);
    size_t content = (size_t)length - 2u;

    if (((uint32_t)be16(&s->rx[2]) << 16 | be16(&s->rx[4])) != s->address || s->rx[6] != PID_COMMAND) {
        return;
    }
    for (size_t i = 0; i < content; i++) {
        sum = (uint16_t)(sum + s->rx[HEADER_BYTES + i]);
    }
    if (content == 0 || sum != be16(&s->rx[HEADER_BYTES + content])) {
        ack_code(s, ERR_RECEIVE);
        return;
    }
    command(s, &s->rx[HEADER_BYTES], content);
}

static void rx_byte(struct zfm20_state *s, uint8_t b) {
    if ((s->rx_len == 0 && b != START_HI) || (s->rx_len == 1 && b != START_LO)) {
        s->rx_len = b == START_HI ? 1 : 0;
        s->rx[0] = b;
        return;
    }
    s->rx[s->rx_len++] = b;
    if (s->rx_len == HEADER_BYTES) {
        uint16_t length = be16(&s->rx[7]);

        if (length < 2 || length > CONTENT_MAX) {
            s->rx_len = 0;
        }
        return;
    }
    if (s->rx_len > HEADER_BYTES && s->rx_len == HEADER_BYTES + be16(&s->rx[7])) {
        package(s);
        s->rx_len = 0;
    }
}

static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct zfm20_state *s = ctx;

    (void)uart;
    for (size_t i = 0; i < len; i++) {
        rx_byte(s, bytes[i]);
    }
}

static void power_up(struct zfm20_state *s) {
    s->rx_len = 0;
    s->image = 0;
    s->chars[0] = 0;
    s->chars[1] = 0;
    s->pass = false;
    s->password_ok = false;
}

static void factory(struct zfm20_state *s) {
    memset(s->library, 0, sizeof(s->library));
    s->address = 0xffffffffu;
    s->password = 0;
    s->baud_n = FACTORY_BAUD_N;
    s->security = FACTORY_SECURITY;
    s->package_size = FACTORY_PACKAGE_SIZE;
}

static int zfm20_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct zfm20_state *s = inst->state;

    s->uart = bindings[BINDING_UART].uart;
    s->finger = 0;
    s->score = 100;
    s->random = 0x2545f491u;
    s->commands = 0;
    factory(s);
    power_up(s);
    mclw_hal_uart_set_rx(s->uart, on_rx, s);
    return 0;
}

static void zfm20_deinit(struct mclw_instance *inst) {
    struct zfm20_state *s = inst->state;

    mclw_hal_uart_set_rx(s->uart, NULL, NULL);
    s->uart = NULL;
}

// A power cycle keeps the library and the settings in flash and comes up at
// the saved baud; a factory reset clears them.
static int zfm20_reset(struct mclw_instance *inst, bool factory_reset) {
    struct zfm20_state *s = inst->state;

    if (factory_reset) {
        factory(s);
    }
    power_up(s);
    return mclw_hal_uart_set_baud(s->uart, 9600u * s->baud_n);
}

static int zfm20_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct zfm20_state *s = inst->state;

    switch (property) {
        case PROP_FINGER:
            out->v[0] = s->finger;
            return 0;
        case PROP_SCORE:
            out->v[0] = s->score;
            return 0;
        case PROP_TEMPLATES:
            out->v[0] = count_templates(s);
            return 0;
        case PROP_COMMANDS:
            out->v[0] = s->commands;
            return 0;
        default:
            return -EINVAL;
    }
}

static int zfm20_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct zfm20_state *s = inst->state;

    switch (property) {
        case PROP_FINGER:
            s->finger = (uint32_t)in->v[0];
            return 0;
        case PROP_SCORE:
            s->score = (uint32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops zfm20_ops = {
    .init = zfm20_init,
    .deinit = zfm20_deinit,
    .reset = zfm20_reset,
    .get = zfm20_get,
    .set = zfm20_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_zfm20 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "zfm20",
    .family = "zfm20",
    .config = NULL,
    .properties = zfm20_properties,
    .property_count = sizeof(zfm20_properties) / sizeof(zfm20_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = zfm20_bindings,
    .binding_count = sizeof(zfm20_bindings) / sizeof(zfm20_bindings[0]),
    .state_size = sizeof(struct zfm20_state),
    .ops = &zfm20_ops,
};
