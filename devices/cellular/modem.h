// SPDX-License-Identifier: Apache-2.0 OR MIT
// The engine every cellular model of the package runs on: the power state
// machine on PWRKEY, AT command lines and data phases out of the UART byte
// stream, V.250 result framing and +CME/+CMS errors (3GPP TS 27.007 9.2,
// TS 27.005 3.2.5), one output FIFO, timed URCs, dispatch over the model's
// command tables, the commands 27.007 and V.250 define, and the building
// blocks of a model's descriptor tables.
//
// A model's state struct starts with a `struct modem`; the engine finds the
// model's `struct modem_config` through `model->config`.
//
// Commands arrive on channels: the raw UART, or after AT+CMUX the DLCIs of
// a 27.010 multiplexer (cmux.h), each with its own line, echo, data phase
// and deferred result. A handler answers on the channel its command came
// from without knowing which: the channel is engine context. A channel
// that dialled with ATD*99# carries PPP (ppp.h) to the IP stack (ip.h)
// until the link ends and it answers NO CARRIER. A model gets CMUX and PPP
// by giving the engine a `struct modem_link` and mapping +CMUX and D to
// modem_cmd_cmux and modem_cmd_dial_ppp in its table.

#ifndef MCLW_DEVICES_CELLULAR_MODEM_H
#define MCLW_DEVICES_CELLULAR_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"

#include "cmux.h"
#include "gnss.h"
#include "net.h"
#include "ppp.h"
#include "text.h"

#define MODEM_LINE_MAX 160
#define MODEM_OUT_SIZE 704
#define MODEM_STASH 48
#define MODEM_ARGS_MAX 10

#define MODEM_BAUD_MIN 9600
#define MODEM_BAUD_MAX 921600
#define MODEM_BOOT_MS_MAX 20000u
#define MODEM_YEAR_MIN 1980
#define MODEM_YEAR_MAX 2099
// 27.007 +CCLK carries -96..+96 quarter hours; real zones span -12..+14 h.
#define MODEM_TIMEZONE_MIN (-48)
#define MODEM_TIMEZONE_MAX 56
#define MODEM_BATTERY_MV_MAX 5000
// 27.007 +CSQ <rssi>: 0..31, or 99 for unknown.
#define MODEM_RSSI_MAX 31
#define MODEM_RSSI_UNKNOWN 99

#define MODEM_DEFAULT_RSSI 20
#define MODEM_DEFAULT_YEAR 2026
#define MODEM_DEFAULT_BATTERY_MV 3800
#define MODEM_DEFAULT_BATTERY_PERCENT 75

// 27.007 9.2 and 27.005 3.2.5.
#define MODEM_CME_NOT_ALLOWED 3
#define MODEM_CME_NOT_SUPPORTED 4
#define MODEM_CME_SIM_NOT_INSERTED 10
#define MODEM_CME_SIM_PIN_REQUIRED 11
#define MODEM_CME_NO_NETWORK 30
#define MODEM_CMS_NOT_ALLOWED 302
#define MODEM_CMS_INVALID_PDU 304
#define MODEM_CMS_SIM_NOT_INSERTED 310
#define MODEM_CMS_SIM_PIN 311
#define MODEM_CMS_INVALID_INDEX 321
#define MODEM_CMS_NO_NETWORK 331

// 27.007 +CREG/+CGREG/+CEREG <stat>.
#define MODEM_REG_NONE 0
#define MODEM_REG_HOME 1
#define MODEM_REG_ROAMING 5

// 27.007 <AcT> of +COPS.
#define MODEM_ACT_CATM 7
#define MODEM_ACT_NBIOT 9

#define MODEM_CTRL_Z 0x1a
#define MODEM_ESC 0x1b

enum {
    MODEM_BINDING_UART,
    // PWRKEY or RESET_N, as `power_pin` says.
    MODEM_BINDING_PWRKEY,
};

// What the second binding is. PWRKEY: a press of `pwrkey_on_us` turns an
// off modem on, one of `pwrkey_off_us` turns it off. RESET_N: the modem is
// powered from attach; the line low for `reset_us` holds it in reset and
// its release boots it.
enum modem_power_pin {
    MODEM_PIN_PWRKEY,
    MODEM_PIN_RESET,
};

// CMUX DLCIs past 1 that hold an AT channel at once: DLCI 2, and one of
// the user pipes 3 and 4.
#define MODEM_LINK_CHANNELS 2
// The UART bytes that may be queued or in flight before the link adds
// another PPP frame: with MRU 296 it keeps the latency of an ACK the DUT
// sees near 75 ms (spec E.2), and AT answers wait behind it at most 22 ms.
#define MODEM_LINK_GATE 256u

enum modem_power {
    MODEM_OFF,
    MODEM_BOOTING,
    MODEM_ON,
};

enum modem_mode {
    MODEM_EXEC,
    MODEM_READ,
    MODEM_TEST,
    MODEM_WRITE,
};

// URC kinds. The engine's come first and a model numbers its own from
// MODEM_EV_MODEL. A kind with MODEM_EV_PER_ARG set queues one event per
// argument (a connection) and a repeat leaves the queued one as it is;
// without it a newer event replaces the queued one of its kind.
#define MODEM_EV_NONE 0
#define MODEM_EV_REG 1
#define MODEM_EV_CPIN 2
#define MODEM_EV_MODEL 3
#define MODEM_EV_PER_ARG 0x80

// MODEM_EV_REG's argument: the stat in the low nibble, and which of the
// three registration URCs to send. One pending event keeps its place and
// takes the union of the URCs; each reports its domain's stat when it
// goes.
#define MODEM_REG_URC_CREG 0x10
#define MODEM_REG_URC_CGREG 0x20
#define MODEM_REG_URC_CEREG 0x40

// MODEM_EV_CPIN arguments.
#define MODEM_CPIN_READY 0
#define MODEM_CPIN_NOT_READY 1
#define MODEM_CPIN_NOT_INSERTED 2
#define MODEM_CPIN_SIM_PIN 3

struct modem;
struct sms;

struct modem_arg {
    char *str;
    int32_t num;
    bool is_num;
    bool quoted;
};

// A write form's arguments, split on commas outside quotes, quotes
// stripped, every one NUL terminated; past the tenth they are ignored.
struct modem_args {
    uint8_t count;
    struct modem_arg v[MODEM_ARGS_MAX];
};

typedef void (*modem_handler_t)(struct modem *m, enum modem_mode mode, struct modem_args *a);

// `name` with its `+` (or a vendor's `^`), upper case; "D" takes every
// ATD. A NULL `fn` makes the name unknown, so a model's table can take a
// command out of a table searched after it.
struct modem_command {
    const char *name;
    modem_handler_t fn;
};

struct modem_commands {
    const struct modem_command *list;
    size_t count;
};

#define MODEM_COMMANDS(array) { .list = (array), .count = sizeof(array) / sizeof((array)[0]) }

// A data phase after a `> ` prompt. A counted phase hands `data` exactly the
// prompt's length in pieces, then ends and calls `complete`. An uncounted
// one (length 0) hands `data` everything that arrives; `data` takes at
// least one byte, returns how many, and ends the phase with
// modem_phase_end(). `expired` runs when the prompt's timeout passes first,
// the phase already ended. A hook a phase cannot reach may be NULL.
struct modem_phase {
    size_t (*data)(struct modem *m, const uint8_t *bytes, size_t len);
    void (*complete)(struct modem *m);
    void (*expired)(struct modem *m);
};

// A run of a model's property and region tables, in table order, and the
// state behind it. `defaults` restores what the user sets (reset(factory)
// and attach), `clear` what reset() clears, `session` what every power
// transition clears; any may be NULL.
struct modem_part {
    uint8_t property_count;
    uint8_t region_count;
    void (*defaults)(struct modem *m);
    void (*clear)(struct modem *m);
    void (*session)(struct modem *m);
    int (*get)(struct modem *m, size_t property, struct mclw_value *out);
    int (*set)(struct modem *m, size_t property, const struct mclw_value *in);
    void *(*region)(struct modem *m, size_t region);
    void (*written)(struct modem *m, size_t region);
};

struct modem_config {
    const char *manufacturer;
    const char *model;
    const char *revision;
    // ATI.
    const char *product;
    const char *imei;
    const char *imsi;
    const char *iccid;
    const char *operator_name;
    const uint32_t *rates;
    uint8_t rate_count;
    const char *ipr_test;
    // Bit n accepts AT+CFUN=n, and of `cfun_reboot` reboots on it.
    uint8_t cfun_levels;
    uint8_t cfun_reboot;
    const char *cfun_test;
    uint32_t pwrkey_on_us;
    uint32_t pwrkey_off_us;
    uint32_t boot_ms;
    // MODEM_PIN_PWRKEY unless the part is driven by RESET_N; then the
    // shortest low pulse that resets it.
    uint8_t power_pin;
    uint32_t reset_us;
    // The line a normal power down sends, or NULL.
    const char *power_down;
    // The room the output FIFO needs before another command line runs: the
    // longest answer that is not clamped to the FIFO.
    uint16_t command_room;
    // The room a URC waits for: the longest URC.
    uint16_t urc_room;
    // Searched in order; the first entry with the name wins.
    const struct modem_commands *const *tables;
    uint8_t table_count;
    const struct modem_part *const *parts;
    uint8_t part_count;
    // The hooks, all required. Sends the boot URCs; the modem has just
    // turned on with a fresh session.
    void (*booted)(struct modem *m);
    // Delivers one of the model's URC kinds.
    void (*event)(struct modem *m, uint8_t kind, uint8_t arg);
    // The network was lost with the bearer up; `net->ses.bearer` is already
    // false.
    void (*bearer_lost)(struct modem *m);
    // +COPS <AcT> while registered.
    uint8_t (*access_technology)(const struct modem *m);
    // Optional. The link's PPP came up (IPCP open) or went down; NULL lets
    // the engine set `net->ses.bearer` itself. A model whose AT commands
    // (CGACT, NETOPEN) share the bearer decides it here, for the context
    // `link->ppp_cid`, and keeps `net->ses.bearer` true while PPP is up so
    // a lost network reaches its `bearer_lost`.
    void (*link_bearer)(struct modem *m, bool up);
};

// `due_us` is the low 32 bits of the simulation clock; delays are far
// below the 71 minutes it wraps in.
struct modem_event {
    uint32_t due_us;
    uint16_t seq;
    uint8_t kind;
    uint8_t arg;
};

// What the user sets: kept by reset(), restored by reset(factory).
struct modem_settings {
    uint32_t boot_ms;
    uint32_t baud;
    int32_t time[6];
    uint16_t battery_mv;
    uint8_t sim;
    uint8_t registration;
    uint8_t rssi;
    uint8_t battery_percent;
    int8_t timezone;
};

// What the modem observed: kept across power cycles, cleared by reset().
struct modem_stats {
    uint32_t commands;
    uint32_t unknown_commands;
    uint32_t power_cycles;
};

// The command mode is zero, so a cleared channel takes commands.
enum modem_chan_mode {
    MODEM_CHAN_COMMAND,
    MODEM_CHAN_PPP,
    MODEM_CHAN_CLOSED,
};

// One AT interpreter: the raw UART, or one DLCI.
struct modem_chan {
    uint64_t data_deadline_us;
    uint64_t final_due_us;
    const struct modem_phase *phase;
    uint16_t line_len;
    uint16_t data_left;
    uint8_t data_arg;
    uint8_t stash_len;
    uint8_t mode;
    // 0 on the raw UART, the DLCI under CMUX.
    uint8_t dlci;
    bool line_overflow;
    bool swallow_lf;
    bool final_pending;
    bool echo;
    // ATV0: V.250 numeric result codes, `<code>\r`, and information
    // responses without their leading CR LF.
    bool numeric;
    char line[MODEM_LINE_MAX + 1];
    // Input that arrived while a final result was pending or the output
    // FIFO was short of room, replayed once the channel can answer again.
    uint8_t stash[MODEM_STASH];
};

// What the modem holds while powered: cleared by every power transition.
struct modem_session {
    uint32_t pending_baud;
    // The open data phase's argument, while its hooks run.
    uint8_t data_arg;
    uint8_t cmee;
    uint8_t cfun;
    uint8_t creg_n;
    uint8_t cgreg_n;
    uint8_t cereg_n;
    uint8_t reported_stat;
    uint8_t reported_ps_stat;
    bool detached;
    bool pin_entered;
    // The raw UART, and DLCI 1 under CMUX.
    struct modem_chan raw;
};

// What the link observed beyond its parts: kept across power cycles,
// cleared by reset().
struct modem_link_stats {
    uint32_t fc_stalls;
};

// CMUX and PPP, in the state of a model that has them.
struct modem_link {
    // The multiplexer is up.
    bool cmux;
    // The DUT had flow-stopped the PPP DLCI when the link last looked.
    bool stopped;
    // The DLCIs the current output goes to; 0 is the raw UART.
    uint8_t out_mask;
    uint8_t stage_len;
    // ATE and ATV of the raw UART when CMUX started, which every DLCI
    // inherits.
    bool echo;
    bool numeric;
    // AT+CMUX N1.
    uint8_t n1;
    // The channel that carries PPP, or NULL.
    struct modem_chan *ppp_chan;
    // The context the call dialled: <cid> of ATD*99***<cid>#, 1 for ATD*99#.
    uint8_t ppp_cid;
    struct cmux mux;
    struct ppp_settings set;
    struct ppp ppp;
    struct ip ip;
    struct modem_link_stats stats;
    struct modem_chan chans[MODEM_LINK_CHANNELS];
    // Output for the current DLCIs, framed once it holds N1 bytes or the
    // engine moves on.
    uint8_t stage[CMUX_MTU];
};

struct modem {
    struct mclw_instance *inst;
    const struct modem_config *config;
    struct mclw_uart *uart;
    struct mclw_gpio *pwrkey;
    // The shared components the model has, NULL where it has none.
    struct net *net;
    struct sms *sms;
    struct gnss *gnss;
    struct modem_link *link;
    // The channel the engine serves: where a handler's output goes.
    struct modem_chan *chan;
    struct modem_event *events;
    uint64_t key_since_us;
    uint64_t boot_due_us;
    uint8_t event_count;
    uint8_t power;
    bool key_down;
    bool key_used;
    uint16_t event_seq;
    uint16_t out_head;
    uint16_t out_len;
    // Bytes handed to the UART engine since attach, against its bytes_tx.
    uint32_t tx_queued;
    struct modem_settings set;
    struct modem_stats stats;
    struct modem_session ses;
    uint8_t out[MODEM_OUT_SIZE];
};

// The building blocks of a model's tables. Parts go in the same order in
// `config->parts` as their properties and regions in the tables.
#define MODEM_PROP(n, t, c, a, u, lo, hi) \
    { .name = (n), .type = (t), .count = (c), .access = (a), .unit = (u), .min = (lo), .max = (hi) }
#define MODEM_PROP_U32_RO(n) MODEM_PROP(n, MCLW_TYPE_U32, 1, MCLW_ACCESS_RO, NULL, 0, UINT32_MAX)
#define MODEM_PROP_BOOL(n, a) MODEM_PROP(n, MCLW_TYPE_BOOL, 1, a, NULL, 0, 1)

#define MODEM_PROPERTY_COUNT 15
#define MODEM_PROPERTIES \
    MODEM_PROP_BOOL("powered", MCLW_ACCESS_RW), \
    MODEM_PROP("boot_ms", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "ms", 0, MODEM_BOOT_MS_MAX), \
    MODEM_PROP("baud", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "baud", MODEM_BAUD_MIN, MODEM_BAUD_MAX), \
    MODEM_PROP_BOOL("echo", MCLW_ACCESS_RO), \
    MODEM_PROP("functionality", MCLW_TYPE_U32, 1, MCLW_ACCESS_RO, NULL, 0, 7), \
    MODEM_PROP("sim", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 0, 2), \
    MODEM_PROP("registration", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 0, MODEM_REG_ROAMING), \
    MODEM_PROP("rssi", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 0, MODEM_RSSI_UNKNOWN), \
    MODEM_PROP("time", MCLW_TYPE_I32, 6, MCLW_ACCESS_RW, NULL, 0, MODEM_YEAR_MAX), \
    MODEM_PROP("timezone", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "15 min", MODEM_TIMEZONE_MIN, \
        MODEM_TIMEZONE_MAX), \
    MODEM_PROP("battery_mv", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "mV", 0, MODEM_BATTERY_MV_MAX), \
    MODEM_PROP("battery_percent", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "%", 0, 100), \
    MODEM_PROP_U32_RO("commands"), \
    MODEM_PROP_U32_RO("unknown_commands"), \
    MODEM_PROP_U32_RO("power_cycles")

#define NET_PROPERTY_COUNT 9
#define NET_PROPERTIES \
    MODEM_PROP_BOOL("pdp", MCLW_ACCESS_RO), \
    MODEM_PROP("ip", MCLW_TYPE_U32, 4, MCLW_ACCESS_RW, NULL, 0, 255), \
    MODEM_PROP("file_length", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "bytes", 0, NET_FILE_SIZE), \
    MODEM_PROP_U32_RO("connections"), \
    MODEM_PROP_U32_RO("bytes_up"), \
    MODEM_PROP_U32_RO("bytes_down"), \
    MODEM_PROP_U32_RO("drops"), \
    MODEM_PROP_U32_RO("mqtt_publishes"), \
    MODEM_PROP_BOOL("mqtt_send", MCLW_ACCESS_WO)

#define NET_REGION_COUNT 5
#define NET_REGIONS \
    { .name = "hosts", .size = NET_HOSTS_SIZE, .access = MCLW_ACCESS_RW }, \
    { .name = "file", .size = NET_FILE_SIZE, .access = MCLW_ACCESS_RW }, \
    { .name = "request", .size = NET_REQUEST_SIZE, .access = MCLW_ACCESS_RO }, \
    { .name = "mqtt_last", .size = NET_MQTT_LAST_SIZE, .access = MCLW_ACCESS_RO }, \
    { .name = "mqtt_inject", .size = NET_MQTT_INJECT_SIZE, .access = MCLW_ACCESS_RW }

#define GNSS_PROPERTY_COUNT 9
#define GNSS_PROPERTIES \
    MODEM_PROP("latitude", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "1e-6 deg", -GNSS_LATITUDE_MAX, \
        GNSS_LATITUDE_MAX), \
    MODEM_PROP("longitude", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "1e-6 deg", -GNSS_LONGITUDE_MAX, \
        GNSS_LONGITUDE_MAX), \
    MODEM_PROP("altitude", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "mm", -GNSS_ALTITUDE_MAX, \
        GNSS_ALTITUDE_MAX), \
    MODEM_PROP("speed", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "0.01 km/h", 0, GNSS_SPEED_MAX), \
    MODEM_PROP("course", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "0.1 deg", 0, GNSS_COURSE_MAX), \
    MODEM_PROP("hdop", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "0.1", 0, GNSS_HDOP_MAX), \
    MODEM_PROP("satellites", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 0, GNSS_SATELLITES_MAX), \
    MODEM_PROP_BOOL("fix", MCLW_ACCESS_RW), \
    MODEM_PROP_BOOL("gnss_power", MCLW_ACCESS_RO)

// The UART at the part's fixed rate, 8N1, and the optional power pin,
// active low, named `pin`.
#define MODEM_BINDINGS_PIN(baud, pin) \
    { \
        { \
            .name = "uart", \
            .kind = MCLW_BINDING_UART, \
            .required = true, \
            .usb = NULL, \
            .spi = { 0 }, \
            .uart = { \
                .default_baud = (baud), \
                .format = { .data_bits = 8, .stop_bits = 1, .parity = MCLW_UART_PARITY_NONE }, \
            }, \
        }, \
        { \
            .name = (pin), \
            .kind = MCLW_BINDING_GPIO_IN, \
            .required = false, \
            .usb = NULL, \
            .spi = { 0 }, \
            .uart = { 0 }, \
        }, \
    }
#define MODEM_BINDINGS(baud) MODEM_BINDINGS_PIN(baud, "pwrkey")
// For a part with `power_pin` MODEM_PIN_RESET.
#define MODEM_BINDINGS_RESET(baud) MODEM_BINDINGS_PIN(baud, "reset")

// The link part: PPP settings and what CMUX, PPP and TCP observed.
#define PPP_PROPERTY_COUNT 9
#define PPP_PROPERTIES \
    MODEM_PROP("mru", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, "bytes", PPP_MRU_MIN, PPP_MRU_MAX), \
    MODEM_PROP("gw", MCLW_TYPE_U32, 4, MCLW_ACCESS_RW, NULL, 0, 255), \
    MODEM_PROP("dns", MCLW_TYPE_U32, 4, MCLW_ACCESS_RW, NULL, 0, 255), \
    MODEM_PROP_BOOL("cmux", MCLW_ACCESS_RO), \
    MODEM_PROP_BOOL("ppp_up", MCLW_ACCESS_RO), \
    MODEM_PROP_U32_RO("lcp_terminates"), \
    MODEM_PROP_U32_RO("tcp_retransmits"), \
    MODEM_PROP_U32_RO("frames_bad_fcs"), \
    MODEM_PROP_U32_RO("dlci_fc_stalls")

extern const struct modem_part modem_engine_part;
extern const struct modem_part net_part;
extern const struct modem_part gnss_part;
extern const struct modem_part ppp_part;

// The commands of 27.007 and V.250 every model answers, for a model's last
// table: +CGMI, +CGMM, +CGMR, +CGSN, +GSN, +CIMI, +CCID, +CMEE, +IPR,
// +CFUN, +CPIN, +CSQ, +CREG, +CGREG, +CEREG, +CGATT, +COPS, +CBC, +CCLK.
// AT, ATE, ATI, ATV0, ATV1, ATZ, AT&F and AT&W are the engine's own.
extern const struct modem_commands modem_standard_commands;

// The model's ops: its init sets the component pointers (`link` needs
// `net`) and calls modem_init(); every other op is the engine's.
int modem_init(struct modem *m, struct mclw_instance *inst, const struct mclw_binding *bindings,
    struct modem_event *events, uint8_t event_count);
void modem_deinit(struct mclw_instance *inst);
int modem_reset(struct mclw_instance *inst, bool factory);
int modem_get(struct mclw_instance *inst, size_t property, struct mclw_value *out);
int modem_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in);
int modem_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len);
int modem_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len);
void modem_time_advanced(struct mclw_instance *inst, uint64_t now_us);
void modem_poll(struct mclw_instance *inst);

uint64_t modem_now(void);
void modem_trace(struct modem *m, enum mclw_trace_kind kind, uint32_t addr, uint16_t len, int16_t result);
// The first four characters of a name, for a trace address.
uint32_t modem_fourcc(const char *name);

size_t modem_out_free(const struct modem *m);
// Queues output; a write that does not fit is dropped whole and traced.
void modem_out(struct modem *m, const void *data, size_t len);
void modem_out_str(struct modem *m, const char *str);
void modem_out_text(struct modem *m, const struct text *t);
// `\r\n<str>\r\n` whole or not at all: a URC raised outside the queue.
void modem_line(struct modem *m, const char *str);

void modem_ok(struct modem *m);
void modem_error(struct modem *m);
// A V.250 result code: `\r\n<text>\r\n`, or `<code>\r` under ATV0
// (0 OK, 1 CONNECT, 3 NO CARRIER, 4 ERROR).
void modem_result(struct modem *m, uint8_t code, const char *text);
// ERROR with CMEE 0, the code with 1, the text with 2.
void modem_cme(struct modem *m, uint16_t code);
void modem_cms(struct modem *m, uint16_t code);
// `\r\n<text>\r\n`.
void modem_info(struct modem *m, const struct text *t);
void modem_info_str(struct modem *m, const char *str);
// `<text>\r\n`: one line of a list, which opens with a single CR LF.
void modem_info_line(struct modem *m, const struct text *t);
// The information line, then OK.
void modem_answer(struct modem *m, const char *line);
void modem_answer_text(struct modem *m, const struct text *t);
void modem_answer_u32(struct modem *m, const char *prefix, uint32_t v);
// An identity query: the value for the execution form, OK for the test.
void modem_identity(struct modem *m, enum modem_mode mode, const char *value);
// A one-number setting: `prefix` and the value for the read form, one of
// `allowed` for the write.
void modem_small_setting(struct modem *m, enum modem_mode mode, struct modem_args *a, const char *prefix,
    uint8_t *value, const uint8_t *allowed, size_t allowed_count);
void modem_cmd_accept(struct modem *m, enum modem_mode mode, struct modem_args *a);
void modem_cmd_unsupported(struct modem *m, enum modem_mode mode, struct modem_args *a);

// For a model with a link. "+CMUX": AT+CMUX=0[,0[,<speed>[,<N1>...]]]
// answers OK on the raw UART and starts the multiplexer, N1 clamped to
// 127. "D": ATD*99# or ATD*99***<cid># answers CONNECT and turns the
// channel to PPP while the modem is attached, NO CARRIER otherwise; the
// engine routes every `ATD<string>` to a table's "D" with the string as
// the one argument.
void modem_cmd_cmux(struct modem *m, enum modem_mode mode, struct modem_args *a);
void modem_cmd_dial_ppp(struct modem *m, enum modem_mode mode, struct modem_args *a);
// A model's `bearer_lost` hook calls it: the link sends LCP
// Terminate-Request and the channel answers NO CARRIER once it ends.
void modem_link_bearer_lost(struct modem *m);
// The socket carries one of the link's TCP connections; a model with AT
// sockets as well leaves such a socket's notices alone.
bool modem_link_owns_socket(const struct modem *m, uint8_t sock);
// A `net_init` notice handler for a model whose sockets are the link's
// alone: the link polls its sockets and needs no notice.
void modem_link_net_notify(void *ctx, uint8_t sock, enum net_notice notice);

bool modem_arg_int(const struct modem_args *a, uint8_t i, int32_t lo, int32_t hi, int32_t *out);
const char *modem_arg_str(const struct modem_args *a, uint8_t i);

// Sends `\r\n> ` and opens a data phase of `len` bytes (0: uncounted) with
// `arg` in `m->ses.data_arg`; `timeout_ms` 0 never expires.
void modem_prompt(struct modem *m, const struct modem_phase *phase, uint16_t len, uint8_t arg, uint32_t timeout_ms);
void modem_phase_end(struct modem *m);
// The command's OK goes `delay_us` from now; input waits for it.
void modem_defer_ok(struct modem *m, uint32_t delay_us);

// Queues a URC `delay_us` from now, coalesced as the kind says. URCs go in
// time order, each once no command line is half received, no data phase
// is open, no final result is pending and the FIFO has the URC room.
void modem_event(struct modem *m, uint32_t delay_us, uint8_t kind, uint8_t arg);
void modem_event_cancel(struct modem *m, uint8_t kind, uint8_t arg);

void modem_power_on(struct modem *m);
// A normal power down sends what is queued and then `power_down`; a cut
// loses whatever had not left yet.
void modem_power_off(struct modem *m, bool announce);

bool modem_sim_ready(const struct modem *m);
// The MODEM_CPIN_* a boot or a SIM change reports, and its line (NULL for
// no such state).
uint8_t modem_sim_cpin(const struct modem *m);
const char *modem_cpin_line(uint8_t cpin);
// CME 10 or 11 unless the SIM is ready.
bool modem_require_sim(struct modem *m);
// The registration stat the DUT sees: none while off, without a SIM or
// below CFUN 1.
uint8_t modem_stat(const struct modem *m);
// The packet domain's stat, of +CGREG and +CEREG: none after AT+CGATT=0.
uint8_t modem_ps_stat(const struct modem *m);
bool modem_registered(const struct modem *m);
// Registered and not detached with CGATT=0.
bool modem_attached(const struct modem *m);
// Something the registration depends on changed: report a new stat to
// every URC the DUT enabled, and drop the bearer if the network is gone.
void modem_network_changed(struct modem *m);
bool modem_time_valid(const int32_t time[6]);

// Engine internals shared between its sources.
void modem_at_rx(struct modem *m, const uint8_t *bytes, size_t len);
// Input for one channel; returns how much it took, short only when a
// command started CMUX and the rest is multiplexed.
size_t modem_chan_rx(struct modem *m, struct modem_chan *ch, const uint8_t *bytes, size_t len);
void modem_at_tick(struct modem *m, uint64_t now_us);
// Replays the current channel's held input once it can answer.
void modem_at_resume(struct modem *m);
void modem_pump(struct modem *m);
void modem_standard_event(struct modem *m, uint8_t kind, uint8_t arg);
// Makes `ch` the current channel, output to its DLCI.
void modem_select(struct modem *m, struct modem_chan *ch);
// The channels in turn, the raw one first; NULL past the last.
struct modem_chan *modem_chan_at(struct modem *m, size_t i);
// Into the output FIFO as is, whole or not at all.
void modem_fifo_out(struct modem *m, const void *data, size_t len);
void modem_link_init(struct modem *m);
void modem_link_session(struct modem *m);
void modem_link_rx(struct modem *m, const uint8_t *bytes, size_t len);
void modem_link_out(struct modem *m, const void *data, size_t len);
size_t modem_link_out_free(const struct modem *m);
void modem_link_flush(struct modem *m);
// Adds PPP frames while the gate allows, and acts on the link's events.
void modem_link_service(struct modem *m);
void modem_link_tick(struct modem *m, uint64_t now_us);
// The DLCIs of every channel in command mode, for a URC; 0 on the raw UART.
uint8_t modem_link_urc_mask(const struct modem *m);

#endif
