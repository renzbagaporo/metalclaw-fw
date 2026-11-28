// SPDX-License-Identifier: Apache-2.0 OR MIT
// The bridge to the emulator holding the DUT (BRIDGE.md): a Unix stream
// socket this process listens on, one newline-delimited JSON request at a
// time from the emulator, each answered before the DUT's clock moves on.
// Runs on the worker thread, so every engine call here is the worker's.

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#include "host.h"
#include "json.h"

// The DUT's GPIO lines (the RP2040's and the RP2350A's GP0-GP29).
#define DUT_LINES 30u
// The Pico's own signals, on no header pin: GP23 (SMPS mode), GP24 (VBUS
// sense), GP25 (the LED) and GP29 (VSYS/3, which soc_voltage reads).
#define ONBOARD_LINES ((1u << 23) | (1u << 24) | (1u << 25) | (1u << 29))
#define BRIDGE_VERSION 1
#define LINE_MAX 65536u
#define REPLY_MAX 262144u
#define ACTIONS_MAX 4096u
#define I2C_BUSES_MAX 8u
#define UART_PORTS_MAX 8u
#define SPI_BUSES_MAX 8u

enum action_kind {
    ACTION_GPIO,
    ACTION_UART,
    ACTION_WATCH,
    ACTION_LATCH_CAPTURE,
    ACTION_SPI_CLOCK,
    ACTION_JOIN,
    ACTION_UNJOIN,
    ACTION_PULL,
    ACTION_WAKE,
    ACTION_I2C_DRIVE,
};

struct action {
    uint64_t t_ns;
    uint8_t kind;
    uint8_t pin;
    uint8_t value;
    // I2C_DRIVE: the event's address or data byte.
    uint8_t extra;
    // LATCH_CAPTURE: the pins a sample holds besides the clock's three
    // (pin = clock, value = latch, extra = enable).
    uint32_t pins;
};

// Actions toward the DUT. Any thread may make one; the worker sends them
// with the next reply.
static struct action actions[ACTIONS_MAX];
static size_t action_count;
static uint32_t actions_dropped;

static struct {
    bool used;
    uint8_t sda;
    uint8_t scl;
} i2c_buses[I2C_BUSES_MAX];

// A UART the hello names is bridged: its bytes come as uart requests and
// go as uart actions while the latest route has it on its pins. Any other
// UART's pins (the console's UART0, a PIO program's) carry bits.
static struct {
    bool used;
    bool bridged;
    bool routed;
    // The DUT's pins: its TX is this side's receive line.
    uint8_t tx;
    uint8_t rx;
} uart_ports[UART_PORTS_MAX];

static struct {
    bool used;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
} spi_buses[SPI_BUSES_MAX];

static char line[LINE_MAX];
static bool pulls_due;

// A bus with no MISO (a half-duplex link), or a lookup naming none,
// matches on SCK and MOSI alone.
static int spi_bus_for(uint8_t sck, uint8_t mosi, uint8_t miso) {
    for (size_t i = 0; i < SPI_BUSES_MAX; i++) {
        if (spi_buses[i].used && spi_buses[i].sck == sck && spi_buses[i].mosi == mosi &&
            (spi_buses[i].miso == miso || spi_buses[i].miso == MCLW_PIN_NONE || miso == MCLW_PIN_NONE)) {
            return (int)i;
        }
    }
    return -1;
}
static size_t line_len;
static char reply[REPLY_MAX];
static struct json_doc doc;

static void add_action(uint64_t t_ns, uint8_t kind, uint8_t pin, uint8_t value) {
    uint32_t state = mclw_hal_critical_enter();

    if (action_count < ACTIONS_MAX) {
        actions[action_count++] = (struct action){ .t_ns = t_ns, .kind = kind, .pin = pin, .value = value, .extra = 0,
            .pins = 0 };
    } else {
        actions_dropped++;
    }
    mclw_hal_critical_exit(state);
}

// METALCLAW_HOST_IDLE_LOW: comma-separated DUT pins, as a mask.
static uint32_t idle_low_lines(void) {
    const char *list = getenv("METALCLAW_HOST_IDLE_LOW");
    uint32_t mask = 0;

    while (list != NULL && *list != '\0') {
        char *end;
        unsigned long pin = strtoul(list, &end, 10);

        if (end == list) {
            break;
        }
        if (pin < DUT_LINES) {
            mask |= 1u << pin;
        }
        list = *end == ',' ? end + 1 : end;
    }
    return mask;
}

void mclw_host_event_gpio(uint8_t pin, uint8_t state) {
    add_action(mclw_host_event_ns(), ACTION_GPIO, pin, state);
}

void mclw_host_event_gpio_at(uint8_t pin, uint8_t state, uint64_t t_ns) {
    add_action(t_ns, ACTION_GPIO, pin, state);
}

static int spi_bus_for(uint8_t sck, uint8_t mosi, uint8_t miso);

// A controller names the data pins from its own side, the swap of the
// bus's: the DUT's RX pin takes the controller's MOSI. Its SCK is the bus.
bool mclw_host_event_spi_clock(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint16_t mosi, uint8_t bits, bool hold,
    uint64_t t_ns) {
    int bus = spi_bus_for(sck, mosi_pin, miso_pin);

    if (bus < 0) {
        bus = spi_bus_for(sck, miso_pin, mosi_pin);
    }

    if (bus < 0) {
        return false;
    }
    uint32_t state = mclw_hal_critical_enter();

    if (action_count < ACTIONS_MAX) {
        actions[action_count++] = (struct action){ .t_ns = t_ns, .kind = ACTION_SPI_CLOCK, .pin = (uint8_t)bus,
            .value = bits, .extra = hold ? 1u : 0u, .pins = mosi };
    } else {
        actions_dropped++;
    }
    mclw_hal_critical_exit(state);
    return true;
}

static int i2c_bus_for(uint8_t sda, uint8_t scl) {
    for (size_t i = 0; i < I2C_BUSES_MAX; i++) {
        if (i2c_buses[i].used && i2c_buses[i].sda == sda && i2c_buses[i].scl == scl) {
            return (int)i;
        }
    }
    return -1;
}

// One I2C event toward the DUT's target on the bus with these pins (the
// harness as controller): the action's `value` is the event and, in bit
// 4, a start's read or a read's ack; `extra` is a start's address or a
// write's byte.
bool mclw_host_event_i2c_drive(uint8_t sda, uint8_t scl, uint8_t ev, uint8_t arg, bool flag, uint64_t t_ns) {
    int bus = i2c_bus_for(sda, scl);
    uint32_t state;

    if (bus < 0) {
        return false;
    }
    state = mclw_hal_critical_enter();
    if (action_count < ACTIONS_MAX) {
        actions[action_count++] = (struct action){ .t_ns = t_ns, .kind = ACTION_I2C_DRIVE, .pin = (uint8_t)bus,
            .value = (uint8_t)(ev | (flag ? 0x10u : 0u)), .extra = arg, .pins = 0 };
    } else {
        actions_dropped++;
    }
    mclw_hal_critical_exit(state);
    return true;
}

void mclw_host_event_join(uint8_t a, uint8_t b, bool joined) {
    add_action(mclw_host_event_ns(), joined ? ACTION_JOIN : ACTION_UNJOIN, a, b);
}

void mclw_host_event_wake(uint64_t t_ns) {
    add_action(t_ns, ACTION_WAKE, 0, 0);
}

void mclw_host_event_watch(uint8_t pin) {
    add_action(mclw_host_event_ns(), ACTION_WATCH, pin, 0);
}

void mclw_host_event_latch_capture(uint8_t clock, uint8_t latch, uint8_t enable, uint32_t pins) {
    uint32_t state = mclw_hal_critical_enter();

    if (action_count < ACTIONS_MAX) {
        actions[action_count++] = (struct action){ .t_ns = mclw_host_event_ns(), .kind = ACTION_LATCH_CAPTURE,
            .pin = clock, .value = latch, .extra = enable, .pins = pins };
    } else {
        actions_dropped++;
    }
    mclw_hal_critical_exit(state);
}

void mclw_host_event_uart(uint8_t rx_pin, uint8_t byte, uint64_t t_ns) {
    add_action(t_ns, ACTION_UART, rx_pin, byte);
}

// A break toward the DUT's UART, ending at `t_ns`.
void mclw_host_event_uart_break(uint8_t rx_pin, uint64_t t_ns) {
    uint32_t state = mclw_hal_critical_enter();

    if (action_count < ACTIONS_MAX) {
        actions[action_count++] = (struct action){ .t_ns = t_ns, .kind = ACTION_UART, .pin = rx_pin, .value = 0,
            .extra = 1u, .pins = 0 };
    } else {
        actions_dropped++;
    }
    mclw_hal_critical_exit(state);
}

bool mclw_host_pin_is_uart_tx(uint8_t pin) {
    for (size_t i = 0; i < UART_PORTS_MAX; i++) {
        if (uart_ports[i].bridged && uart_ports[i].routed && uart_ports[i].tx == pin) {
            return true;
        }
    }
    return false;
}

bool mclw_host_pin_is_uart_rx(uint8_t pin) {
    for (size_t i = 0; i < UART_PORTS_MAX; i++) {
        if (uart_ports[i].bridged && uart_ports[i].routed && uart_ports[i].rx == pin) {
            return true;
        }
    }
    return false;
}

static int uart_port_for_rx(uint8_t rx_pin) {
    for (size_t i = 0; i < UART_PORTS_MAX; i++) {
        if (uart_ports[i].used && uart_ports[i].rx == rx_pin) {
            return (int)i;
        }
    }
    return -1;
}

// Appends `,"actions":[...]` for every action made so far; the reply's
// own fields come before it.
static size_t put_actions(size_t at) {
    uint32_t state = mclw_hal_critical_enter();
    size_t n = action_count;

    if (n != 0) {
        at += (size_t)snprintf(reply + at, REPLY_MAX - at, ",\"actions\":[");
        for (size_t i = 0; i < n && at < REPLY_MAX - 128u; i++) {
            const struct action *a = &actions[i];

            if (a->kind == ACTION_GPIO) {
                if (a->value == MCLW_HOST_GPIO_RELEASED) {
                    at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                        "%s{\"t_ns\":%" PRIu64 ",\"op\":\"gpio\",\"pin\":%u,\"level\":\"z\"}", i ? "," : "",
                        a->t_ns, a->pin);
                } else {
                    at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                        "%s{\"t_ns\":%" PRIu64 ",\"op\":\"gpio\",\"pin\":%u,\"level\":%u}", i ? "," : "", a->t_ns,
                        a->pin, a->value);
                }
            } else if (a->kind == ACTION_SPI_CLOCK) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"spi_clock\",\"bus\":%u,\"mosi\":%" PRIu32 ",\"bits\":%u%s}",
                    i ? "," : "", a->t_ns, a->pin, a->pins, a->value, a->extra ? ",\"cs_hold\":true" : "");
            } else if (a->kind == ACTION_JOIN || a->kind == ACTION_UNJOIN) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"%s\",\"pins\":[%u,%u]}", i ? "," : "", a->t_ns,
                    a->kind == ACTION_JOIN ? "join" : "unjoin", a->pin, a->value);
            } else if (a->kind == ACTION_I2C_DRIVE) {
                static const char *const names[] = { "start", "write", "read", "stop" };
                uint8_t ev = a->value & 0x0fu;
                bool flag = (a->value & 0x10u) != 0u;

                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"i2c_drive\",\"bus\":%u,\"ev\":\"%s\"", i ? "," : "",
                    a->t_ns, a->pin, names[ev & 3u]);
                if (ev == MCLW_HOST_I2C_START) {
                    at += (size_t)snprintf(reply + at, REPLY_MAX - at, ",\"addr\":%u,\"read\":%s", a->extra,
                        flag ? "true" : "false");
                } else if (ev == MCLW_HOST_I2C_WRITE) {
                    at += (size_t)snprintf(reply + at, REPLY_MAX - at, ",\"data\":%u", a->extra);
                } else if (ev == MCLW_HOST_I2C_READ) {
                    at += (size_t)snprintf(reply + at, REPLY_MAX - at, ",\"ack\":%s", flag ? "true" : "false");
                }
                at += (size_t)snprintf(reply + at, REPLY_MAX - at, "}");
            } else if (a->kind == ACTION_WAKE) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at, "%s{\"t_ns\":%" PRIu64 ",\"op\":\"wake\"}",
                    i ? "," : "", a->t_ns);
            } else if (a->kind == ACTION_PULL) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"pull\",\"pin\":%u,\"level\":%u}", i ? "," : "", a->t_ns,
                    a->pin, a->value);
            } else if (a->kind == ACTION_LATCH_CAPTURE) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"latch_capture\",\"clock\":%u,\"latch\":%u,\"enable\":%u,"
                    "\"pins\":[", i ? "," : "", a->t_ns, a->pin, a->value, a->extra);
                for (uint8_t pin = 0, first = 1; pin < 32u; pin++) {
                    if ((a->pins >> pin) & 1u) {
                        at += (size_t)snprintf(reply + at, REPLY_MAX - at, "%s%u", first ? "" : ",", pin);
                        first = 0;
                    }
                }
                at += (size_t)snprintf(reply + at, REPLY_MAX - at, "]}");
            } else if (a->kind == ACTION_WATCH) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"watch\",\"pin\":%u}", i ? "," : "", a->t_ns, a->pin);
            } else if (a->extra != 0u) {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"uart\",\"port\":%d,\"break\":true}", i ? "," : "", a->t_ns,
                    uart_port_for_rx(a->pin));
            } else {
                at += (size_t)snprintf(reply + at, REPLY_MAX - at,
                    "%s{\"t_ns\":%" PRIu64 ",\"op\":\"uart\",\"port\":%d,\"data\":%u}", i ? "," : "", a->t_ns,
                    uart_port_for_rx(a->pin), a->value);
            }
        }
        at += (size_t)snprintf(reply + at, REPLY_MAX - at, "]");
        action_count = 0;
    }
    mclw_hal_critical_exit(state);
    return at;
}

// A request may carry the pins its block is routed to now ("pins"), which
// override the hello's for a program that moved the block; for I2C they
// hold until the next START names others.
static uint8_t pin_or(const struct json_node *root, const char *name, uint8_t fallback) {
    const struct json_node *pins = json_get(&doc, root, "pins");
    int64_t pin = json_int(&doc, pins, name, -1);

    return pin >= 0 ? (uint8_t)pin : fallback;
}

static bool i2c_pins(const struct json_node *root, int64_t bus, uint8_t *sda, uint8_t *scl) {
    if (bus < 0 || bus >= (int64_t)I2C_BUSES_MAX) {
        return false;
    }
    if (json_get(&doc, root, "pins") != NULL) {
        i2c_buses[bus].used = true;
        i2c_buses[bus].sda = pin_or(root, "sda", i2c_buses[bus].sda);
        i2c_buses[bus].scl = pin_or(root, "scl", i2c_buses[bus].scl);
    }
    if (!i2c_buses[bus].used) {
        return false;
    }
    *sda = i2c_buses[bus].sda;
    *scl = i2c_buses[bus].scl;
    return true;
}

// The buses' and ports' DUT pins, from a hello or a route report: each
// block named replaces its pins; a block not named keeps them (a bus the
// emulator decodes from pins, which no routing moves).
static void bind_blocks(const struct json_node *root, bool hello) {
    const struct json_node *list = json_get(&doc, root, "i2c");

    for (const struct json_node *b = json_child(&doc, list); b != NULL; b = json_next(&doc, b)) {
        int64_t bus = json_int(&doc, b, "bus", -1);

        if (bus >= 0 && bus < (int64_t)I2C_BUSES_MAX) {
            i2c_buses[bus].used = true;
            i2c_buses[bus].sda = (uint8_t)json_int(&doc, b, "sda", 0);
            i2c_buses[bus].scl = (uint8_t)json_int(&doc, b, "scl", 0);
        }
    }
    // A route names every UART that has pins: one it leaves out has none.
    list = json_get(&doc, root, "uart");
    if (!hello) {
        for (size_t i = 0; i < UART_PORTS_MAX; i++) {
            uart_ports[i].routed = false;
        }
    }
    for (const struct json_node *u = json_child(&doc, list); u != NULL; u = json_next(&doc, u)) {
        int64_t port = json_int(&doc, u, "port", -1);

        if (port >= 0 && port < (int64_t)UART_PORTS_MAX) {
            uart_ports[port].used = true;
            uart_ports[port].bridged |= hello;
            uart_ports[port].routed = true;
            uart_ports[port].tx = (uint8_t)json_int(&doc, u, "tx", 0);
            uart_ports[port].rx = (uint8_t)json_int(&doc, u, "rx", 0);
        }
    }
    mclw_host_uart_route_changed();
    list = json_get(&doc, root, "spi");
    for (const struct json_node *b = json_child(&doc, list); b != NULL; b = json_next(&doc, b)) {
        int64_t bus = json_int(&doc, b, "bus", -1);

        if (bus >= 0 && bus < (int64_t)SPI_BUSES_MAX) {
            spi_buses[bus].used = true;
            spi_buses[bus].sck = (uint8_t)json_int(&doc, b, "sck", 0);
            spi_buses[bus].mosi = (uint8_t)json_int(&doc, b, "mosi", 0);
            spi_buses[bus].miso = (uint8_t)json_int(&doc, b, "miso", MCLW_PIN_NONE);
        }
    }
}

static size_t handle_hello(const struct json_node *root) {
    memset(i2c_buses, 0, sizeof(i2c_buses));
    memset(uart_ports, 0, sizeof(uart_ports));
    memset(spi_buses, 0, sizeof(spi_buses));
    bind_blocks(root, true);
    pulls_due = true;
    return (size_t)snprintf(reply, REPLY_MAX, "{\"version\":%d", BRIDGE_VERSION);
}

static size_t handle_i2c(const struct json_node *root) {
    const struct json_node *ev = json_get(&doc, root, "ev");
    uint8_t sda;
    uint8_t scl;

    if (!i2c_pins(root, json_int(&doc, root, "bus", -1), &sda, &scl)) {
        return (size_t)snprintf(reply, REPLY_MAX, "{\"ack\":false,\"error\":\"unknown bus\"");
    }
    if (json_is(ev, "start")) {
        bool ack = mclw_host_i2c_start(sda, scl, (uint8_t)json_int(&doc, root, "addr", 0),
            json_bool(&doc, root, "read", false));
        return (size_t)snprintf(reply, REPLY_MAX, "{\"ack\":%s", ack ? "true" : "false");
    }
    if (json_is(ev, "write")) {
        bool ack = mclw_host_i2c_write(sda, scl, (uint8_t)json_int(&doc, root, "data", 0));
        return (size_t)snprintf(reply, REPLY_MAX, "{\"ack\":%s", ack ? "true" : "false");
    }
    if (json_is(ev, "read")) {
        return (size_t)snprintf(reply, REPLY_MAX, "{\"data\":%u", mclw_host_i2c_read(sda, scl));
    }
    if (json_is(ev, "stop")) {
        mclw_host_i2c_stop(sda, scl);
        return (size_t)snprintf(reply, REPLY_MAX, "{");
    }
    return (size_t)snprintf(reply, REPLY_MAX, "{\"error\":\"unknown i2c event\"");
}

// One request: the DUT's time first, then the op, then the reply with
// every action made meanwhile.
static size_t handle(const char *text, size_t len) {
    const struct json_node *root;
    const struct json_node *op;
    size_t at;

    if (!json_parse(&doc, text, len)) {
        return (size_t)snprintf(reply, REPLY_MAX, "{\"error\":\"malformed\"}\n");
    }
    root = &doc.nodes[0];
    op = json_get(&doc, root, "op");
    // When this request happened: what the DUT reports (an edge) is timed
    // by it, never by a delay something else ran up.
    uint64_t request_ns;
    {
        int64_t t_ns = json_int(&doc, root, "t_ns", -1);

        if (t_ns >= 0) {
            mclw_host_advance((uint64_t)t_ns);
        }
        request_ns = t_ns >= 0 ? (uint64_t)t_ns : mclw_host_event_ns();
        mclw_host_uart_poll(mclw_host_event_ns());
        mclw_host_wire_poll(mclw_host_event_ns());
    }
    // The host harness models a board with line pull-ups on the Pico's
    // headers, like metalclaw_pico_rp2 (METALCLAW_LINE_PULLUPS,
    // variant/mpconfigvariant.h): a line nothing drives reads high. The
    // lines METALCLAW_HOST_IDLE_LOW names are pulled down instead, as the
    // rig holds a line low on such a board where an absent part would
    // look active; a part that drives the line wins over either pull. The pulls
    // go with the first reply after the hello's.
    if (pulls_due && !json_is(op, "hello")) {
        uint32_t low = idle_low_lines();

        pulls_due = false;
        for (uint8_t pin = 0; pin < DUT_LINES; pin++) {
            if ((ONBOARD_LINES & (1u << pin)) == 0u) {
                add_action(mclw_host_event_ns(), ACTION_PULL, pin, (low & (1u << pin)) != 0u ? 0u : 1u);
            }
        }
    }
    if (json_is(op, "hello")) {
        at = handle_hello(root);
    } else if (json_is(op, "route")) {
        bind_blocks(root, false);
        at = (size_t)snprintf(reply, REPLY_MAX, "{");
    } else if (json_is(op, "tick") || json_is(op, "wake")) {
        at = (size_t)snprintf(reply, REPLY_MAX, "{");
    } else if (json_is(op, "i2c")) {
        at = handle_i2c(root);
    } else if (json_is(op, "uart")) {
        int64_t port = json_int(&doc, root, "port", -1);
        const struct json_node *data = json_get(&doc, root, "data");
        uint8_t bytes[1024];
        size_t n = 0;

        if (data != NULL && data->kind == JSON_NUMBER) {
            bytes[n++] = (uint8_t)data->number;
        }
        for (const struct json_node *b = data != NULL && data->kind == JSON_ARRAY ? json_child(&doc, data) : NULL;
             b != NULL && n < sizeof(bytes); b = json_next(&doc, b)) {
            bytes[n++] = (uint8_t)b->number;
        }
        if (port >= 0 && port < (int64_t)UART_PORTS_MAX && (uart_ports[port].used ||
                json_get(&doc, root, "pins") != NULL)) {
            uart_ports[port].used = true;
            uart_ports[port].bridged = true;
            uart_ports[port].routed = true;
            uart_ports[port].tx = pin_or(root, "tx", uart_ports[port].tx);
            uart_ports[port].rx = pin_or(root, "rx", uart_ports[port].rx);
            mclw_host_uart_receive(uart_ports[port].tx, bytes, n);
            if (json_bool(&doc, root, "break", false)) {
                mclw_host_uart_receive_break(uart_ports[port].tx);
            }
            at = (size_t)snprintf(reply, REPLY_MAX, "{");
        } else {
            at = (size_t)snprintf(reply, REPLY_MAX, "{\"error\":\"unknown uart port\"");
        }
    } else if (json_is(op, "spi")) {
        int64_t bus = json_int(&doc, root, "bus", -1);
        uint8_t miso = 0xff;

        if (bus >= 0 && bus < (int64_t)SPI_BUSES_MAX && (spi_buses[bus].used ||
                json_get(&doc, root, "pins") != NULL)) {
            uint8_t sck = pin_or(root, "sck", spi_buses[bus].sck);
            uint8_t mosi_pin = pin_or(root, "mosi", spi_buses[bus].mosi);
            uint16_t mosi = (uint16_t)json_int(&doc, root, "mosi", 0);
            uint8_t bits = (uint8_t)json_int(&doc, root, "bits", 8);
            // A frame whose pins name no MISO has none, whatever the bus has.
            const struct json_node *pins = json_get(&doc, root, "pins");
            uint8_t miso_pin = pins != NULL ? pin_or(root, "miso", MCLW_PIN_NONE) : spi_buses[bus].miso;

            uint8_t next = 0xff;
            uint16_t wide = bits == 16u ? 0xffffu : 0xffu;
            bool target = bits == 8u && mclw_host_spi_frame(sck, mosi_pin, miso_pin, (uint8_t)mosi, &miso) == 0;

            if (target) {
                wide = miso;
            } else if (bits != 8u || (!mclw_host_spi_sink_frame(sck, mosi_pin, (uint8_t)mosi) &&
                           !mclw_host_wire_spi_frame(sck, mosi_pin, (uint8_t)mosi))) {
                (void)mclw_host_spi_peer_frame(sck, mosi_pin, miso_pin, mosi, bits, &wide);
            }
            if (target && mclw_host_spi_next_miso(sck, mosi_pin, miso_pin, &next) == 0) {
                at = (size_t)snprintf(reply, REPLY_MAX, "{\"miso\":%u,\"miso_next\":%u", miso, next);
            } else {
                at = (size_t)snprintf(reply, REPLY_MAX, "{\"miso\":%u", wide);
            }
        } else {
            at = (size_t)snprintf(reply, REPLY_MAX, "{\"miso\":255,\"error\":\"unknown spi bus\"");
        }
    } else if (json_is(op, "i2c_driven")) {
        int64_t bus = json_int(&doc, root, "bus", -1);
        const struct json_node *ack = json_get(&doc, root, "ack");
        const struct json_node *data = json_get(&doc, root, "data");

        if (bus >= 0 && bus < (int64_t)I2C_BUSES_MAX && i2c_buses[bus].used) {
            mclw_host_i2c_driven(i2c_buses[bus].sda, i2c_buses[bus].scl,
                ack == NULL || ack->kind != JSON_BOOL ? -1 : (ack->boolean ? 1 : 0),
                data != NULL && data->kind == JSON_NUMBER ? (int)data->number : -1, mclw_host_event_ns());
        }
        at = (size_t)snprintf(reply, REPLY_MAX, "{");
    } else if (json_is(op, "latched")) {
        static uint32_t samples[512];
        size_t n = 0;
        bool overflow = json_bool(&doc, root, "overflow", false);
        const struct json_node *list = json_get(&doc, root, "samples");

        for (const struct json_node *v = json_child(&doc, list); v != NULL; v = json_next(&doc, v)) {
            if (n == sizeof(samples) / sizeof(samples[0])) {
                overflow = true;
                break;
            }
            samples[n++] = v->kind == JSON_NUMBER ? (uint32_t)v->number : 0u;
        }
        mclw_host_wire_latched((uint8_t)json_int(&doc, root, "clock", MCLW_PIN_NONE), samples, n,
            (uint64_t)json_int(&doc, root, "enabled_ns", 0), overflow);
        at = (size_t)snprintf(reply, REPLY_MAX, "{");
    } else if (json_is(op, "spi_clocked")) {
        int64_t bus = json_int(&doc, root, "bus", -1);
        const struct json_node *miso = json_get(&doc, root, "miso");

        if (bus >= 0 && bus < (int64_t)SPI_BUSES_MAX && spi_buses[bus].used) {
            mclw_host_spi_peer_clocked(spi_buses[bus].sck, miso != NULL && miso->kind == JSON_NUMBER ? (int)miso->number
                                                                                                   : -1);
        }
        at = (size_t)snprintf(reply, REPLY_MAX, "{");
    } else if (json_is(op, "gpio")) {
        const struct json_node *level = json_get(&doc, root, "level");
        int value = level != NULL && level->kind == JSON_NUMBER ? (level->number != 0) : -1;
        uint8_t pin = (uint8_t)json_int(&doc, root, "pin", 0xff);

        mclw_host_gpio_dut(pin, value);
        mclw_host_wire_edge(pin, value, request_ns);
        if (value >= 0) {
            mclw_host_uart_edge(pin, value, request_ns);
        }
        mclw_host_spi_sink_line(pin, value);
        if (value >= 0) {
            mclw_host_spi_cs(pin, value != 0);
            (void)mclw_host_spi_sink_cs(pin, value != 0);
        }
        uint8_t sck;
        uint8_t mosi_pin;
        uint8_t miso_pin;
        uint8_t first;

        if (value == 0 && mclw_host_spi_target_on_cs(pin, &sck, &mosi_pin, &miso_pin) &&
            mclw_host_spi_next_miso(sck, mosi_pin, miso_pin, &first) == 0) {
            at = (size_t)snprintf(reply, REPLY_MAX, "{\"miso_next\":%u", first);
        } else {
            at = (size_t)snprintf(reply, REPLY_MAX, "{");
        }
    } else {
        at = (size_t)snprintf(reply, REPLY_MAX, "{\"error\":\"unknown op\"");
    }
    // An op that changed something may have posted work; run it at this
    // time, so what it makes goes out with this reply.
    mclw_host_drain_work();
    at = put_actions(at);
    // A reply that opened with "{" alone has no field before the actions.
    if (at >= 2u && reply[0] == '{' && reply[1] == ',') {
        memmove(reply + 1, reply + 2, at - 2u);
        at--;
    }
    at += (size_t)snprintf(reply + at, REPLY_MAX - at, "}\n");
    return at;
}

static bool send_all(int fd, const char *buf, size_t len) {
    while (len != 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);

        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static void serve(int fd, int wake_fd) {
    line_len = 0;
    for (;;) {
        struct pollfd p[2] = {
            { .fd = fd, .events = POLLIN, .revents = 0 },
            { .fd = wake_fd, .events = POLLIN, .revents = 0 },
        };

        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if ((p[1].revents & POLLIN) != 0) {
            mclw_host_drain_wake(wake_fd);
            mclw_host_drain_work();
        }
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
        }
        ssize_t n = recv(fd, line + line_len, LINE_MAX - line_len, 0);

        if (n <= 0) {
            return;
        }
        line_len += (size_t)n;
        for (;;) {
            char *nl = memchr(line, '\n', line_len);
            size_t used;

            if (nl == NULL) {
                if (line_len == LINE_MAX) {
                    return;
                }
                break;
            }
            used = (size_t)(nl - line) + 1u;
            if (used > 1u) {
                size_t out = handle(line, used - 1u);

                if (!send_all(fd, reply, out)) {
                    return;
                }
            }
            memmove(line, line + used, line_len - used);
            line_len -= used;
        }
    }
}

void mclw_host_bridge_run(const char *path, int wake_fd) {
    struct sockaddr_un addr;
    int listener;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "metalclaw: bridge path too long: %s\n", path);
        sleep(1);
        return;
    }
    strcpy(addr.sun_path, path);
    (void)unlink(path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        perror("metalclaw: bridge socket");
        if (listener >= 0) {
            close(listener);
        }
        sleep(1);
        return;
    }
    for (;;) {
        struct pollfd p[2] = {
            { .fd = listener, .events = POLLIN, .revents = 0 },
            { .fd = wake_fd, .events = POLLIN, .revents = 0 },
        };

        if (poll(p, 2, -1) < 0 && errno != EINTR) {
            break;
        }
        // Before the emulator connects, posted work still runs, at time 0.
        if ((p[1].revents & POLLIN) != 0) {
            mclw_host_drain_wake(wake_fd);
            mclw_host_drain_work();
        }
        if ((p[0].revents & POLLIN) != 0) {
            int fd = accept(listener, NULL, NULL);

            if (fd >= 0) {
                serve(fd, wake_fd);
                close(fd);
            }
        }
    }
    close(listener);
}
