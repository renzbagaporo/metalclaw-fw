// SPDX-License-Identifier: Apache-2.0 OR MIT
// Microchip MCP2515 stand-alone CAN controller on SPI, served as a command
// image: READ (03h) is the image's short register read, READ RX BUFFER,
// READ STATUS and RX STATUS its immediate responses, and every other
// instruction reaches the model after chip select rises. The CAN side is
// the controller's own loopback: a frame sent in Loopback mode is received
// through the masks and filters, and one sent in Normal mode finds no node
// to acknowledge it. Section, table and register numbers are the
// datasheet's, DS20001801J.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/command.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "mcp2515.h"

#define REGISTERS 0x80u
#define TX_BUFFERS 3u
// A buffer's identifier, DLC and data: SIDH .. D7 (sections 3.1, 4.1).
#define FRAME_BYTES 13u
// 9.1: a transmit-error count of 96 warns, 128 is error-passive, and an
// unacknowledged frame stops counting there.
#define TEC_WARNING 96u
#define TEC_PASSIVE 128u
#define TEC_ACK_STEP 8u

enum {
    PROP_MODE,
    PROP_TEC,
    PROP_SENT,
    PROP_RECEIVED,
    PROP_OVERFLOWS,
    PROP_IGNORED,
    PROP_SPI_MODE,
};

enum {
    BINDING_SPI,
    BINDING_INT,
};

struct mcp2515_state {
    struct mclw_command_image image;
    struct mclw_spi_target *spi;
    struct mclw_gpio *int_pin;
    uint32_t sent;
    uint32_t received;
    uint32_t overflows;
    uint32_t ignored;
};

static const struct mclw_property_desc mcp2515_properties[] = {
    { .name = "mode", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 7 },
    { .name = "tec", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "sent", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "received", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "overflows", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "ignored", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    MCLW_PROPERTY_SPI_MODE,
};

static const struct mclw_region_desc mcp2515_regions[] = {
    { .name = "registers", .size = REGISTERS, .access = MCLW_ACCESS_RO },
};

// 12.0: SPI modes 0,0 and 1,1, MSB first.
static const struct mclw_binding_desc mcp2515_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
    {
        .name = "int",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(struct mcp2515_state *s, uint8_t reg) {
    return mclw_command_get_register(&s->image, reg);
}

static void put(struct mcp2515_state *s, uint8_t reg, uint8_t value) {
    *mclw_command_register(&s->image, reg) = value;
}

static uint8_t mode(struct mcp2515_state *s) {
    return (uint8_t)(get(s, MCP2515_CANSTAT) >> MCP2515_CANCTRL_REQOP_SHIFT);
}

// CANCTRL and CANSTAT read alike at every xFh and xEh (table 11-1).
static void MCLW_RAM_FUNC(page_put_mirrored)(uint8_t *page, uint8_t reg, uint8_t value) {
    for (uint8_t row = 0; row < REGISTERS; row += 0x10u) {
        page[(uint8_t)(row | (reg & 0x0fu))] = value;
    }
}

static void put_mirrored(struct mcp2515_state *s, uint8_t reg, uint8_t value) {
    page_put_mirrored(mclw_command_register(&s->image, 0), reg, value);
}

// CANSTAT's ICOD (register 10-2): the highest enabled pending interrupt,
// error first, then wake, TXB0..TXB2, RXB0, RXB1.
static uint8_t interrupt_code(uint8_t pending) {
    static const struct {
        uint8_t flag;
        uint8_t code;
    } order[] = { { 0x20, 1 }, { 0x40, 2 }, { 0x04, 3 }, { 0x08, 4 }, { 0x10, 5 }, { 0x01, 6 }, { 0x02, 7 } };

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if ((pending & order[i].flag) != 0) {
            return order[i].code;
        }
    }
    return 0;
}

// READ STATUS (figure 12-8): RX0IF, RX1IF, then TXREQ and TXnIF for each
// transmit buffer. RX STATUS (figure 12-9): the buffers holding a message,
// the type of RXB0's (or RXB1's) message and the filter that took it.
static uint8_t read_status(struct mcp2515_state *s) {
    uint8_t intf = get(s, MCP2515_CANINTF);
    uint8_t v = intf & (MCP2515_INT_RX0 | MCP2515_INT_RX1);

    for (uint8_t n = 0; n < TX_BUFFERS; n++) {
        if ((get(s, (uint8_t)(MCP2515_TXB0CTRL + 0x10u * n)) & MCP2515_TXB_TXREQ) != 0) {
            v |= (uint8_t)(0x04u << (2u * n));
        }
        if ((intf & (MCP2515_INT_TX0 << n)) != 0) {
            v |= (uint8_t)(0x08u << (2u * n));
        }
    }
    return v;
}

static uint8_t rx_status(struct mcp2515_state *s) {
    uint8_t intf = get(s, MCP2515_CANINTF);
    uint8_t buffer = (intf & MCP2515_INT_RX0) != 0 ? MCP2515_RXB0CTRL : MCP2515_RXB1CTRL;
    uint8_t v = (uint8_t)((intf & (MCP2515_INT_RX0 | MCP2515_INT_RX1)) << 6);

    if ((v & 0xc0u) != 0) {
        bool extended = (get(s, (uint8_t)(buffer + 2u)) & MCP2515_SIDL_IDE) != 0;
        bool remote = (get(s, buffer) & MCP2515_RXB_RXRTR) != 0;

        v |= (uint8_t)((extended ? 0x10u : 0u) | (remote ? 0x08u : 0u));
        if (buffer == MCP2515_RXB0CTRL) {
            v |= (uint8_t)(get(s, buffer) & 0x01u);
        } else {
            uint8_t hit = get(s, buffer) & 0x07u;
            // A message rolled over from RXB0 reports RXF0 or RXF1 as 6 or 7.
            v |= hit <= 1u ? (uint8_t)(hit + 6u) : hit;
        }
    }
    return v;
}

// Everything served from hardware follows the registers: CANSTAT's ICOD,
// the RX buffer reads (sixteen bytes from their start address, as a READ
// from there would continue), the two status reads (their byte repeated,
// figures 12-8 and 12-9), and INT, low while an enabled flag is set (7.0).
static void publish(struct mcp2515_state *s) {
    static const struct {
        uint8_t opcode;
        uint8_t start;
    } rx_reads[] = { { 0x90, 0x61 }, { 0x92, 0x66 }, { 0x94, 0x71 }, { 0x96, 0x76 } };
    uint8_t pending = get(s, MCP2515_CANINTF) & get(s, MCP2515_CANINTE);
    uint8_t bytes[MCLW_COMMAND_RESPONSE_MAX];

    put_mirrored(s, MCP2515_CANSTAT,
        (uint8_t)((get(s, MCP2515_CANSTAT) & 0xe0u) | (uint8_t)(interrupt_code(pending) << 1)));
    for (size_t i = 0; i < sizeof(rx_reads) / sizeof(rx_reads[0]); i++) {
        for (uint8_t b = 0; b < sizeof(bytes); b++) {
            uint8_t reg = (uint8_t)(rx_reads[i].start + b);
            bytes[b] = reg < REGISTERS ? get(s, reg) : 0;
        }
        (void)mclw_command_set_response(&s->image, rx_reads[i].opcode, bytes, sizeof(bytes));
    }
    memset(bytes, read_status(s), sizeof(bytes));
    (void)mclw_command_set_response(&s->image, MCP2515_READ_STATUS, bytes, sizeof(bytes));
    memset(bytes, rx_status(s), sizeof(bytes));
    (void)mclw_command_set_response(&s->image, MCP2515_RX_STATUS, bytes, sizeof(bytes));
    mclw_command_publish(&s->image);
    if (s->int_pin != NULL) {
        mclw_hal_gpio_write(s->int_pin, pending == 0);
    }
}

// The reset values (table 11-2): every register zero but CANCTRL (87h,
// Configuration mode) and CANSTAT (80h).
static void reset_registers(struct mcp2515_state *s) {
    for (uint8_t reg = 0; reg < REGISTERS; reg++) {
        put(s, reg, 0);
    }
    put_mirrored(s, MCP2515_CANCTRL, MCP2515_CANCTRL_RESET);
    put_mirrored(s, MCP2515_CANSTAT, (uint8_t)(MCP2515_MODE_CONFIG << MCP2515_CANCTRL_REQOP_SHIFT));
}

// 10.0: the mode REQOP requests is the one CANSTAT reports; ABAT ends every
// pending transmission (3.4), marking it aborted.
static void MCLW_RAM_FUNC(page_control_written)(uint8_t *page, uint8_t value) {
    uint8_t requested = (uint8_t)(value >> MCP2515_CANCTRL_REQOP_SHIFT);

    page_put_mirrored(page, MCP2515_CANCTRL, value);
    if (requested <= MCP2515_MODE_CONFIG) {
        page_put_mirrored(page, MCP2515_CANSTAT,
            (uint8_t)((page[MCP2515_CANSTAT] & 0x1fu) | (uint8_t)(requested << MCP2515_CANCTRL_REQOP_SHIFT)));
    }
    if ((value & MCP2515_CANCTRL_ABAT) != 0) {
        for (uint8_t n = 0; n < TX_BUFFERS; n++) {
            uint8_t ctrl = (uint8_t)(MCP2515_TXB0CTRL + 0x10u * n);

            if ((page[ctrl] & MCP2515_TXB_TXREQ) != 0) {
                page[ctrl] = (uint8_t)((page[ctrl] & ~MCP2515_TXB_TXREQ) | MCP2515_TXB_ABTF);
            }
        }
    }
}

// What the bus may write (table 11-1 and the registers' R/W bits): the
// status, counters and received messages are the controller's, and the bit
// timing, filters and masks take writes only in Configuration mode (5.0,
// 4.5).
static void MCLW_RAM_FUNC(page_write_register)(uint8_t *page, uint8_t reg, uint8_t value) {
    uint8_t low = reg & 0x0fu;
    bool config = (page[MCP2515_CANSTAT] >> MCP2515_CANCTRL_REQOP_SHIFT) == MCP2515_MODE_CONFIG;

    if (reg >= REGISTERS || low == 0x0eu || reg == MCP2515_TEC || reg == MCP2515_REC) {
        return;
    }
    if (low == 0x0fu) {
        page_control_written(page, value);
        return;
    }
    if (reg < 0x0cu || (reg >= 0x10u && reg < 0x1cu) || (reg >= MCP2515_RXM0SIDH && reg <= MCP2515_CNF1)) {
        if (config) {
            page[reg] = value;
        }
        return;
    }
    if (reg == MCP2515_EFLG) {
        page[reg] = (uint8_t)((page[reg] & 0x3fu) | (value & 0xc0u));
        return;
    }
    if (reg >= MCP2515_TXB0CTRL && reg < MCP2515_RXB0CTRL && low == 0) {
        page[reg] = (uint8_t)((page[reg] & 0x70u) | (value & 0x0bu));
        return;
    }
    if (reg == MCP2515_RXB0CTRL) {
        page[reg] = (uint8_t)((page[reg] & 0x0bu) | (value & 0x64u) | ((value & MCP2515_RXB0_BUKT) >> 1));
        return;
    }
    if (reg == MCP2515_RXB1CTRL) {
        page[reg] = (uint8_t)((page[reg] & 0x0fu) | (value & 0x60u));
        return;
    }
    if ((reg > MCP2515_RXB0CTRL && reg <= 0x6du) || (reg > MCP2515_RXB1CTRL && reg <= 0x7du)) {
        return;
    }
    page[reg] = value;
}

static void write_register(struct mcp2515_state *s, uint8_t reg, uint8_t value) {
    page_write_register(mclw_command_register(&s->image, 0), reg, value);
}

// BIT MODIFY works on these alone (table 11-1, shaded); any other register
// takes the data as if the mask were FFh (12.10).
static bool MCLW_RAM_FUNC(bit_modifiable)(uint8_t reg) {
    uint8_t low = reg & 0x0fu;

    return reg == MCP2515_BFPCTRL || reg == MCP2515_TXRTSCTRL || low == 0x0fu ||
        (reg >= MCP2515_CNF3 && reg <= MCP2515_EFLG) || (reg >= MCP2515_TXB0CTRL && reg < MCP2515_RXB0CTRL && low == 0) ||
        reg == MCP2515_RXB0CTRL || reg == MCP2515_RXB1CTRL;
}

struct frame {
    uint32_t id;
    bool extended;
    bool remote;
    // SIDH .. D7 as a buffer holds them.
    uint8_t raw[FRAME_BYTES];
};

static struct frame frame_of(struct mcp2515_state *s, uint8_t base) {
    struct frame f;

    for (uint8_t i = 0; i < FRAME_BYTES; i++) {
        f.raw[i] = get(s, (uint8_t)(base + i));
    }
    f.extended = (f.raw[1] & MCP2515_SIDL_IDE) != 0;
    f.id = ((uint32_t)f.raw[0] << 3) | (f.raw[1] >> 5);
    if (f.extended) {
        f.id = (f.id << 18) | ((uint32_t)(f.raw[1] & 0x03u) << 16) | ((uint32_t)f.raw[2] << 8) | f.raw[3];
    }
    f.remote = (f.raw[4] & MCP2515_DLC_RTR) != 0;
    return f;
}

// An identifier in a filter's or mask's four registers (registers 4-4 to
// 4-11), the same layout as a buffer's.
static uint32_t id_of(struct mcp2515_state *s, uint8_t base, bool extended) {
    uint32_t id = ((uint32_t)get(s, base) << 3) | (get(s, (uint8_t)(base + 1u)) >> 5);

    if (extended) {
        id = (id << 18) | ((uint32_t)(get(s, (uint8_t)(base + 1u)) & 0x03u) << 16) |
            ((uint32_t)get(s, (uint8_t)(base + 2u)) << 8) | get(s, (uint8_t)(base + 3u));
    }
    return id;
}

// 4.5: a filter takes a message of its EXIDE type whose identifier matches
// it in every bit the mask sets. (The mask and filter bits a standard
// frame applies to its first two data bytes are not modelled.)
static bool filter_takes(struct mcp2515_state *s, uint8_t filter, uint8_t mask, const struct frame *f) {
    static const uint8_t filters[] = { 0x00, 0x04, 0x08, 0x10, 0x14, 0x18 };
    uint8_t base = filters[filter];
    bool exide = (get(s, (uint8_t)(base + 1u)) & MCP2515_SIDL_IDE) != 0;
    uint32_t m = id_of(s, mask, f->extended);

    return exide == f->extended && ((id_of(s, base, f->extended) ^ f->id) & m) == 0;
}

// The message into RXBn (4.1): its identifier, DLC and data, RXRTR, the
// filter hit, and RXnIF.
static void store(struct mcp2515_state *s, uint8_t n, const struct frame *f, uint8_t hit) {
    uint8_t ctrl = n == 0 ? MCP2515_RXB0CTRL : MCP2515_RXB1CTRL;
    uint8_t keep = n == 0 ? 0x66u : 0x60u;

    for (uint8_t i = 0; i < FRAME_BYTES; i++) {
        put(s, (uint8_t)(ctrl + 1u + i), f->raw[i]);
    }
    // A standard remote frame shows as SRR in SIDL (register 4-5).
    if (f->remote && !f->extended) {
        put(s, (uint8_t)(ctrl + 2u), (uint8_t)(f->raw[1] | MCP2515_SIDL_SRR));
    }
    put(s, ctrl, (uint8_t)((get(s, ctrl) & keep) | (f->remote ? MCP2515_RXB_RXRTR : 0u) | hit));
    put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) | (n == 0 ? MCP2515_INT_RX0 : MCP2515_INT_RX1)));
    s->received++;
}

// 4.2 and 4.5, figure 4-2: RXB0 takes what RXF0 or RXF1 passes under RXM0,
// or anything with RXM 11; a message for a full RXB0 rolls over into RXB1
// with BUKT set. RXB1 takes what RXF2..RXF5 pass under RXM1. A message for
// a full buffer is lost and flags RXnOVR (6.0).
static void receive(struct mcp2515_state *s, const struct frame *f) {
    uint8_t ctrl0 = get(s, MCP2515_RXB0CTRL);
    uint8_t ctrl1 = get(s, MCP2515_RXB1CTRL);
    uint8_t intf = get(s, MCP2515_CANINTF);
    int hit0 = -1;
    int hit1 = -1;

    if ((ctrl0 & MCP2515_RXB_RXM_ANY) == MCP2515_RXB_RXM_ANY) {
        hit0 = 0;
    } else {
        for (uint8_t filter = 0; filter < 2u && hit0 < 0; filter++) {
            if (filter_takes(s, filter, MCP2515_RXM0SIDH, f)) {
                hit0 = filter;
            }
        }
    }
    if (hit0 >= 0) {
        if ((intf & MCP2515_INT_RX0) == 0) {
            store(s, 0, f, (uint8_t)hit0);
        } else if ((ctrl0 & MCP2515_RXB0_BUKT) != 0 && (intf & MCP2515_INT_RX1) == 0) {
            store(s, 1, f, (uint8_t)hit0);
        } else {
            put(s, MCP2515_EFLG, (uint8_t)(get(s, MCP2515_EFLG) | 0x40u));
            put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) | MCP2515_INT_ERR));
            s->overflows++;
        }
        return;
    }
    if ((ctrl1 & MCP2515_RXB_RXM_ANY) == MCP2515_RXB_RXM_ANY) {
        hit1 = 2;
    } else {
        for (uint8_t filter = 2; filter < 6u && hit1 < 0; filter++) {
            if (filter_takes(s, filter, MCP2515_RXM1SIDH, f)) {
                hit1 = filter;
            }
        }
    }
    if (hit1 < 0) {
        return;
    }
    if ((intf & MCP2515_INT_RX1) == 0) {
        store(s, 1, f, (uint8_t)hit1);
    } else {
        put(s, MCP2515_EFLG, (uint8_t)(get(s, MCP2515_EFLG) | 0x80u));
        put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) | MCP2515_INT_ERR));
        s->overflows++;
    }
}

// 3.2: of the buffers requesting, the highest TXP goes first, the higher
// buffer number among equals. In Loopback mode the controller receives its
// own message (10.4) and the transmission completes; in Normal mode no
// other node acknowledges it, so TXERR rises and the transmit-error count
// climbs to error-passive, where it stays, the message pending (one-shot
// mode aborts it instead). Listen-only, Sleep and Configuration send
// nothing. Whether anything changed.
static bool transmit_one(struct mcp2515_state *s) {
    int best = -1;
    uint8_t best_prio = 0;
    uint8_t m = mode(s);

    if (m != MCP2515_MODE_NORMAL && m != MCP2515_MODE_LOOPBACK) {
        return false;
    }
    for (int n = (int)TX_BUFFERS - 1; n >= 0; n--) {
        uint8_t ctrl = get(s, (uint8_t)(MCP2515_TXB0CTRL + 0x10u * (uint8_t)n));

        if ((ctrl & MCP2515_TXB_TXREQ) != 0 && (best < 0 || (ctrl & 0x03u) > best_prio)) {
            best = n;
            best_prio = ctrl & 0x03u;
        }
    }
    if (best < 0) {
        return false;
    }
    uint8_t ctrl = (uint8_t)(MCP2515_TXB0CTRL + 0x10u * (uint8_t)best);
    if (m == MCP2515_MODE_LOOPBACK) {
        struct frame f = frame_of(s, (uint8_t)(ctrl + 1u));

        put(s, ctrl, (uint8_t)(get(s, ctrl) & ~(MCP2515_TXB_TXREQ | MCP2515_TXB_TXERR | MCP2515_TXB_ABTF)));
        put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) | (MCP2515_INT_TX0 << best)));
        s->sent++;
        receive(s, &f);
        return true;
    }
    uint8_t tec = get(s, MCP2515_TEC);
    uint8_t eflg = get(s, MCP2515_EFLG);
    uint8_t before = get(s, ctrl);
    bool changed = tec < TEC_PASSIVE;
    if (tec < TEC_PASSIVE) {
        tec = (uint8_t)(tec + TEC_ACK_STEP);
        put(s, MCP2515_TEC, tec);
    }
    uint8_t flags = (uint8_t)((tec >= TEC_WARNING ? MCP2515_EFLG_EWARN | MCP2515_EFLG_TXWAR : 0u) |
        (tec >= TEC_PASSIVE ? MCP2515_EFLG_TXEP : 0u));
    if ((eflg | flags) != eflg) {
        put(s, MCP2515_EFLG, (uint8_t)(eflg | flags));
        put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) | MCP2515_INT_ERR));
    }
    put(s, ctrl, (uint8_t)(get(s, ctrl) | MCP2515_TXB_TXERR));
    if ((get(s, MCP2515_CANCTRL) & 0x08u) != 0) {
        put(s, ctrl, (uint8_t)((get(s, ctrl) & ~MCP2515_TXB_TXREQ) | MCP2515_TXB_ABTF));
    }
    return changed || get(s, ctrl) != before || get(s, MCP2515_EFLG) != eflg;
}

static void mcp2515_on_window(struct mclw_command_image *image, const uint8_t *bytes, size_t len, bool busy,
    void *ctx) {
    struct mcp2515_state *s = ctx;
    uint8_t op = len != 0 ? bytes[0] : 0;

    (void)image;
    (void)busy;
    if (len == 0) {
        return;
    }
    if (op == MCP2515_RESET) {
        reset_registers(s);
    } else if (op == MCP2515_WRITE && len >= 3) {
        for (size_t i = 2; i < len; i++) {
            write_register(s, (uint8_t)(bytes[1] + (i - 2u)), bytes[i]);
        }
    } else if (op == MCP2515_BIT_MODIFY && len >= 4) {
        uint8_t mask = bit_modifiable(bytes[1]) ? bytes[2] : 0xffu;
        write_register(s, bytes[1], (uint8_t)((get(s, bytes[1]) & ~mask) | (bytes[3] & mask)));
    } else if ((op & 0xf8u) == MCP2515_LOAD_TX_BUFFER && (op & 0x07u) <= 5u && len >= 2) {
        // 12.6: abc picks TXBn's SIDH (even) or D0 (odd).
        uint8_t start = (uint8_t)(MCP2515_TXB0CTRL + 0x10u * ((op & 0x07u) >> 1) + ((op & 1u) != 0 ? 6u : 1u));
        for (size_t i = 1; i < len; i++) {
            uint8_t reg = (uint8_t)(start + (i - 1u));
            if (reg < REGISTERS) {
                put(s, reg, bytes[i]);
            }
        }
    } else if ((op & 0xf8u) == MCP2515_RTS) {
        for (uint8_t n = 0; n < TX_BUFFERS; n++) {
            if ((op & (1u << n)) != 0) {
                uint8_t ctrl = (uint8_t)(MCP2515_TXB0CTRL + 0x10u * n);
                put(s, ctrl, (uint8_t)(get(s, ctrl) | MCP2515_TXB_TXREQ));
            }
        }
    } else if ((op & 0xf9u) == MCP2515_READ_RX_BUFFER) {
        // 12.4: reading a receive buffer this way clears its RXnIF once chip
        // select rises.
        uint8_t flag = (op & 0x04u) == 0 ? MCP2515_INT_RX0 : MCP2515_INT_RX1;
        put(s, MCP2515_CANINTF, (uint8_t)(get(s, MCP2515_CANINTF) & ~flag));
    } else if (op != MCP2515_READ && op != MCP2515_READ_STATUS && op != MCP2515_RX_STATUS) {
        s->ignored++;
    }
    mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_COMMAND, op, (uint16_t)len, 0);
    publish(s);
}

// WRITE and BIT MODIFY take effect at once on the part (a READ right after
// reads them), long before the model has taken the window: the engine runs
// this on the registers it serves at the window's release. Everything else
// the instruction does (an RTS, a mode's effect on the CAN side) waits for
// the model.
static void MCLW_RAM_FUNC(mcp2515_on_release)(const uint8_t *bytes, size_t len, uint8_t *const *pages, void *ctx) {
    uint8_t *page = pages[0];

    (void)ctx;
    if (bytes[0] == MCP2515_WRITE && len >= 3u) {
        for (size_t i = 2; i < len; i++) {
            page_write_register(page, (uint8_t)(bytes[1] + (i - 2u)), bytes[i]);
        }
    } else if (bytes[0] == MCP2515_BIT_MODIFY && len >= 4u) {
        uint8_t mask = bit_modifiable(bytes[1]) ? bytes[2] : 0xffu;

        if (bytes[1] < REGISTERS) {
            page_write_register(page, bytes[1], (uint8_t)((page[bytes[1]] & ~mask) | (bytes[3] & mask)));
        }
    }
}

static void build_image(struct mcp2515_state *s) {
    static const uint8_t responses[] = { 0x90, 0x92, 0x94, 0x96, MCP2515_READ_STATUS, MCP2515_RX_STATUS };

    mclw_command_init(&s->image, MCP2515_READ, MCP2515_READ);
    mclw_command_set_immediate_response(&s->image);
    mclw_command_set_short_register_read(&s->image);
    (void)mclw_command_add_page(&s->image, 0);
    for (size_t i = 0; i < sizeof(responses); i++) {
        (void)mclw_command_add_response(&s->image, responses[i]);
    }
    mclw_command_set_on_window(&s->image, mcp2515_on_window, s);
    mclw_command_set_on_release(&s->image, mcp2515_on_release, s);
    reset_registers(s);
}

static int mcp2515_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mcp2515_state *s = inst->state;
    int err;

    s->spi = bindings[BINDING_SPI].spi;
    s->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
    s->sent = 0;
    s->received = 0;
    s->overflows = 0;
    s->ignored = 0;
    build_image(s);
    err = mclw_hal_spi_target_arm_command(s->spi, &s->image, NULL);
    if (err == 0) {
        publish(s);
    }
    return err;
}

static void mcp2515_deinit(struct mclw_instance *inst) {
    struct mcp2515_state *s = inst->state;

    mclw_hal_spi_target_disarm(s->spi);
    s->spi = NULL;
    s->int_pin = NULL;
}

// A power cycle: the registers as after RESET.
static int mcp2515_reset(struct mclw_instance *inst, bool factory) {
    struct mcp2515_state *s = inst->state;

    (void)factory;
    reset_registers(s);
    s->sent = 0;
    s->received = 0;
    s->overflows = 0;
    s->ignored = 0;
    publish(s);
    return 0;
}

static int mcp2515_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mcp2515_state *s = inst->state;

    switch (property) {
        case PROP_MODE:
            out->v[0] = mode(s);
            return 0;
        case PROP_TEC:
            out->v[0] = get(s, MCP2515_TEC);
            return 0;
        case PROP_SENT:
            out->v[0] = s->sent;
            return 0;
        case PROP_RECEIVED:
            out->v[0] = s->received;
            return 0;
        case PROP_OVERFLOWS:
            out->v[0] = s->overflows;
            return 0;
        case PROP_IGNORED:
            out->v[0] = s->ignored;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

static int mcp2515_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    if (property == PROP_SPI_MODE) {
        return mclw_spi_mode_set(inst, BINDING_SPI, in);
    }
    return -EINVAL;
}

static int mcp2515_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct mcp2515_state *s = inst->state;
    uint8_t *out = buf;

    (void)region;
    for (size_t i = 0; i < len; i++) {
        out[i] = get(s, (uint8_t)(offset + i));
    }
    return 0;
}

// A frame takes about a millisecond on the bus at the bit rates CAN hosts
// use, so a request is sent at the next tick, one a tick.
static void mcp2515_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct mcp2515_state *s = inst->state;

    (void)now_us;
    if (transmit_one(s)) {
        publish(s);
    }
}

static const struct mclw_model_ops mcp2515_ops = {
    .init = mcp2515_init,
    .deinit = mcp2515_deinit,
    .reset = mcp2515_reset,
    .get = mcp2515_get,
    .set = mcp2515_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = mcp2515_read_region,
    .write_region = NULL,
    .time_advanced = mcp2515_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mcp2515 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mcp2515",
    .family = "mcp2515",
    .config = NULL,
    .properties = mcp2515_properties,
    .property_count = sizeof(mcp2515_properties) / sizeof(mcp2515_properties[0]),
    .regions = mcp2515_regions,
    .region_count = sizeof(mcp2515_regions) / sizeof(mcp2515_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = mcp2515_bindings,
    .binding_count = sizeof(mcp2515_bindings) / sizeof(mcp2515_bindings[0]),
    .state_size = sizeof(struct mcp2515_state),
    .ops = &mcp2515_ops,
};
