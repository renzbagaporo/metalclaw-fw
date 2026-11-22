// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The SPI target engine. Per target: a data state machine that shifts the
// bits, a monitor state machine on chip select, and five DMA channels. No
// CPU runs at a chip-select edge.
//
//   W, L, D   the response program: W executes control blocks that L loads
//             (a ring of four words onto W's alias 0), and D streams the
//             answer into the TX FIFO. A register map's and a command
//             image's program decode the first bytes of a window and point
//             D at the answer; a stream's W and D just copy.
//   RW, RL    the reset program, the same loader and worker pair. One list
//             per record slot; each list's first block parks RW on the
//             monitor's RX FIFO, which the monitor pushes at every
//             chip-select rise.
//
// At a rise the list restarts the data state machine for the next window,
// pauses and snapshots the response program into its record slot, aborts
// it, arms it again, publishes the slot and loads the next slot's list,
// raising the DMA interrupt. The interrupt hands finished windows on: a
// register map's and a stream's to the model there, a command image's to
// the worker. Each window's MOSI bytes land in its slot's capture buffer,
// so windows reach the model in order however close together they come.
//
// Timing, measured on a 150 MHz RP2350 from the list's first block after
// the token (docs/devices.md): a command image's and a register map's data
// state machine runs again after 36 to 38 cycles, and one DMA transfer more
// for the forced jump that parks it, not measured; a window may start
// clocking from then on; the response program is armed after about 285
// cycles for a register map, 410 for a command image and about 470 for one
// with status opcodes. Its first byte out of the TX FIFO is needed once the
// window's first byte has been clocked, and a register map's descriptor is
// fetched once its command byte is in, so a window whose first byte ends
// sooner than that after the previous rise is answered late.
//
// Channels are stopped the way RP2350-E5 needs: every channel of a chain
// has its enable cleared and its chain pointed at itself before they are
// aborted together; RP2040-E13's spurious completion interrupt is kept off
// by never enabling interrupts on an aborted channel.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/regs/dreq.h"
#include "hardware/structs/dma_debug.h"
#include "pico/time.h"

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "pio_window.h"

#define SPI_PIO pio1
#define SPI_TARGET_COUNT 2u
#define SPI_PROGRAM_LENGTH 32u

#define SPI_ENTRY 0u
#define SPI_ENTRY_WAIT 1u
#define SPI_BYTE 3u
#define SPI_STREAM_WRAP_TARGET 4u
#define SPI_STREAM_WRAP_TOP 9u
#define SPI_REGMAP_WRAP_TARGET 10u
#define SPI_REGMAP_WRAP_TOP 8u
#define SPI_REGMAP_RESTART 12u
#define SPI_REGMAP_ENTRY 14u
#define SPI_REGMAP_WAIT 15u
#define SPI_DUMMY_SLOT 25u
#define SPI_MONITOR_ENTRY 28u
#define SPI_MONITOR_WRAP_TOP 31u

#define SPI_BUFFER_BYTES 264u
#define SPI_TABLE_SLOTS 8u
#define SPI_TABLE_ALIGN 2048u
#define SPI_TABLE_SHIFT 11u
#define SPI_BANK_BYTES ((uint32_t)MCLW_REGMAP_BANK_SIZE)
#define SPI_FRAME_BYTES ((uint32_t)MCLW_STREAM_FRAME_MAX)

#define SPI_REGMAP_CLOCK_HZ 10714285u
#define SPI_STREAM_CLOCK_HZ 15000000u

// Record slots: windows the engine holds for the interrupt to take. The
// interrupt takes them a few microseconds after each window, so a slot is
// reused only after this many more windows.
#define SPI_RECORDS 4u
// The longest list, a command image's with BUSY and status opcodes, is 27
// blocks.
#define SPI_RESET_CBS 28u
#define SPI_REGMAP_CBS 4u
#define SPI_TX_BUFFERS 3u
// Command windows waiting for the worker. A part with BUSY has at most one;
// a part without is polled back to back and has a write enable and the
// erase right behind it.
#define SPI_QUEUE 8u
#define SPI_FORCED 6u
// Longer than any reset program, which runs in about 3 us.
#define SPI_KICK_WAIT_US 20u
#define SPI_LOADER_WAIT_LOOPS 64u

#if PICO_RP2040
#define SPI_COUNT_MASK 0xffffffffu
#else
#define SPI_COUNT_MASK 0x0fffffffu
#endif

#define SPI_DMA_ERROR_BITS (DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS | \
    DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS)

enum spi_image {
    SPI_IMAGE_NONE,
    SPI_IMAGE_REGMAP,
    SPI_IMAGE_STREAM,
    SPI_IMAGE_COMMAND,
};

struct spi_descriptor {
    uint32_t count;
    uint32_t source;
};

// A command image is served by a DMA program, not by the CPU. The data
// state machine shifts every MOSI byte into its RX FIFO and every MISO byte
// out of its TX FIFO. L copies 16-byte control blocks, one at a time, into
// W, which chains back to L when the block is done. The blocks read the
// opcode, offset or address bytes from the RX FIFO and patch them into the
// low bytes of later blocks and of address templates (SRAM honours byte
// writes, DMA registers do not), which is how a table lookup and an
// address composition happen without the CPU. D streams the answer into
// the TX FIFO.
//
//   prologue  1 status byte out; opcode -> &optab[op] -> jump -> L
//   optab[op] picks a list: write, busy write (a jump to the write list,
//             whose head tells the reset program to raise the busy
//             status), response, buffer read, register read, or the
//             memory read's stub, which jumps to the memory list:
//             2 status bytes out, address bits 23..16 -> memory_high,
//             jump into the register list after its status block, which
//             then takes bits 15..8 and 7..0 as a register read takes its
//             two address bytes
//   pagetab   maps address bits 15..8 to the page's address bits 15..8
//   drain     every list ends by jumping to it: the rest of the window's
//             MOSI bytes into the record slot's capture buffer
//
// The window's first MISO byte is the status the reset program left in the
// state machine's OSR; the prologue's status byte is its second.
//
// A chain trigger fires before the last write of the finished block has
// landed (measured on the bench). So the jump to a list writes L's
// READ_ADDR through its trigger alias instead of chaining, and a block
// that W patches is never the very next one L loads: a spacer block sits
// between them, or the patch is written SPI_COMMAND_STEP_REPEAT times so
// the first write has landed. Without these, about one window in a
// thousand loaded a block from a stale address.
//
// An image with immediate responses has no status byte in the prologue:
// each list serves the window's second byte itself, once the opcode is in.
// The prologue's status slot holds a spacer, which leaves its layout as it
// is; with one response the response list is its data, D loaded at once,
// and with more it is the usual lookup, whose first byte is then the
// response's own; the buffer, register and memory lists serve one status
// byte more. A short register read is the register list with no high
// address byte: D's read is page 0's slot, its low byte the one address
// byte.
//
// A response read runs six blocks between the opcode's last bit and its
// first data byte reaching the TX FIFO, which must happen within the one
// byte time the status byte gives: about 600 ns, measured as a clean pass
// at 12.5 MHz and a failure at 15 MHz. That sets the qualified clock.
//
// The structure is one 2 KiB table slot; the register pages are two more,
// adjacent (see command_layout).
struct spi_cb {
    uint32_t read;
    uint32_t write;
    uint32_t count;
    uint32_t ctrl;
};

// DMA AL1 order: CTRL, READ_ADDR, WRITE_ADDR, TRANS_COUNT_TRIG.
struct spi_dblock {
    uint32_t ctrl;
    uint32_t read;
    uint32_t write;
    uint32_t count;
};

enum {
    SPI_LIST_WRITE = 0,
    SPI_LIST_BUSY = 2,
    SPI_LIST_RESPONSE = 3,
    SPI_LIST_BUFFER = 8,
    SPI_LIST_REGISTER = 13,
    SPI_LIST_CBS = 21,
};

// The memory read's stub sits in the buffer list's terminator slot: an
// image with a memory read names it for the buffer read too, so no window
// ever runs the buffer list on.
#define SPI_LIST_MEMORY_STUB 12u
#define SPI_MEMORY_CBS 3u

enum {
    SPI_PROLOGUE_STATUS,
    SPI_PROLOGUE_OPCODE,
    SPI_PROLOGUE_SPACER,
    SPI_PROLOGUE_LOOKUP,
    SPI_PROLOGUE_JUMP,
    SPI_PROLOGUE_CBS,
};

enum {
    SPI_DBLOCK_WRITE,
    SPI_DBLOCK_RESPONSE,
    SPI_DBLOCK_BUFFER,
    SPI_DBLOCK_REGISTER,
    SPI_DBLOCKS,
};

#define SPI_COMMAND_PAGE_SLOTS (2 * MCLW_COMMAND_PAGES_MAX + 2)
#define SPI_COMMAND_NO_OPCODE 0xffu
#define SPI_COMMAND_STATUS_RUN 300u
#define SPI_COMMAND_DATA_RUN 256u
// The bytes a window may run past the drain: more than any window clocks.
#define SPI_SINK_COUNT 0x0fffffffu
// A register or memory read runs on through the page slots after its own,
// as the part's address runs on through its array: far enough to cross
// every held page from the first, and the zero slot after the last. A
// read from a later slot runs on past the slots into the table pool,
// which reads harmlessly.
#define SPI_COMMAND_PAGE_RUN ((MCLW_COMMAND_PAGES_MAX + 1u) * MCLW_COMMAND_PAGE_SIZE)
#define SPI_COMMAND_CLOCK_HZ 8000000u
#define SPI_COMMAND_STEP_REPEAT 4u
// A memory read's first data byte must reach the TX FIFO between the
// sample of its last address bit and the host's sample of the next bit:
// the state machine's autopull stalls the MISO bit until it does. After the
// last address byte the chain runs two blocks and the data channel's first
// transfer, a third of the six blocks the response read was measured to fit
// in 600 ns, so about 200 ns against one SCK period. Not measured on the
// bench: the ceiling below keeps a factor of two on that estimate.
#define SPI_COMMAND_MEMORY_CLOCK_HZ 2000000u
// An immediate image's byte after the opcode must reach the TX FIFO within
// one SCK period of the opcode's last bit, as a memory read's first data
// byte must: the opcode, spacer, lookup and jump blocks and the list's
// first, which is five, or six for the busy and memory lists, against the
// six the response read was measured to run in 600 ns. A 1 MHz period
// leaves under a factor of two on that; to be measured on the bench.
#define SPI_COMMAND_IMMEDIATE_CLOCK_HZ 1000000u
// With several immediate responses the response list looks the opcode up
// first: its three blocks and the opcode copy's repeats come before D's
// first transfer, twice the one-response path. Measured with Zephyr's
// MCP2515 driver and its isotp sample: 500 kHz corrupts responses on a
// 125 MHz RP2040 harness every run, 250 kHz passes. On a 150 MHz RP2350
// harness an mcp2515's READ RX BUFFER and READ STATUS are exact to 2 MHz
// and wrong at 4 MHz: 1 MHz keeps a factor of two.
#if PICO_RP2040
#define SPI_COMMAND_IMMEDIATE_LOOKUP_CLOCK_HZ 250000u
#else
#define SPI_COMMAND_IMMEDIATE_LOOKUP_CLOCK_HZ 1000000u
#endif
// How long a publish waits for an open window's opcode, to learn whether
// the window reads only the status byte: a byte at 250 kHz.
#define SPI_COMMAND_OPCODE_WAIT_US 32u

struct spi_command_block {
    uint8_t optab[256];
    uint8_t resptab[256];
    uint8_t pagetab[256];
    // List heads must sit within 256 bytes of `lists`: the jump template
    // takes the head's low byte from optab.
    struct spi_cb lists[SPI_LIST_CBS];
    struct spi_cb prologue[SPI_PROLOGUE_CBS];
    struct spi_dblock dblock[SPI_DBLOCKS];
    uint32_t jump;
    uint32_t scratch;
    uint32_t memory_jump;
    uint32_t register_jump;
    uint32_t memory_high;
    uint32_t write_jump;
    uint32_t drain_jump;
    uint32_t unused;
    uint8_t responses[MCLW_COMMAND_RESPONSES_MAX][MCLW_COMMAND_RESPONSE_MAX];
    // The memory list's own blocks; the loader reaches them by a jump.
    struct spi_cb memory[SPI_MEMORY_CBS];
    struct spi_cb drain;
    // A window longer than the drain: its other MOSI bytes go to one word,
    // so the RX FIFO never fills and stalls the state machine, which would
    // stop a long read's MISO bytes too. The zeroes after it are a null
    // trigger that stops W, which it never reaches within a window.
    struct spi_cb sink;
    struct spi_cb stop;
    uint32_t sink_word;
    uint8_t unused2[0x600 - 0x5e4];
    // Two copies, so a read from any offset runs on without wrapping.
    uint8_t buffer[2 * MCLW_COMMAND_BUFFER_SIZE];
};

_Static_assert(sizeof(struct spi_command_block) == 2048u, "command block is one table slot");
_Static_assert(offsetof(struct spi_command_block, lists) % 256u == 0u, "list base is page aligned");
_Static_assert(offsetof(struct spi_command_block, buffer) % 256u == 0u, "buffer is page aligned");
_Static_assert(SPI_LIST_REGISTER * sizeof(struct spi_cb) < 255u, "list heads fit a byte");
_Static_assert(SPI_LIST_MEMORY_STUB * sizeof(struct spi_cb) < 255u, "list heads fit a byte");
_Static_assert(offsetof(struct spi_command_block, responses) == 0x500u, "responses are page aligned");
_Static_assert(offsetof(struct spi_command_block, drain) == 0x5b0u, "drain follows the memory list");
_Static_assert(offsetof(struct spi_command_block, sink) == 0x5c0u, "the sink follows the drain");

// What the reset program copies of a command window: from the opcode
// patched into the prologue's lookup to the memory read's high address.
#define SPI_HDR_START offsetof(struct spi_command_block, prologue[SPI_PROLOGUE_LOOKUP])
#define SPI_HDR_END (offsetof(struct spi_command_block, memory_high) + 4u)
#define SPI_HDR_WORDS ((SPI_HDR_END - SPI_HDR_START) / 4u)

// A command image's status, one 256-byte page per target, addressed by a
// list head's byte offset: the status the engine serves sits at the busy
// write list's offset, so the reset program raises it with one byte copy
// to page + head that lands harmlessly in this page for every other head.
//
//   [4] the model's status   [5] the same with the busy bits, then 5
//   [7] an opcode that changes no status bits
//   [32] served             [33] 4 or 5: which of the two the reset
//                                program serves after a window that set
//                                no busy bits
//   [64] a status opcode's pair, [4] and [5] after its window, two bytes
//        each
//   [512] a status opcode's pair's offset by opcode, 4 for any other
//
// At the release of a window that began with a status opcode the reset
// program copies its pair over [4] and [5] before it picks what to serve,
// so the next window reads the change, and a busy window right after it
// raises the busy bits on it. The CPU keeps every pair at what the window
// would leave, from the model's status and the windows it has yet to take,
// or at [4] and [5] unchanged while the part ignores such commands.
#define SPI_STATUS_MODEL 4u
#define SPI_STATUS_BUSY 5u
#define SPI_STATUS_NEUTRAL 7u
#define SPI_STATUS_SERVED (SPI_LIST_BUSY * sizeof(struct spi_cb))
#define SPI_STATUS_SELECT (SPI_STATUS_SERVED + 1u)
#define SPI_STATUS_PAIRS 64u
#define SPI_STATUS_TABLE 512u
#define SPI_STATUS_PAGE 768u

_Static_assert(SPI_STATUS_BUSY == SPI_STATUS_MODEL + 1u, "a pair is one halfword");
_Static_assert(SPI_STATUS_PAIRS >= SPI_LIST_RESPONSE * sizeof(struct spi_cb) + 2u &&
    SPI_STATUS_PAIRS + 2u * MCLW_COMMAND_STATUS_OPCODES_MAX <= SPI_LIST_BUFFER * sizeof(struct spi_cb),
    "the pairs sit between two list heads' offsets");
_Static_assert(SPI_STATUS_SERVED == 32u, "the busy head addresses the served status");

// `token` is what the monitor pushed: 0 at a chip-select rise, all ones
// when the CPU asked for a re-arm with no window to record (see kick).
struct spi_record {
    uint32_t token;
    uint32_t w_count;
    uint32_t loader;
    uint32_t hdr[SPI_HDR_WORDS];
    uint32_t reghi;
    uint8_t capture[SPI_BUFFER_BYTES];
};

// Everything the reset program reads or writes, per target.
struct spi_engine {
    struct spi_cb list[SPI_RECORDS][SPI_RESET_CBS];
    struct spi_record rec[SPI_RECORDS];
    struct spi_cb regmap[SPI_REGMAP_CBS];
    uint32_t scratch[2];
    volatile uint32_t head;
    uint32_t off_w;
    uint32_t off_l;
    uint32_t abort_d;
    uint32_t zero;
    uint32_t abort_mask;
    uint32_t fjoin[2];
    uint32_t instr[SPI_FORCED];
    uint32_t forced;
    uint32_t no_jump;
    volatile uint32_t bank_base;
    uint32_t l_cfg[4];
    volatile uint32_t d_cfg[4];
    uint32_t next_list[SPI_RECORDS];
    uint32_t head_value[SPI_RECORDS];
    uint32_t next_capture[SPI_RECORDS];
    uint32_t w_cfg[SPI_RECORDS][4];
};

enum {
    SPI_REGMAP_COMMAND,
    SPI_REGMAP_SPACER,
    SPI_REGMAP_DESCRIPTOR,
    SPI_REGMAP_CAPTURE,
};

struct spi_queued {
    uint16_t len;
    bool busy;
    bool forced;
    uint8_t bytes[MCLW_COMMAND_WINDOW_MAX + 1u];
};

struct spi_bus {
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
    uint8_t open_count;

    uint16_t program_words[SPI_PROGRAM_LENGTH];
    pio_program_t program;
    bool program_loaded;
    uint8_t program_offset;
    uint8_t program_sck;
    uint8_t program_class;
    uint16_t program_dummy;

    bool dma_installed;
    bool poll_registered;

    uint8_t table_owner[SPI_TABLE_SLOTS];
};

struct mclw_spi_target {
    struct spi_bus *bus;
    const void *owner;
    uint8_t cs;
    uint8_t mode;
    bool msb_first;
    uint8_t sm;
    uint8_t monitor_sm;
    int8_t tx_dma;
    int8_t rx_dma;
    int8_t loader_dma;
    int8_t rw_dma;
    int8_t rl_dma;

    enum spi_image image;
    struct mclw_regmap *map;
    struct mclw_regmap_framing framing;
    struct mclw_stream *stream;
    struct mclw_command_image *command;
    struct spi_engine *engine;
    uint8_t *status;
    int8_t busy_pin;
    volatile bool busy;
    // The model lowered BUSY while a release's record was still on its way
    // or windows were queued: BUSY stays high until none is.
    volatile bool busy_lower_pending;
    volatile bool shadow_pending;
    bool busy_image;
    uint8_t busy_windows;
    // The image has status opcodes, and a window taken since the status
    // pairs were last written may change them.
    bool status_image;
    volatile bool status_stale;
    // The model's status as it last published it, and whether the
    // delivered window is with the model still.
    uint8_t model_status;
    volatile bool delivering;
    // A window that set the busy bits was dropped, or its record lost, so
    // nothing will be delivered to lower them: the poll lowers them.
    volatile bool busy_orphaned;
    // The status in Y, the first byte of every window.
    uint8_t primed;
    uint8_t page_slot[MCLW_COMMAND_PAGES_MAX];
    struct mclw_work record_work;

    uint8_t tail;
    bool skip_first;
    bool running;
    uint8_t stuck_polls;
    uint8_t queue_first;
    volatile uint8_t queue_count;
    struct spi_queued queue[SPI_QUEUE];
    struct spi_queued incoming;
    struct spi_queued delivered;

    uint8_t banks;
    uint8_t table[MCLW_REGMAP_BANKS_MAX];
    uint8_t bank;

    uint8_t tx_buf[SPI_TX_BUFFERS][SPI_BUFFER_BYTES];

    struct mclw_spi_target_stats stats;
};

static inline __attribute__((always_inline)) void sm_enable(uint sm, bool enabled) {
    if (enabled) {
        hw_set_bits(&SPI_PIO->ctrl, 1u << sm);
    } else {
        hw_clear_bits(&SPI_PIO->ctrl, 1u << sm);
    }
}

static struct spi_bus spi_bus;
static struct mclw_spi_target targets[SPI_TARGET_COUNT];
static struct spi_engine engines[SPI_TARGET_COUNT] __attribute__((aligned(16)));
static uint8_t status_pages[SPI_TARGET_COUNT][SPI_STATUS_PAGE] __attribute__((aligned(256)));

_Static_assert(SPI_STATUS_PAGE % 256u == 0u, "every target's page is aligned");

union spi_table {
    struct spi_descriptor descriptors[MCLW_REGMAP_BANK_SIZE];
    struct spi_command_block command;
};

_Static_assert(SPI_COMMAND_PAGE_SLOTS * MCLW_COMMAND_PAGE_SIZE == 2 * sizeof(union spi_table),
    "the page slots fill two table slots");

static union spi_table table_pool[SPI_TABLE_SLOTS] __attribute__((aligned(SPI_TABLE_ALIGN)));

static const uint8_t wait_slots[] = { 4u, 5u, 18u, 19u };

// pico-sdk's dma_channel_get_default_config and gpio_get are static inline,
// but at -Os GCC emits one out-of-line copy of each, in flash, and the
// engine's RAM functions call it there. These are the same, forced inline.
static __force_inline dma_channel_config default_config(uint channel) {
    dma_channel_config c = { 0 };
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_FORCE);
    channel_config_set_chain_to(&c, channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_ring(&c, false, 0);
    channel_config_set_bswap(&c, false);
    channel_config_set_irq_quiet(&c, false);
    channel_config_set_enable(&c, true);
    channel_config_set_sniff_enable(&c, false);
    channel_config_set_high_priority(&c, false);
    return c;
}

static __force_inline bool pin_high(uint pin) {
    return gpio_get(pin);
}

static uint8_t __not_in_flash_func(reverse_byte)(uint8_t value) {
    uint32_t v = value;

    v = ((v & 0x55u) << 1) | ((v >> 1) & 0x55u);
    v = ((v & 0x33u) << 2) | ((v >> 2) & 0x33u);
    v = ((v & 0x0fu) << 4) | ((v >> 4) & 0x0fu);
    return (uint8_t)v;
}

static uint8_t mode_class(uint8_t mode) {
    return (mode == 1u || mode == 2u) ? 1u : 0u;
}

static uint8_t __not_in_flash_func(wire_byte)(const struct mclw_spi_target *t, uint8_t value) {
    return t->msb_first ? reverse_byte(value) : value;
}

static uint32_t address_of(const volatile void *p) {
    return (uint32_t)(uintptr_t)p;
}

// A JMP, PULL or OUT addresses the program's own instructions; the reset
// program writes these into the data state machine's INSTR register.
static uint16_t jmp_to(const struct spi_bus *bus, uint8_t target) {
    return (uint16_t)pio_encode_jmp(bus->program_offset + target);
}

// The data program (0..27) and the monitor (28..31), one block for both.
//
//   0..2    a window's start: the first MISO bit, then wait for chip select
//           low and drive MISO. The reset program enters here with the OSR
//           empty, so the OUT's autopull waits for the first byte: the
//           status it preloads into the TX FIFO, or D's first frame byte.
//           Waiting in the program rather than in a forced PULL keeps the
//           word's arrival from racing the reset program's next blocks.
//   3..9    the bit loop, wrapped at 9 for a stream or command image;
//           the sample edge's IN autopushes and the OUT after it
//           autopulls, so MISO changes right after the sampling edge,
//           which gives the host half a period of setup in every mode.
//   10..11  a register map's wrap: one manual push per byte.
//   12..13  a register map's restart: Y from the word the reset program
//           preloads, the active bank's descriptor table.
//   14..27  a register map's start: the command byte composed with Y into
//           its descriptor's address and pushed, the read dummy byte from
//           the state machine, then the bit loop.
//   28..31  the monitor, on chip select: at each rise it raises BUSY (its
//           SET pin, if it has one) and pushes the token the reset program
//           waits on.
static void encode_program(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    uint16_t *p = bus->program_words;
    bool lead = cls != 0u;

    // A program's WAIT GPIO holds the pin's low five bits; pio_add_program
    // rebases it to the block's GPIO base, program_patch does the same.
    sck = (uint8_t)(sck % 32u);

    p[0] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[1] = (uint16_t)pio_encode_jmp_pin(SPI_ENTRY_WAIT);
    p[2] = (uint16_t)pio_encode_set(pio_pindirs, 1u);
    p[3] = (uint16_t)pio_encode_set(pio_x, 7u);
    p[4] = (uint16_t)pio_encode_wait_gpio(lead, sck);
    p[5] = (uint16_t)pio_encode_wait_gpio(!lead, sck);
    p[6] = (uint16_t)pio_encode_in(pio_pins, 1u);
    p[7] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[8] = (uint16_t)pio_encode_jmp_x_dec(4u);
    p[9] = (uint16_t)pio_encode_set(pio_x, 7u);
    p[10] = (uint16_t)pio_encode_push(false, false);
    p[11] = (uint16_t)pio_encode_jmp(SPI_BYTE);
    p[12] = (uint16_t)pio_encode_pull(false, true);
    p[13] = (uint16_t)pio_encode_out(pio_y, 32u);
    p[14] = (uint16_t)pio_encode_pull(false, false);
    p[15] = (uint16_t)pio_encode_jmp_pin(SPI_REGMAP_WAIT);
    p[16] = (uint16_t)pio_encode_in(pio_y, 21u);
    p[17] = (uint16_t)pio_encode_set(pio_pindirs, 1u);
    p[18] = (uint16_t)pio_encode_wait_gpio(lead, sck);
    p[19] = (uint16_t)pio_encode_wait_gpio(!lead, sck);
    p[20] = (uint16_t)pio_encode_in(pio_pins, 1u);
    p[21] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[22] = (uint16_t)pio_encode_jmp_not_osre(18u);
    p[23] = (uint16_t)pio_encode_in(pio_null, 3u);
    p[24] = (uint16_t)pio_encode_push(false, true);
    p[25] = bus->program_dummy;
    p[26] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[27] = (uint16_t)pio_encode_jmp(SPI_BYTE);
    // A monitor with no SET pin configured skips the SET.
    p[28] = (uint16_t)pio_encode_wait_pin(false, 0u);
    p[29] = (uint16_t)pio_encode_wait_pin(true, 0u);
    p[30] = (uint16_t)pio_encode_set(pio_pins, 1u);
    p[31] = (uint16_t)pio_encode_push(false, false);

    bus->program.instructions = bus->program_words;
    bus->program.length = (uint8_t)SPI_PROGRAM_LENGTH;
    bus->program.origin = -1;
    bus->program.pio_version = 0;
}

static uint16_t dummy_instruction(const struct mclw_regmap_framing *framing, uint8_t filler) {
    if (framing->read_dummy_bytes == 0u) {
        return (uint16_t)pio_encode_nop();
    }
    return filler == 0u ? (uint16_t)pio_encode_mov(pio_osr, pio_null)
        : (uint16_t)pio_encode_mov_not(pio_osr, pio_null);
}

static void program_patch_dummy(struct spi_bus *bus, uint16_t instruction) {
    bus->program_dummy = instruction;
    bus->program_words[SPI_DUMMY_SLOT] = instruction;
    SPI_PIO->instr_mem[bus->program_offset + SPI_DUMMY_SLOT] = instruction;
}

static void program_patch(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    encode_program(bus, sck, cls);
    for (size_t i = 0; i < sizeof(wait_slots); i++) {
        uint8_t slot = wait_slots[i];

        SPI_PIO->instr_mem[bus->program_offset + slot] =
            bus->program_words[slot] ^ (uint16_t)pio_get_gpio_base(SPI_PIO);
    }
    bus->program_sck = sck;
    bus->program_class = cls;
}

static int bus_load_program(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    if (bus->program_loaded) {
        return 0;
    }
    bus->program_dummy = (uint16_t)pio_encode_nop();
    encode_program(bus, sck, cls);
    if (!pio_can_add_program(SPI_PIO, &bus->program)) {
        return -ENOSPC;
    }
    bus->program_offset = (uint8_t)pio_add_program(SPI_PIO, &bus->program);
    bus->program_sck = sck;
    bus->program_class = cls;
    bus->program_loaded = true;
    return 0;
}

static void bus_unload_program(struct spi_bus *bus) {
    if (!bus->program_loaded) {
        return;
    }
    pio_remove_program(SPI_PIO, &bus->program, bus->program_offset);
    bus->program_loaded = false;
}

static __force_inline uint32_t channel_bit(int8_t channel) {
    return channel >= 0 ? 1u << channel : 0u;
}

static bool __not_in_flash_func(channel_has)(int8_t channel, uint32_t bits) {
    return channel >= 0 && (dma_channel_hw_addr((uint)channel)->ctrl_trig & bits) != 0u;
}

// Clears a channel's enable and points its chain at itself, so that
// neither a pending chain trigger nor its own completion starts anything.
static void __not_in_flash_func(disable_channel)(int8_t channel) {
    if (channel >= 0) {
        hw_write_masked(&dma_channel_hw_addr((uint)channel)->al1_ctrl,
            ((uint32_t)channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
            DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS | DMA_CH0_CTRL_TRIG_EN_BITS);
    }
}

static void __not_in_flash_func(clear_errors)(int8_t channel) {
    if (channel >= 0) {
        hw_set_bits(&dma_channel_hw_addr((uint)channel)->al1_ctrl,
            DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
    }
}

// The target's channels trigger one another (W and L, RW and RL), and the
// reset program loads the response channels' control registers. Aborting
// them one at a time let one restart another; RP2350-E5 has an aborted
// channel trigger the channel it chains to unless that one's enable is
// already clear (pico-sdk dma_channel_abort). So all are disabled, then
// aborted together, and the pair is repeated in case a load in flight
// re-enabled one before the abort retired it.
static void __not_in_flash_func(stop_channels)(const struct mclw_spi_target *t) {
    const int8_t channels[] = { t->rl_dma, t->rw_dma, t->loader_dma, t->rx_dma, t->tx_dma };
    uint32_t mask = 0;

    for (size_t c = 0; c < sizeof(channels); c++) {
        mask |= channel_bit(channels[c]);
        if (channels[c] >= 0) {
            dma_channel_set_irq1_enabled((uint)channels[c], false);
        }
    }
    for (uint32_t i = 0; i < 4u; i++) {
        bool settled = true;

        for (size_t c = 0; c < sizeof(channels); c++) {
            disable_channel(channels[c]);
        }
        dma_hw->abort = mask;
        // RP2040-E13: ABORT can clear while a transfer is still in flight;
        // BUSY stays set until it has retired.
        for (uint32_t spin = 0; spin < 256u; spin++) {
            bool busy = false;

            for (size_t c = 0; c < sizeof(channels); c++) {
                busy = busy || channel_has(channels[c], DMA_CH0_CTRL_TRIG_BUSY_BITS);
            }
            if (!busy) {
                break;
            }
        }
        for (size_t c = 0; c < sizeof(channels); c++) {
            if (channel_has(channels[c], DMA_CH0_CTRL_TRIG_EN_BITS | DMA_CH0_CTRL_TRIG_BUSY_BITS)) {
                settled = false;
            }
        }
        if (settled) {
            break;
        }
    }
    for (size_t c = 0; c < sizeof(channels); c++) {
        clear_errors(channels[c]);
    }
    dma_hw->intr = mask;
}

static void __not_in_flash_func(release_miso)(const struct mclw_spi_target *t) {
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_set(pio_pindirs, 0u));
}

// Stops the engine and leaves the data state machine disabled with empty
// FIFOs and MISO undriven.
static void __not_in_flash_func(engine_stop)(struct mclw_spi_target *t) {
    sm_enable(t->sm, false);
    stop_channels(t);
    pio_sm_clear_fifos(SPI_PIO, t->sm);
    pio_sm_clear_fifos(SPI_PIO, t->monitor_sm);
    release_miso(t);
    t->running = false;
}

static void configure_sm(struct mclw_spi_target *t, enum spi_image image) {
    const struct spi_bus *bus = t->bus;
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_out_pins(&c, bus->miso, 1u);
    sm_config_set_set_pins(&c, bus->miso, 1u);
    sm_config_set_in_pins(&c, bus->mosi);
    sm_config_set_jmp_pin(&c, t->cs);
    if (image == SPI_IMAGE_REGMAP) {
        sm_config_set_out_shift(&c, false, true, 8u);
        sm_config_set_in_shift(&c, false, false, 32u);
        sm_config_set_wrap(&c, bus->program_offset + SPI_REGMAP_WRAP_TARGET,
            bus->program_offset + SPI_REGMAP_WRAP_TOP);
    } else {
        // A command image is MSB first in the image; a stream frame is
        // bit-reversed into tx_buf instead.
        sm_config_set_out_shift(&c, image != SPI_IMAGE_COMMAND, true, 8u);
        sm_config_set_in_shift(&c, false, true, 8u);
        sm_config_set_wrap(&c, bus->program_offset + SPI_STREAM_WRAP_TARGET,
            bus->program_offset + SPI_STREAM_WRAP_TOP);
    }
    pio_sm_init(SPI_PIO, t->sm, bus->program_offset + SPI_ENTRY, &c);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, bus->miso, 1u, false);
}

static volatile void *__not_in_flash_func(tx_fifo_msb)(const struct mclw_spi_target *t) {
    return (volatile uint8_t *)&SPI_PIO->txf[t->sm] + 3;
}

static uint32_t channel_ctrl(uint channel, uint chain_to, enum dma_channel_transfer_size size, bool read_increment,
    bool write_increment, uint dreq) {
    dma_channel_config c = default_config(channel);

    channel_config_set_transfer_data_size(&c, size);
    channel_config_set_read_increment(&c, read_increment);
    channel_config_set_write_increment(&c, write_increment);
    channel_config_set_dreq(&c, dreq);
    channel_config_set_chain_to(&c, chain_to);
    channel_config_set_high_priority(&c, true);
    channel_config_set_irq_quiet(&c, true);
    return channel_config_get_ctrl_value(&c);
}

static uint32_t channel_off(uint channel) {
    return channel_ctrl(channel, channel, DMA_SIZE_32, false, false, DREQ_FORCE) & ~DMA_CH0_CTRL_TRIG_EN_BITS;
}

static struct spi_command_block *command_block(const struct mclw_spi_target *t) {
    return &table_pool[t->table[0]].command;
}

static uint8_t (*command_pages(const struct mclw_spi_target *t))[MCLW_COMMAND_PAGE_SIZE] {
    return (uint8_t (*)[MCLW_COMMAND_PAGE_SIZE])(void *)&table_pool[t->table[1]];
}

static __force_inline uint32_t live_count(uint32_t count) {
    return count & SPI_COUNT_MASK;
}

// Whether the reset program is parked on the monitor's FIFO with no rise
// waiting for it: nothing of the engine's is moving but the response
// program, which waits for the window.
static bool __not_in_flash_func(reset_idle)(const struct mclw_spi_target *t) {
    const dma_channel_hw_t *rw = dma_channel_hw_addr((uint)t->rw_dma);

    return t->running && (rw->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u &&
        rw->read_addr == address_of(&SPI_PIO->rxf[t->monitor_sm]) &&
        pio_sm_is_rx_fifo_empty(SPI_PIO, t->monitor_sm);
}

static bool __not_in_flash_func(data_parked)(const struct mclw_spi_target *t, uint8_t at) {
    return (uint8_t)(pio_sm_get_pc(SPI_PIO, t->sm) - t->bus->program_offset) == at && pin_high(t->cs);
}

// The reset program for one record slot. Every block chains to RL, which
// loads the next; the last one jumps RL to the next slot's list and raises
// the DMA interrupt.
struct list_builder {
    struct spi_cb *cb;
    uint32_t n;
    uint rw;
    uint rl;
};

static uint32_t emit(struct list_builder *b, const volatile void *read, const volatile void *write, uint32_t count,
    enum dma_channel_transfer_size size, bool read_increment, bool write_increment) {
    b->cb[b->n] = (struct spi_cb){ address_of(read), address_of(write), count,
        channel_ctrl(b->rw, b->rl, size, read_increment, write_increment, DREQ_FORCE) };
    return b->n++;
}

static void emit_word(struct list_builder *b, const volatile void *read, const volatile void *write) {
    (void)emit(b, read, write, 1u, DMA_SIZE_32, false, false);
}

// The common front of every list: wait for the rise, then start the data
// state machine on the next window before anything else, with forced
// instructions that park it, clear the ISR, set the OSR and jump to the
// image's start (none of them stalls). A command image's first byte is Y,
// which holds its status, and a register map's is don't-care, so neither
// depends on the FIFOs: they restart in the list's second block, and D is
// aborted and the FIFOs cleared after, before the first byte has been
// clocked and the OSR wants the second. A stream's first byte comes from the TX FIFO,
// so there D is stopped and the FIFOs cleared first (D is stalled on its
// DREQ there and chains to nothing, the case an abort is clean in) and the
// start waits in the program for D's first byte, with the OSR emptied (with
// autopull on, a PULL is a fence that does nothing while the OSR holds a
// byte). The state machine runs throughout. Then the rest of the response
// program is paused, snapshotted into `r` and aborted.
static void emit_front(struct list_builder *b, struct mclw_spi_target *t, struct spi_record *r, bool first,
    bool loader) {
    struct spi_engine *e = t->engine;

    b->cb[b->n++] = (struct spi_cb){ address_of(&SPI_PIO->rxf[t->monitor_sm]), address_of(&e->scratch[0]), 1u,
        channel_ctrl(b->rw, b->rl, DMA_SIZE_32, false, false, pio_get_dreq(SPI_PIO, t->monitor_sm, false)) };
    if (first) {
        (void)emit(b, e->instr, &SPI_PIO->sm[t->sm].instr, e->forced, DMA_SIZE_32, true, false);
    }
    emit_word(b, &e->abort_d, &dma_hw->abort);
    // Changing FJOIN_RX flushes both FIFOs; plain writes of the known value
    // leave the join as configured whatever the register held.
    (void)emit(b, e->fjoin, &SPI_PIO->sm[t->sm].shiftctrl, 2u, DMA_SIZE_32, true, false);
    if (!first) {
        (void)emit(b, e->instr, &SPI_PIO->sm[t->sm].instr, e->forced, DMA_SIZE_32, true, false);
    }
    emit_word(b, &e->off_w, &dma_hw->ch[t->rx_dma].al1_ctrl);
    if (loader) {
        emit_word(b, &e->off_l, &dma_hw->ch[t->loader_dma].al1_ctrl);
    }
    emit_word(b, &dma_hw->ch[t->rx_dma].transfer_count, &r->w_count);
    if (loader) {
        emit_word(b, &dma_hw->ch[t->loader_dma].read_addr, &r->loader);
    }
    emit_word(b, &e->abort_mask, &dma_hw->abort);
    emit_word(b, &e->scratch[0], &r->token);
    // The FIFO clear above left both channels counting DREQ credits for
    // words and slots that are gone; restarting the handshake recounts them.
    emit_word(b, &e->zero, &dma_debug_hw->ch[t->rx_dma].dbg_ctdreq);
    emit_word(b, &e->zero, &dma_debug_hw->ch[t->tx_dma].dbg_ctdreq);
}

// The forced instructions that start the next window at `entry` with the
// OSR full of `osr`, or empty. The state machine runs its own program in
// any cycle between two of them (RP2040 datasheet 3.5.7), so the first
// jumps it to `wait`, the start's loop on chip select high, which changes
// nothing. Left where the window ended, an OUT stalled on an empty OSR (at
// the arm, or after D ran dry) would shift a bit out of the OSR the next
// one sets, and the next window would take its command byte in seven bits. The
// DMA can write every cycle (2.5), leaving no such cycle; Renode's DMA
// writes every other one.
static void forced_start(struct mclw_spi_target *t, uint8_t wait, uint8_t entry, enum pio_src_dest osr,
    bool empty) {
    struct spi_engine *e = t->engine;
    uint32_t n = 0;

    e->instr[n++] = jmp_to(t->bus, wait);
    e->instr[n++] = (uint16_t)pio_encode_mov(pio_isr, pio_null);
    e->instr[n++] = (uint16_t)pio_encode_mov(pio_osr, osr);
    if (empty) {
        e->instr[n++] = (uint16_t)pio_encode_out(pio_null, 32u);
    }
    e->instr[n++] = (uint16_t)pio_encode_set(pio_pindirs, 0u);
    e->instr[n++] = jmp_to(t->bus, entry);
    e->forced = n;
}

static void emit_back(struct list_builder *b, struct mclw_spi_target *t, uint32_t slot) {
    struct spi_engine *e = t->engine;

    b->cb[b->n++] = (struct spi_cb){ address_of(&e->next_list[slot]),
        address_of(&dma_hw->ch[t->rl_dma].al3_read_addr_trig), 1u,
        (channel_ctrl(b->rw, b->rw, DMA_SIZE_32, false, false, DREQ_FORCE) & ~DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS) };
}

static void engine_common(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;

    e->off_w = channel_off((uint)t->rx_dma);
    e->off_l = channel_off((uint)t->loader_dma);
    e->abort_d = channel_bit(t->tx_dma);
    e->zero = 0;
    e->abort_mask = channel_bit(t->rx_dma) | channel_bit(t->loader_dma) | channel_bit(t->tx_dma);
    e->fjoin[1] = SPI_PIO->sm[t->sm].shiftctrl &
        ~(PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS | PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS);
    e->fjoin[0] = e->fjoin[1] | PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS;
    e->head = 0;
    for (uint32_t i = 0; i < SPI_RECORDS; i++) {
        uint32_t next = (i + 1u) % SPI_RECORDS;

        e->next_list[i] = address_of(e->list[next]);
        e->head_value[i] = next;
        e->next_capture[i] = address_of(e->rec[next].capture);
    }
    memset(e->list, 0, sizeof(e->list));
    t->tail = 0;
    t->skip_first = true;
    t->stuck_polls = 0;
}

static void command_lists(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;
    struct spi_command_block *b = command_block(t);
    uint8_t *s = t->status;

    forced_start(t, SPI_ENTRY_WAIT, SPI_ENTRY, pio_y, false);
    e->no_jump = address_of(b->lists) | SPI_COMMAND_NO_OPCODE;
    {
        dma_channel_config c = default_config((uint)t->loader_dma);

        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, 4u);
        channel_config_set_high_priority(&c, true);
        channel_config_set_irq_quiet(&c, true);
        e->l_cfg[0] = address_of(b->prologue);
        e->l_cfg[1] = address_of(&dma_hw->ch[t->rx_dma].read_addr);
        e->l_cfg[2] = 4u;
        e->l_cfg[3] = channel_config_get_ctrl_value(&c);
    }
    for (uint32_t i = 0; i < SPI_RECORDS; i++) {
        struct list_builder lb = { e->list[i], 0, (uint)t->rw_dma, (uint)t->rl_dma };
        struct spi_record *r = &e->rec[i];
        uint32_t patch_jump = 0;
        uint32_t patch_select = 0;
        uint32_t select;

        emit_front(&lb, t, r, true, true);
        (void)emit(&lb, &b->prologue[SPI_PROLOGUE_LOOKUP], r->hdr, SPI_HDR_WORDS, DMA_SIZE_32, true, true);
        emit_word(&lb, &b->lists[SPI_LIST_REGISTER + 3].read, &r->reghi);
        if (t->status_image) {
            // The pair the window's opcode names over [4] and [5], or [4]
            // and [5] onto themselves for any other; then the lookup's
            // opcode back to one that names none, so that a release with no
            // opcode in its window (a bare chip-select pulse) changes
            // nothing. Before the head moves: a window the CPU has taken
            // has had its pair applied.
            uint32_t pick = emit(&lb, &b->prologue[SPI_PROLOGUE_LOOKUP].read, NULL, 1u, DMA_SIZE_8, false, false);
            uint32_t apply = emit(&lb, &s[SPI_STATUS_TABLE], NULL, 1u, DMA_SIZE_8, false, false);

            lb.cb[pick].write = address_of(&lb.cb[pick + 1u].read);
            lb.cb[apply].write = address_of(&lb.cb[apply + 1u].read);
            (void)emit(&lb, &s[SPI_STATUS_MODEL], &s[SPI_STATUS_MODEL], 1u, DMA_SIZE_16, false, false);
            (void)emit(&lb, &s[SPI_STATUS_NEUTRAL], &b->prologue[SPI_PROLOGUE_LOOKUP].read, 1u, DMA_SIZE_8,
                false, false);
        }
        if (t->busy_image) {
            patch_jump = emit(&lb, &b->jump, NULL, 1u, DMA_SIZE_8, false, false);
        }
        emit_word(&lb, &e->no_jump, &b->jump);
        emit_word(&lb, &e->next_capture[i], &b->drain.write);
        if (t->busy_image) {
            patch_select = emit(&lb, &s[SPI_STATUS_SELECT], NULL, 1u, DMA_SIZE_8, false, false);
        }
        emit_word(&lb, &e->head_value[i], &e->head);
        // The status the next window serves: the model's, or the busy one
        // while a window that set the busy bits waits for the model; a
        // window that just set them sets them again, whatever the choice.
        select = emit(&lb, &s[t->busy_image ? 0u : SPI_STATUS_MODEL], &s[SPI_STATUS_SERVED], 1u, DMA_SIZE_8,
            false, false);
        if (t->busy_image) {
            uint32_t force = emit(&lb, &s[SPI_STATUS_BUSY], s, 2u, DMA_SIZE_8, true, true);

            lb.cb[patch_jump].write = address_of(&lb.cb[force].write);
            lb.cb[patch_select].write = address_of(&lb.cb[select].read);
        }
        (void)emit(&lb, e->l_cfg, &dma_hw->ch[t->loader_dma].read_addr, 4u, DMA_SIZE_32, true, true);
        emit_back(&lb, t, i);
    }
}

static void regmap_lists(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;
    struct spi_cb *p = e->regmap;
    uint W = (uint)t->rx_dma;
    uint L = (uint)t->loader_dma;
    uint rx_dreq = pio_get_dreq(SPI_PIO, t->sm, false);

    forced_start(t, SPI_REGMAP_WAIT, SPI_REGMAP_ENTRY, pio_null, false);
    // The command byte's word is its descriptor's address; the descriptor
    // goes into D's (TRANS_COUNT, READ_ADDR_TRIG), which starts the answer.
    p[SPI_REGMAP_COMMAND] = (struct spi_cb){ address_of(&SPI_PIO->rxf[t->sm]),
        address_of(&p[SPI_REGMAP_DESCRIPTOR].read), 1u, channel_ctrl(W, L, DMA_SIZE_32, false, false, rx_dreq) };
    p[SPI_REGMAP_SPACER] = (struct spi_cb){ address_of(&e->scratch[0]), address_of(&e->scratch[1]), 1u,
        channel_ctrl(W, L, DMA_SIZE_32, false, false, DREQ_FORCE) };
    p[SPI_REGMAP_DESCRIPTOR] = (struct spi_cb){ 0u, address_of(&dma_hw->ch[t->tx_dma].al3_transfer_count), 2u,
        channel_ctrl(W, L, DMA_SIZE_32, true, true, DREQ_FORCE) };
    p[SPI_REGMAP_CAPTURE] = (struct spi_cb){ address_of(&SPI_PIO->rxf[t->sm]), address_of(e->rec[0].capture),
        SPI_BUFFER_BYTES, channel_ctrl(W, W, DMA_SIZE_8, false, true, rx_dreq) };
    {
        dma_channel_config c = default_config(L);

        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, 4u);
        channel_config_set_high_priority(&c, true);
        channel_config_set_irq_quiet(&c, true);
        e->l_cfg[0] = address_of(p);
        e->l_cfg[1] = address_of(&dma_hw->ch[W].read_addr);
        e->l_cfg[2] = 4u;
        e->l_cfg[3] = channel_config_get_ctrl_value(&c);
    }
    e->d_cfg[0] = channel_ctrl((uint)t->tx_dma, (uint)t->tx_dma, DMA_SIZE_8, t->framing.auto_increment, false,
        pio_get_dreq(SPI_PIO, t->sm, true));
    e->d_cfg[1] = address_of(t->tx_buf[0]);
    e->d_cfg[2] = address_of(tx_fifo_msb(t));
    for (uint32_t i = 0; i < SPI_RECORDS; i++) {
        struct list_builder lb = { e->list[i], 0, (uint)t->rw_dma, (uint)t->rl_dma };
        struct spi_record *r = &e->rec[i];

        emit_front(&lb, t, r, true, true);
        emit_word(&lb, &p[SPI_REGMAP_DESCRIPTOR].read, &r->hdr[0]);
        emit_word(&lb, &e->head_value[i], &e->head);
        emit_word(&lb, &e->next_capture[i], &p[SPI_REGMAP_CAPTURE].write);
        (void)emit(&lb, e->d_cfg, &dma_hw->ch[t->tx_dma].al1_ctrl, 3u, DMA_SIZE_32, true, true);
        (void)emit(&lb, e->l_cfg, &dma_hw->ch[L].read_addr, 4u, DMA_SIZE_32, true, true);
        emit_back(&lb, t, i);
    }
}

static void stream_lists(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;
    uint rx_dreq = pio_get_dreq(SPI_PIO, t->sm, false);

    forced_start(t, SPI_ENTRY_WAIT, SPI_ENTRY, pio_null, true);
    e->abort_mask = channel_bit(t->rx_dma) | channel_bit(t->tx_dma);
    e->d_cfg[1] = address_of(&SPI_PIO->txf[t->sm]);
    e->d_cfg[2] = SPI_BUFFER_BYTES;
    e->d_cfg[3] = channel_ctrl((uint)t->tx_dma, (uint)t->tx_dma, DMA_SIZE_8, true, false,
        pio_get_dreq(SPI_PIO, t->sm, true));
    for (uint32_t i = 0; i < SPI_RECORDS; i++) {
        struct list_builder lb = { e->list[i], 0, (uint)t->rw_dma, (uint)t->rl_dma };

        e->w_cfg[i][0] = address_of(&SPI_PIO->rxf[t->sm]);
        e->w_cfg[i][1] = e->next_capture[i];
        e->w_cfg[i][2] = SPI_BUFFER_BYTES;
        e->w_cfg[i][3] = channel_ctrl((uint)t->rx_dma, (uint)t->rx_dma, DMA_SIZE_8, false, true, rx_dreq);
        emit_front(&lb, t, &e->rec[i], false, false);
        emit_word(&lb, &e->head_value[i], &e->head);
        (void)emit(&lb, e->d_cfg, &dma_hw->ch[t->tx_dma].read_addr, 4u, DMA_SIZE_32, true, true);
        (void)emit(&lb, e->w_cfg[i], &dma_hw->ch[t->rx_dma].read_addr, 4u, DMA_SIZE_32, true, true);
        emit_back(&lb, t, i);
    }
}

// Y of a disabled data state machine, which keeps it across windows.
static void load_y(const struct mclw_spi_target *t, uint32_t value) {
    pio_sm_put(SPI_PIO, t->sm, value);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_pull(false, true));
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_mov(pio_y, pio_osr));
}

// Starts the reset program and hands it the first window: a chip-select
// rise the monitor is made to report if chip select is high now, else the
// next real one. Either way the first record describes no window.
//
// The monitor stays stopped until RW is parked on its FIFO, so that every
// token it pushes runs the reset program at its rise. RW counts the
// monitor's DREQ while disarmed too, a credit for each token pushed since
// (4 with the FIFO full, measured), and clearing the FIFO leaves the
// credits without their tokens (RP2350 datasheet 12.6.4.2: a FIFO the CPU
// touches desynchronises its channel); a rise that came before RW was
// parked would be taken once it was, microseconds late. Either way the
// reset program ran at no rise, restarting the data state machine inside
// a window a host had begun, which lost its first bits: seen arming a w25q
// while pico-examples' spi_flash polled it back to back.
static void engine_start(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;
    dma_channel_config c = default_config((uint)t->rl_dma);

    sm_enable(t->monitor_sm, false);
    pio_sm_clear_fifos(SPI_PIO, t->monitor_sm);
    dma_debug_hw->ch[t->rw_dma].dbg_ctdreq = 0;
    // The data state machine waits at its first OUT for a TX word that
    // only the reset program provides, after it has stopped it.
    pio_sm_restart(SPI_PIO, t->sm);
    pio_sm_exec(SPI_PIO, t->sm, jmp_to(t->bus, SPI_ENTRY));
    sm_enable(t->sm, true);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 4u);
    channel_config_set_high_priority(&c, true);
    channel_config_set_irq_quiet(&c, true);
    __compiler_memory_barrier();
    dma_irqn_acknowledge_channel(1u, (uint)t->rw_dma);
    dma_channel_set_irq1_enabled((uint)t->rw_dma, true);
    dma_channel_configure((uint)t->rl_dma, &c, &dma_hw->ch[t->rw_dma].read_addr, e->list[0], 4u, true);
    t->running = true;
    for (uint32_t i = 0; i < SPI_LOADER_WAIT_LOOPS && !reset_idle(t); i++) {
        tight_loop_contents();
    }
    pio_sm_restart(SPI_PIO, t->monitor_sm);
    pio_sm_exec(SPI_PIO, t->monitor_sm, jmp_to(t->bus, SPI_MONITOR_ENTRY));
    sm_enable(t->monitor_sm, true);
    if (pin_high(t->cs)) {
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_push(false, false));
    }
}

// The engine's copy of what the model published, but for the status, which
// the status page holds. Called with no window reading what it copies.
// The image's on_release over the served pages, for one window.
static void __not_in_flash_func(command_release)(struct mclw_spi_target *t, const struct spi_queued *q) {
    const struct mclw_command_image *image = t->command;
    uint8_t *pages[MCLW_COMMAND_PAGES_MAX];

    if (image->on_release == NULL || q->len == 0u) {
        return;
    }
    for (uint8_t i = 0; i < image->page_count; i++) {
        pages[i] = command_pages(t)[t->page_slot[i]];
    }
    image->on_release(q->bytes, q->len, pages, image->on_release_ctx);
}

static uint8_t __not_in_flash_func(command_shadow)(struct mclw_spi_target *t) {
    struct mclw_command_image *image = t->command;
    struct spi_command_block *b = command_block(t);
    uint8_t dirty = image->dirty;

    image->dirty = 0;
    if ((dirty & MCLW_COMMAND_DIRTY_RESPONSES) != 0u) {
        for (uint8_t i = 0; i < image->response_count; i++) {
            memcpy(b->responses[i], image->responses[i].bytes, MCLW_COMMAND_RESPONSE_MAX);
        }
    }
    if ((dirty & MCLW_COMMAND_DIRTY_PAGES) != 0u) {
        for (uint8_t i = 0; i < image->page_count; i++) {
            memcpy(command_pages(t)[t->page_slot[i]], image->pages[i], MCLW_COMMAND_PAGE_SIZE);
        }
        // The model published what it has taken; the windows still waiting
        // for it have their effect again.
        for (uint32_t k = 0; k < t->queue_count; k++) {
            command_release(t, &t->queue[(t->queue_first + k) % SPI_QUEUE]);
        }
    }
    if ((dirty & MCLW_COMMAND_DIRTY_BUFFER) != 0u) {
        memcpy(b->buffer, image->buffer, MCLW_COMMAND_BUFFER_SIZE);
        memcpy(b->buffer + MCLW_COMMAND_BUFFER_SIZE, image->buffer, MCLW_COMMAND_BUFFER_SIZE);
    }
    t->shadow_pending = false;
    return dirty;
}

static void __not_in_flash_func(status_consider)(const struct mclw_command_image *image,
    const struct spi_queued *q, uint8_t *status, bool *held) {
    if (*held || q->len == 0u) {
        return;
    }
    if (q->forced || mclw_command_status_held(image, *status)) {
        *held = true;
        return;
    }
    *status = mclw_command_status_after(image, *status, q->bytes[0]);
}

// The status the part has once the model has taken every window the engine
// holds for it: the model's own, changed by each status opcode among them,
// up to one the part would ignore. `held` says the part ignores the next.
static uint8_t __not_in_flash_func(command_projected)(const struct mclw_spi_target *t, bool *held) {
    const struct mclw_command_image *image = t->command;
    uint8_t status = t->model_status;

    *held = false;
    if (t->delivering) {
        status_consider(image, &t->delivered, &status, held);
    }
    for (uint32_t k = 0; k < t->queue_count; k++) {
        status_consider(image, &t->queue[(t->queue_first + k) % SPI_QUEUE], &status, held);
    }
    *held = *held || mclw_command_status_held(image, status);
    return status;
}

static void __not_in_flash_func(status_pair)(uint8_t *s, uint32_t at, const struct mclw_command_image *image,
    uint8_t status) {
    uint8_t busy = (uint8_t)((status | image->busy_status) & (uint8_t)~image->ready_status);

    *(volatile uint16_t *)(void *)&s[at] = (uint16_t)(status | (uint16_t)busy << 8);
}

// [4] and [5], and every status opcode's pair, from the model's status and
// the windows it has yet to take. Called in the critical section.
static void __not_in_flash_func(command_status_publish)(struct mclw_spi_target *t) {
    const struct mclw_command_image *image = t->command;
    uint8_t *s = t->status;
    bool held = false;
    uint8_t status = t->status_image ? command_projected(t, &held) : t->model_status;

    t->status_stale = false;
    for (uint8_t i = 0; t->status_image && i < image->status_opcode_count; i++) {
        uint8_t after = held ? status : mclw_command_status_after(image, status, image->status_opcodes[i].opcode);

        status_pair(s, SPI_STATUS_PAIRS + 2u * i, image, after);
    }
    status_pair(s, SPI_STATUS_MODEL, image, status);
}

// A register read runs on from its page's slot into the next one, as the
// part's address runs on into the next page, so the slot after a page must
// hold the page after it. Held pages sit in runs of consecutive numbers,
// each run after a zero slot and the last one before another. An unheld
// page maps to the zero slot before the run its successor starts, or else
// to slot 0, which zero slot 1 follows. Page numbers are address bits
// 15..8, so a run may wrap from FFh to 00h, as the part's address crosses
// a 64 KiB boundary: the layout starts at a held page whose predecessor is
// not held.
static void command_layout(struct mclw_spi_target *t, uint8_t *pagetab) {
    const struct mclw_command_image *image = t->command;
    uint8_t (*pages)[MCLW_COMMAND_PAGE_SIZE] = command_pages(t);
    uint32_t held[256u / 32u];
    uint32_t start = 0;
    uint8_t next = 1;
    bool in_run = false;

    memset(pagetab, (int)((address_of(pages[0]) >> 8) & 0xffu), 256u);
#define HELD(page) ((held[(page) / 32u] >> ((page) % 32u)) & 1u)
    memset(held, 0, sizeof(held));
    for (uint8_t i = 0; i < image->page_count; i++) {
        held[image->page_number[i] / 32u] |= 1u << (image->page_number[i] % 32u);
    }
    for (uint32_t page = 0; page < 256u; page++) {
        if (HELD(page) && !HELD((page - 1u) & 0xffu)) {
            start = page;
            break;
        }
    }
    for (uint32_t k = 0; k < 256u; k++) {
        uint32_t page = (start + k) & 0xffu;

        if (!HELD(page)) {
            in_run = false;
            continue;
        }
        if (!in_run) {
            pagetab[(page - 1u) & 0xffu] = (uint8_t)((address_of(pages[next]) >> 8) & 0xffu);
            next++;
            in_run = true;
        }
        for (uint8_t i = 0; i < image->page_count; i++) {
            if (image->page_number[i] == page) {
                t->page_slot[i] = next;
            }
        }
        pagetab[page] = (uint8_t)((address_of(pages[next]) >> 8) & 0xffu);
        next++;
    }
#undef HELD
}

static void command_build(struct mclw_spi_target *t) {
    struct mclw_command_image *image = t->command;
    struct spi_command_block *b = command_block(t);
    struct spi_cb *l = b->lists;
    struct spi_cb *p = b->prologue;
    uint8_t (*pages)[MCLW_COMMAND_PAGE_SIZE] = command_pages(t);
    uint8_t *s = t->status;
    uint L = (uint)t->loader_dma;
    uint W = (uint)t->rx_dma;
    uint D = (uint)t->tx_dma;
    uint rx_dreq = pio_get_dreq(SPI_PIO, t->sm, false);
    uint tx_dreq = pio_get_dreq(SPI_PIO, t->sm, true);
    uint32_t rxf = address_of(&SPI_PIO->rxf[t->sm]);
    uint32_t txf = address_of(tx_fifo_msb(t));
    uint32_t d_al1 = address_of(&dma_hw->ch[D].al1_ctrl);
    uint32_t jump_l = address_of(&dma_hw->ch[L].al3_read_addr_trig);
    uint32_t status = address_of(&s[SPI_STATUS_SERVED]);
    uint32_t rx_byte = channel_ctrl(W, L, DMA_SIZE_8, false, false, rx_dreq);
    uint32_t tx_status = channel_ctrl(W, L, DMA_SIZE_8, false, false, tx_dreq);
    uint32_t copy_byte = channel_ctrl(W, L, DMA_SIZE_8, false, false, DREQ_FORCE);
    uint32_t copy_word = channel_ctrl(W, L, DMA_SIZE_32, false, false, DREQ_FORCE);
    uint32_t jump = channel_ctrl(W, W, DMA_SIZE_32, false, false, DREQ_FORCE);
    uint32_t load_d = channel_ctrl(W, L, DMA_SIZE_32, true, true, DREQ_FORCE);
    uint32_t drain = channel_ctrl(W, L, DMA_SIZE_8, false, true, rx_dreq);
    uint32_t page_high = address_of(pages) & 0xffff0000u;

    memset(b, 0, sizeof(*b));
    memset(pages, image->filler, SPI_COMMAND_PAGE_SLOTS * MCLW_COMMAND_PAGE_SIZE);
    memset(s, 0, SPI_STATUS_PAGE);
    t->busy_image = false;
    t->status_image = image->status_opcode_count != 0u;
    t->status_stale = false;
    t->delivering = false;
    memset(&s[SPI_STATUS_TABLE], SPI_STATUS_MODEL, 256u);
    for (uint8_t i = 0; i < image->status_opcode_count; i++) {
        s[SPI_STATUS_TABLE + image->status_opcodes[i].opcode] = (uint8_t)(SPI_STATUS_PAIRS + 2u * i);
    }
    for (uint32_t op = 0; op < 256u; op++) {
        if (s[SPI_STATUS_TABLE + op] == SPI_STATUS_MODEL) {
            s[SPI_STATUS_NEUTRAL] = (uint8_t)op;
            break;
        }
    }

    memset(b->optab, (int)(SPI_LIST_WRITE * sizeof(struct spi_cb)), sizeof(b->optab));
    if (image->busy_status != 0u || image->ready_status != 0u) {
        for (uint32_t op = 0; op < 256u; op++) {
            if (mclw_command_busy_opcode(image, (uint8_t)op)) {
                b->optab[op] = (uint8_t)(SPI_LIST_BUSY * sizeof(struct spi_cb));
                t->busy_image = true;
            }
        }
    }
    for (uint8_t i = 0; i < image->response_count; i++) {
        b->optab[image->responses[i].opcode] = (uint8_t)(SPI_LIST_RESPONSE * sizeof(struct spi_cb));
        b->resptab[image->responses[i].opcode] = (uint8_t)(i * MCLW_COMMAND_RESPONSE_MAX);
    }
    b->optab[image->read_buffer_opcode] = (uint8_t)(SPI_LIST_BUFFER * sizeof(struct spi_cb));
    b->optab[image->read_register_opcode] = (uint8_t)(SPI_LIST_REGISTER * sizeof(struct spi_cb));
    if (image->has_memory_read) {
        b->optab[image->read_memory_opcode] = (uint8_t)(SPI_LIST_MEMORY_STUB * sizeof(struct spi_cb));
    }
    command_layout(t, b->pagetab);

    b->dblock[SPI_DBLOCK_WRITE] = (struct spi_dblock){
        channel_ctrl(D, D, DMA_SIZE_8, false, false, tx_dreq), status, txf, SPI_COMMAND_STATUS_RUN };
    b->dblock[SPI_DBLOCK_RESPONSE] = (struct spi_dblock){
        channel_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), address_of(b->responses), txf,
        MCLW_COMMAND_RESPONSE_MAX };
    b->dblock[SPI_DBLOCK_BUFFER] = (struct spi_dblock){
        channel_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), address_of(b->buffer), txf, SPI_COMMAND_DATA_RUN };
    b->dblock[SPI_DBLOCK_REGISTER] = (struct spi_dblock){
        channel_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), page_high, txf, SPI_COMMAND_PAGE_RUN };
    b->jump = address_of(b->lists) | SPI_COMMAND_NO_OPCODE;
    b->write_jump = address_of(&l[SPI_LIST_WRITE]);
    b->drain_jump = address_of(&b->drain);

    struct spi_cb spacer = { address_of(&b->jump), address_of(&b->scratch), 1u, copy_word };
    struct spi_cb to_drain = { address_of(&b->drain_jump), jump_l, 1u, jump };
    uint32_t opcode = address_of(&p[SPI_PROLOGUE_LOOKUP].read);

    b->drain = (struct spi_cb){ rxf, address_of(t->engine->rec[0].capture), MCLW_COMMAND_WINDOW_MAX, drain };
    b->sink = (struct spi_cb){ rxf, address_of(&b->sink_word), SPI_SINK_COUNT,
        channel_ctrl(W, L, DMA_SIZE_8, false, false, rx_dreq) };

    bool immediate = image->immediate_response;
    // The status bytes a list serves before its data, the one the prologue
    // serves aside.
    uint32_t ahead = immediate ? 1u : 0u;

    p[SPI_PROLOGUE_STATUS] = immediate ? spacer : (struct spi_cb){ status, txf, 1u, tx_status };
    p[SPI_PROLOGUE_OPCODE] = (struct spi_cb){ rxf, opcode, 1u, rx_byte };
    p[SPI_PROLOGUE_SPACER] = spacer;
    p[SPI_PROLOGUE_LOOKUP] = (struct spi_cb){
        address_of(b->optab), address_of(&b->jump), 1u, copy_byte };
    p[SPI_PROLOGUE_JUMP] = (struct spi_cb){
        address_of(&b->jump), jump_l, 1u, jump };

    l[SPI_LIST_WRITE + 0] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_WRITE]), d_al1, 4u, load_d };
    l[SPI_LIST_WRITE + 1] = to_drain;
    l[SPI_LIST_BUSY] = (struct spi_cb){ address_of(&b->write_jump), jump_l, 1u, jump };

    if (immediate && image->response_count <= 1u) {
        // D's read stays at the first response, which is the only one.
        l[SPI_LIST_RESPONSE + 0] = (struct spi_cb){
            address_of(&b->dblock[SPI_DBLOCK_RESPONSE]), d_al1, 4u, load_d };
        l[SPI_LIST_RESPONSE + 1] = to_drain;
    } else {
        l[SPI_LIST_RESPONSE + 0] = (struct spi_cb){
            opcode, address_of(&l[SPI_LIST_RESPONSE + 1].read), SPI_COMMAND_STEP_REPEAT, copy_byte };
        l[SPI_LIST_RESPONSE + 1] = (struct spi_cb){
            address_of(b->resptab), address_of(&b->dblock[SPI_DBLOCK_RESPONSE].read), 1u, copy_byte };
        l[SPI_LIST_RESPONSE + 2] = (struct spi_cb){
            address_of(&b->dblock[SPI_DBLOCK_RESPONSE]), d_al1, 4u, load_d };
        l[SPI_LIST_RESPONSE + 3] = to_drain;
    }

    l[SPI_LIST_BUFFER + 0] = (struct spi_cb){ status, txf, 1u + ahead, tx_status };
    l[SPI_LIST_BUFFER + 1] = (struct spi_cb){ rxf, address_of(&b->dblock[SPI_DBLOCK_BUFFER].read), 1u, rx_byte };
    l[SPI_LIST_BUFFER + 2] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_BUFFER]), d_al1, 4u, load_d };
    l[SPI_LIST_BUFFER + 3] = to_drain;

    if (image->short_register_read) {
        // The byte during the address, then the registers from it.
        b->dblock[SPI_DBLOCK_REGISTER].read = page_high | ((uint32_t)b->pagetab[0] << 8);
        l[SPI_LIST_REGISTER + 0] = (struct spi_cb){ status, txf, 1u, tx_status };
        l[SPI_LIST_REGISTER + 1] = (struct spi_cb){
            rxf, address_of(&b->dblock[SPI_DBLOCK_REGISTER].read), 1u, rx_byte };
        l[SPI_LIST_REGISTER + 2] = (struct spi_cb){
            address_of(&b->dblock[SPI_DBLOCK_REGISTER]), d_al1, 4u, load_d };
        l[SPI_LIST_REGISTER + 3] = to_drain;
    } else {
        l[SPI_LIST_REGISTER + 0] = (struct spi_cb){ status, txf, 2u + ahead, tx_status };
        l[SPI_LIST_REGISTER + 1] = (struct spi_cb){
            rxf, address_of(&l[SPI_LIST_REGISTER + 3].read), 1u, rx_byte };
        l[SPI_LIST_REGISTER + 2] = spacer;
        l[SPI_LIST_REGISTER + 3] = (struct spi_cb){
            address_of(b->pagetab), address_of(&b->dblock[SPI_DBLOCK_REGISTER].read) + 1u, 1u, copy_byte };
        l[SPI_LIST_REGISTER + 4] = (struct spi_cb){
            rxf, address_of(&b->dblock[SPI_DBLOCK_REGISTER].read), 1u, rx_byte };
        l[SPI_LIST_REGISTER + 5] = (struct spi_cb){
            address_of(&b->dblock[SPI_DBLOCK_REGISTER]), d_al1, 4u, load_d };
        l[SPI_LIST_REGISTER + 6] = to_drain;
    }

    if (image->has_memory_read) {
        b->memory_jump = address_of(b->memory);
        b->register_jump = address_of(&l[SPI_LIST_REGISTER + 1]);
        l[SPI_LIST_MEMORY_STUB] = (struct spi_cb){ address_of(&b->memory_jump), jump_l, 1u, jump };
        b->memory[0] = (struct spi_cb){ status, txf, 2u + ahead, tx_status };
        b->memory[1] = (struct spi_cb){ rxf, address_of(&b->memory_high), 1u, rx_byte };
        b->memory[2] = (struct spi_cb){ address_of(&b->register_jump), jump_l, 1u, jump };
    }

    image->dirty = 0xffu;
    (void)command_shadow(t);
    t->model_status = image->status;
    command_status_publish(t);
    s[SPI_STATUS_SERVED] = s[SPI_STATUS_MODEL];
    s[SPI_STATUS_SELECT] = SPI_STATUS_MODEL;
    s[SPI_STATUS_BUSY + 1u] = SPI_STATUS_BUSY;
}

static uint8_t hdr_byte(const struct spi_record *r, size_t offset) {
    return ((const uint8_t *)r->hdr)[offset - SPI_HDR_START];
}

#define HDR_BYTE(r, field) hdr_byte((r), offsetof(struct spi_command_block, field))

// How far the loader got into the list at `head`: the blocks it fetched,
// or UINT32_MAX once it has jumped to the drain, which every list ends in.
static uint32_t command_progress(const struct mclw_spi_target *t, const struct spi_record *r, uint32_t head) {
    const struct spi_command_block *b = command_block(t);
    uint32_t start = address_of(&b->lists[head]);
    uint32_t drain = address_of(&b->drain);

    if (r->loader > drain) {
        return UINT32_MAX;
    }
    return r->loader > start ? (r->loader - start) / sizeof(struct spi_cb) : 0u;
}

// The address bytes a memory read received: bits 23..16 once the loader has
// gone past the memory list's second block, then 15..8 and 7..0 as the
// register list it jumped into counts them.
static uint16_t command_memory_record(const struct mclw_spi_target *t, const struct spi_record *r, uint8_t *record,
    uint16_t len) {
    const struct spi_command_block *b = command_block(t);
    uint32_t at = r->loader;
    uint32_t memory = address_of(b->memory);
    uint32_t reg = address_of(&b->lists[SPI_LIST_REGISTER]);
    uint32_t reg_end = address_of(&b->lists[SPI_LIST_CBS]);
    uint32_t loaded;

    if (at > memory && at <= memory + SPI_MEMORY_CBS * sizeof(struct spi_cb)) {
        if ((at - memory) / sizeof(struct spi_cb) >= SPI_MEMORY_CBS) {
            record[len++] = HDR_BYTE(r, memory_high);
        }
        return len;
    }
    if (at > address_of(&b->drain)) {
        loaded = 6u;
    } else if (at <= reg || at > reg_end) {
        return len;
    } else {
        loaded = (at - reg) / sizeof(struct spi_cb);
    }
    record[len++] = HDR_BYTE(r, memory_high);
    if (loaded >= 3u) {
        record[len++] = (uint8_t)r->reghi;
        if (loaded >= 6u) {
            record[len++] = HDR_BYTE(r, dblock[SPI_DBLOCK_REGISTER].read);
        }
    }
    return len;
}

static __force_inline struct spi_queued *queued(struct mclw_spi_target *t, uint32_t k) {
    return &t->queue[(t->queue_first + k) % SPI_QUEUE];
}

// Only the bytes a window carried are copied: a host polling a full queue
// back to back would otherwise keep the core copying whole entries, and
// SRAM traffic slows the reset program.
static void __not_in_flash_func(copy_queued)(struct spi_queued *dst, const struct spi_queued *src) {
    dst->len = src->len;
    dst->busy = src->busy;
    dst->forced = src->forced;
    memcpy(dst->bytes, src->bytes, src->len);
}

static bool __not_in_flash_func(same_window)(const struct spi_queued *a, const struct spi_queued *b) {
    return !b->forced && b->len == a->len && b->busy == a->busy && memcmp(b->bytes, a->bytes, b->len) == 0;
}

// A full queue for a part without BUSY is a host polling faster than the
// worker delivers. The latest queued window that only repeats the one
// before it makes room, so no window that differs from its predecessor,
// and none that set the busy bits, is lost to the backlog.
static bool __not_in_flash_func(command_make_room)(struct mclw_spi_target *t) {
    for (uint32_t k = t->queue_count - 1u; k >= 1u; k--) {
        if (!same_window(queued(t, k - 1u), queued(t, k))) {
            continue;
        }
        for (uint32_t j = k; j + 1u < t->queue_count; j++) {
            copy_queued(queued(t, j), queued(t, j + 1u));
        }
        t->queue_count--;
        return true;
    }
    return false;
}

// A finished command window, from its record into the worker's queue.
// Called in the critical section.
static void __not_in_flash_func(command_take)(struct mclw_spi_target *t, const struct spi_record *r) {
    const struct spi_command_block *b = command_block(t);
    uint8_t list = HDR_BYTE(r, jump);
    bool window_busy = t->busy;
    bool forced = false;
    uint16_t len = 0;
    struct spi_queued *q;

    t->busy = t->busy || t->busy_pin >= 0;
    // With BUSY, the window before is still waiting for the worker, BUSY
    // high since its release: the part ignores this one, and the one it was
    // busy with is kept for delivery. Without, the worker is behind by
    // more windows than the engine keeps.
    if (t->busy_pin >= 0 ? (window_busy || t->queue_count != 0u) : t->queue_count >= SPI_QUEUE) {
        t->stats.stale_windows++;
    }
    if (t->busy_pin >= 0 && t->queue_count != 0u) {
        return;
    }
    q = &t->incoming;
    if (list == SPI_COMMAND_NO_OPCODE) {
        t->stats.short_windows++;
    } else {
        uint32_t head = list / sizeof(struct spi_cb);
        uint32_t loaded = command_progress(t, r, head == SPI_LIST_BUSY ? SPI_LIST_WRITE : head);

        q->bytes[len++] = HDR_BYTE(r, prologue[SPI_PROLOGUE_LOOKUP].read);
        if (t->status_image) {
            t->status_stale = true;
        }
        if (head == SPI_LIST_BUSY) {
            forced = true;
            t->busy_windows++;
        }
        if (head == SPI_LIST_WRITE || head == SPI_LIST_BUSY) {
            uint32_t params = 0;

            if (r->loader == address_of(&b->drain) + sizeof(struct spi_cb)) {
                uint32_t remaining = live_count(r->w_count);

                params = remaining <= MCLW_COMMAND_WINDOW_MAX ? MCLW_COMMAND_WINDOW_MAX - remaining : 0u;
            } else if (r->loader > address_of(&b->drain) + sizeof(struct spi_cb)) {
                params = MCLW_COMMAND_WINDOW_MAX;
                t->stats.long_windows++;
            }
            memcpy(&q->bytes[len], r->capture, params);
            len = (uint16_t)(len + params);
        } else if (head == SPI_LIST_BUFFER && loaded >= 3u) {
            q->bytes[len++] = HDR_BYTE(r, dblock[SPI_DBLOCK_BUFFER].read);
        } else if (head == SPI_LIST_MEMORY_STUB) {
            len = command_memory_record(t, r, q->bytes, len);
        } else if (head == SPI_LIST_REGISTER && t->command->short_register_read) {
            // The address is in once D's load was fetched.
            if (loaded >= 3u) {
                q->bytes[len++] = HDR_BYTE(r, dblock[SPI_DBLOCK_REGISTER].read);
            }
        } else if (head == SPI_LIST_REGISTER && loaded >= 3u) {
            q->bytes[len++] = (uint8_t)r->reghi;
            if (loaded >= 6u) {
                q->bytes[len++] = HDR_BYTE(r, dblock[SPI_DBLOCK_REGISTER].read);
            }
        }
        t->stats.transactions++;
        t->stats.bytes += len;
        if (!forced && mclw_command_quiet_opcode(t->command, q->bytes[0])) {
            return;
        }
    }
    q->len = len;
    q->busy = window_busy;
    q->forced = forced;
    if (t->queue_count >= SPI_QUEUE) {
        // A poll that repeats the last queued one goes, or else a queued
        // repeat makes room; a window that set the busy bits is never the
        // one dropped while there is a repeat to drop instead.
        bool repeat = same_window(queued(t, t->queue_count - 1u), q);

        if (repeat || !command_make_room(t)) {
            if (forced && t->busy_windows != 0u) {
                t->busy_windows--;
            }
            if (forced) {
                t->busy_orphaned = true;
            }
            t->stats.missed_windows++;
            if (!repeat) {
                t->stats.lost_windows++;
            }
            return;
        }
        t->stats.missed_windows++;
    }
    copy_queued(queued(t, t->queue_count), q);
    t->queue_count++;
    command_release(t, q);
    mclw_hal_work_post(&t->record_work);
}

static bool __not_in_flash_func(regmap_command)(const struct mclw_spi_target *t, uint32_t descriptor,
    uint8_t *command) {
    for (uint8_t bank = 0; bank < t->banks; bank++) {
        uint32_t base = address_of(table_pool[t->table[bank]].descriptors);

        if (descriptor >= base && descriptor < base + SPI_BANK_BYTES * sizeof(struct spi_descriptor) &&
            (descriptor - base) % sizeof(struct spi_descriptor) == 0u) {
            *command = (uint8_t)((descriptor - base) / sizeof(struct spi_descriptor));
            return true;
        }
    }
    return false;
}

static void __not_in_flash_func(regmap_take)(struct mclw_spi_target *t, const struct spi_record *r) {
    struct mclw_regmap *map = t->map;
    const struct spi_cb *p = t->engine->regmap;
    uint32_t payload = 0;
    uint32_t dummies = t->framing.read_dummy_bytes;
    uint32_t address;
    uint8_t command = 0;

    if (r->loader <= address_of(&p[SPI_REGMAP_COMMAND + 1])) {
        t->stats.short_windows++;
        return;
    }
    if (!regmap_command(t, r->hdr[0], &command)) {
        t->stats.dma_faults++;
        t->stats.short_windows++;
        return;
    }
    if (r->loader == address_of(&p[SPI_REGMAP_CAPTURE + 1])) {
        uint32_t remaining = live_count(r->w_count);

        payload = remaining <= SPI_BUFFER_BYTES ? SPI_BUFFER_BYTES - remaining : 0u;
    }
    t->stats.transactions++;
    t->stats.bytes += payload + 1u;

    address = command & t->framing.address_mask;
    if ((command & t->framing.read_bit_mask) != 0u) {
        uint32_t available = dummies + (SPI_BANK_BYTES - address);
        uint32_t served;

        if (payload > available) {
            t->stats.long_windows++;
            payload = available;
        }
        served = payload > dummies ? payload - dummies : 0u;
        mclw_regmap_bus_read_burst(map, (uint8_t)address, served, t->framing.auto_increment);
    } else {
        if (payload >= SPI_BUFFER_BYTES) {
            t->stats.long_windows++;
        }
        if (payload > SPI_BANK_BYTES) {
            payload = SPI_BANK_BYTES;
        }
        mclw_regmap_bus_write_burst(map, (uint8_t)address, r->capture, payload, t->framing.auto_increment);
    }
}

static void __not_in_flash_func(stream_take)(struct mclw_spi_target *t, struct spi_record *r) {
    struct mclw_stream *s = t->stream;
    uint32_t remaining = live_count(r->w_count);
    uint32_t count = remaining <= SPI_BUFFER_BYTES ? SPI_BUFFER_BYTES - remaining : 0u;

    if (count == 0u) {
        t->stats.short_windows++;
        return;
    }
    t->stats.transactions++;
    t->stats.bytes += count;
    if (count < (uint32_t)s->length) {
        t->stats.short_windows++;
    } else if (count > (uint32_t)s->length) {
        t->stats.long_windows++;
    }
    if (s->direction == MCLW_STREAM_IN && s->on_frame != NULL) {
        uint32_t len = count > SPI_FRAME_BYTES ? SPI_FRAME_BYTES : count;

        if (!t->msb_first) {
            for (uint32_t i = 0; i < len; i++) {
                r->capture[i] = reverse_byte(r->capture[i]);
            }
        }
        s->on_frame(s, r->capture, len, s->on_frame_ctx);
    }
}

static bool __not_in_flash_func(regmap_idle)(const struct mclw_spi_target *t) {
    return reset_idle(t) && data_parked(t, SPI_REGMAP_WAIT) &&
        dma_channel_hw_addr((uint)t->loader_dma)->read_addr == address_of(&t->engine->regmap[1]);
}

// Runs the reset program as a chip-select rise would, for a next window
// that is already armed with what has since changed: the bank, the frame. The monitor pushes a token the rise never does, so its record
// describes no window. Only with nothing moving (reset_idle and the data
// state machine parked): the CPU never touches the data state machine, so
// a window that begins meanwhile meets the same restart as after any rise.
// It returns once the program has run.
static void __not_in_flash_func(kick)(const struct mclw_spi_target *t) {
    uint32_t head = t->engine->head;
    uint32_t start = time_us_32();

    pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_mov_not(pio_isr, pio_null));
    pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_push(false, false));
    pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_mov(pio_isr, pio_null));
    while ((t->engine->head == head || !reset_idle(t)) && time_us_32() - start < SPI_KICK_WAIT_US) {
    }
}

// The bank the next window uses is the one the reset program primes from
// bank_base; a bank select the model took after that release is primed
// here if no window has begun since.
static void __not_in_flash_func(regmap_bank_sync)(struct mclw_spi_target *t) {
    uint8_t bank = t->map->active_bank;

    if (bank >= t->banks) {
        bank = 0;
    }
    if (bank == t->bank) {
        return;
    }
    t->bank = bank;
    t->engine->bank_base = address_of(table_pool[t->table[bank]].descriptors) >> SPI_TABLE_SHIFT;
    if (regmap_idle(t)) {
        // Y is loaded only here and at the arm: the reset program leaves it.
        sm_enable(t->sm, false);
        pio_sm_clear_fifos(SPI_PIO, t->sm);
        pio_sm_restart(SPI_PIO, t->sm);
        pio_sm_put(SPI_PIO, t->sm, t->engine->bank_base);
        pio_sm_exec(SPI_PIO, t->sm, jmp_to(t->bus, SPI_REGMAP_RESTART));
        sm_enable(t->sm, true);
    }
}

static void __not_in_flash_func(count_faults)(struct mclw_spi_target *t) {
    const int8_t channels[] = { t->rx_dma, t->loader_dma, t->tx_dma, t->rw_dma, t->rl_dma };

    for (size_t c = 0; c < sizeof(channels); c++) {
        if (channel_has(channels[c], SPI_DMA_ERROR_BITS)) {
            t->stats.dma_faults++;
            clear_errors(channels[c]);
        }
    }
}

// Takes every window the reset program has recorded. Called in the
// critical section, from the DMA interrupt or the worker's poll.
// Whether the engine holds a window the model has yet to take: one queued,
// one open, or one whose release the reset program is still recording or
// has recorded since the last intake. reset_idle comes before the record
// check: the reset program publishes a record before it parks again.
static bool __not_in_flash_func(window_pending)(const struct mclw_spi_target *t) {
    if (t->queue_count != 0u || !pin_high(t->cs)) {
        return true;
    }
    if (!t->running) {
        return false;
    }
    if (!reset_idle(t)) {
        return true;
    }
    return t->engine->head < SPI_RECORDS && t->tail != t->engine->head;
}

// Lowers BUSY for the model once the engine holds no window it has yet to
// take. A host may begin a window without waiting for BUSY (a driver that
// pulses chip select with no clocks right after each command, before it
// polls BUSY). Lowered before that window is taken, BUSY reads low while the
// engine counts the target busy from that window's release, and the command
// the host then sends is taken as one the part ignored. Deferred, BUSY is
// lowered when that window has been delivered, or by the next intake that
// finds nothing pending. A release between the check and the lowering is
// caught after it, and BUSY raised again. Called in the critical section.
static void __not_in_flash_func(busy_lower)(struct mclw_spi_target *t) {
    if (window_pending(t)) {
        t->busy_lower_pending = true;
        return;
    }
    pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, 0u));
    if (window_pending(t)) {
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, 1u));
        t->busy_lower_pending = true;
        return;
    }
    t->busy_lower_pending = false;
    t->busy = false;
}

static void __not_in_flash_func(intake)(struct mclw_spi_target *t) {
    struct spi_engine *e = t->engine;
    uint32_t head;

    if (!t->running) {
        return;
    }
    head = e->head;
    if (head >= SPI_RECORDS) {
        return;
    }
    for (;;) {
        while (t->tail != head) {
            struct spi_record *r = &e->rec[t->tail];

            if (t->skip_first) {
                t->skip_first = false;
            } else if (r->token != 0u) {
            } else if (t->image == SPI_IMAGE_REGMAP) {
                regmap_take(t, r);
            } else if (t->image == SPI_IMAGE_STREAM) {
                stream_take(t, r);
            } else if (t->image == SPI_IMAGE_COMMAND) {
                command_take(t, r);
            }
            t->tail = (uint8_t)((t->tail + 1u) % SPI_RECORDS);
        }
        if (t->image != SPI_IMAGE_COMMAND || !t->status_stale) {
            break;
        }
        // The pairs a release applies are the CPU's; one that applied its
        // pair while they were rewritten is taken and they are rewritten
        // again.
        command_status_publish(t);
        head = e->head;
        if (head >= SPI_RECORDS || t->tail == head) {
            break;
        }
    }
    count_faults(t);
    if (t->image == SPI_IMAGE_REGMAP) {
        regmap_bank_sync(t);
    }
    if (t->busy_lower_pending) {
        busy_lower(t);
    }
}

// Whether the command image's next window is parked at its start, its
// first status byte in the OSR and its second in the TX FIFO, with no
// window begun since the last release.
static bool __not_in_flash_func(command_idle)(const struct mclw_spi_target *t) {
    const struct spi_command_block *b = command_block(t);

    return reset_idle(t) && data_parked(t, SPI_ENTRY_WAIT) &&
        dma_channel_hw_addr((uint)t->loader_dma)->read_addr ==
            address_of(&b->prologue[SPI_PROLOGUE_OPCODE + 1]) &&
        *(const volatile uint8_t *)&b->jump == SPI_COMMAND_NO_OPCODE;
}

// Whether the open window, once its opcode is in, reads nothing but the
// status byte: then the engine's copy of the rest may change under it. A
// part without BUSY is polled with such windows back to back, so a publish
// that waited for an idle bus might wait as long as the host keeps polling.
// Called in the critical section.
static bool __not_in_flash_func(command_status_window)(const struct mclw_spi_target *t) {
    const volatile uint8_t *list = (const volatile uint8_t *)&command_block(t)->jump;
    uint32_t start = time_us_32();

    while (*list == SPI_COMMAND_NO_OPCODE && !pin_high(t->cs) &&
        time_us_32() - start < SPI_COMMAND_OPCODE_WAIT_US) {
    }
    return *list == SPI_LIST_WRITE * sizeof(struct spi_cb) || *list == SPI_LIST_BUSY * sizeof(struct spi_cb);
}

// Y and the OSR to `word`, the TX FIFO to `word` for the second byte, and
// the state machine back at the window's start. Called with it parked.
static void __not_in_flash_func(command_set_first)(const struct mclw_spi_target *t, uint32_t word) {
    pio_sm_clear_fifos(SPI_PIO, t->sm);
    pio_sm_put(SPI_PIO, t->sm, word);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_out(pio_null, 32u));
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_pull(false, true));
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_put(SPI_PIO, t->sm, word);
    pio_sm_exec(SPI_PIO, t->sm, jmp_to(t->bus, SPI_ENTRY));
}

static void command_notify(struct mclw_command_image *image, void *ctx) {
    struct mclw_spi_target *t = ctx;
    uint32_t state = mclw_hal_critical_enter();

    if (t->image == SPI_IMAGE_COMMAND) {
        uint8_t *s = t->status;

        t->model_status = image->status;
        t->status_stale = true;
        intake(t);
        if (t->status_stale) {
            command_status_publish(t);
        }
        // Only a part with BUSY is re-primed: its host waits for BUSY, which
        // the model lowers after this. A part without has the new status
        // from the next release, and a host that polls it back to back
        // would have a window cut by the re-prime now and then (seen as a
        // poll that lost its first bits).
        if (t->busy_pin >= 0 && command_idle(t) && (s[s[SPI_STATUS_SELECT]] != t->primed ||
            s[s[SPI_STATUS_SELECT]] != s[SPI_STATUS_SERVED])) {
            uint32_t word;

            // The next window's two status bytes, Y in its OSR and the
            // prologue's in the TX FIFO, predate the model's run and are
            // replaced here when they differ from its status. The OSR
            // holds the first with one bit shifted out, so the OUT empties
            // it and the PULL (or an autopull before it) takes the word Y
            // is set from. A window a host ignoring BUSY begins during
            // these few instructions loses its answer, which is why they
            // run only when the status changed.
            (void)command_shadow(t);
            s[SPI_STATUS_SERVED] = s[s[SPI_STATUS_SELECT]];
            t->primed = s[SPI_STATUS_SERVED];
            word = (uint32_t)s[SPI_STATUS_SERVED] << 24;
            command_set_first(t, word);
        } else if (command_idle(t)) {
            (void)command_shadow(t);
        } else if ((image->dirty & (uint8_t)~MCLW_COMMAND_DIRTY_STATUS) == 0u) {
            // The status reaches the next window from the reset program.
            image->dirty = 0;
        } else if (t->busy_pin < 0 && command_status_window(t)) {
            (void)command_shadow(t);
        } else {
            image->dirty &= (uint8_t)~MCLW_COMMAND_DIRTY_STATUS;
            if (image->dirty != 0u) {
                t->shadow_pending = true;
            }
        }
    }
    mclw_hal_critical_exit(state);
}

// Once the model has taken every window that set the busy bits, the
// reset program serves its own status again. A release that set them
// between the check and the write is seen by the second check; the next
// release, microseconds later, is the first to read the choice.
static void __not_in_flash_func(command_busy_settle)(struct mclw_spi_target *t) {
    uint8_t *s = t->status;

    intake(t);
    if (t->busy_windows != 0u) {
        return;
    }
    s[SPI_STATUS_SELECT] = SPI_STATUS_MODEL;
    intake(t);
    if (t->busy_windows != 0u) {
        s[SPI_STATUS_SELECT] = SPI_STATUS_BUSY;
    }
}

// Every window queued when the lock is taken goes to the model under that
// one lock: core 0's script holds the lock often while the host talks to
// it (more often per emulated millisecond the slower an emulation runs),
// and one window per lock left the queue filling and windows missed.
// Windows that arrive meanwhile wait for the next round, after the polls.
static void command_deliver(struct mclw_work *work) {
    struct mclw_spi_target *t = work->ctx;
    struct mclw_command_image *image;
    uint32_t state;
    uint32_t pending;
    bool forced;

    if (t->queue_count == 0u) {
        return;
    }
    if (!mclw_hal_try_lock()) {
        mclw_hal_work_post(work);
        return;
    }
    pending = t->queue_count;
    if (pending > t->stats.max_batch) {
        t->stats.max_batch = pending;
    }
    while (pending-- != 0u) {
        state = mclw_hal_critical_enter();
        if (t->queue_count == 0u) {
            mclw_hal_critical_exit(state);
            break;
        }
        image = t->image == SPI_IMAGE_COMMAND ? t->command : NULL;
        copy_queued(&t->delivered, &t->queue[t->queue_first]);
        t->delivering = true;
        t->queue_first = (uint8_t)((t->queue_first + 1u) % SPI_QUEUE);
        t->queue_count--;
        mclw_hal_critical_exit(state);
        forced = t->delivered.forced;
        // A publish made while a window read what it changes is copied
        // here, under the runtime lock: core 0 may be editing the image
        // otherwise.
        if (image != NULL && t->shadow_pending) {
            command_notify(image, t);
        }
        if (image != NULL && image->on_window != NULL) {
            image->on_window(image, t->delivered.bytes, t->delivered.len, t->delivered.busy, image->on_window_ctx);
        }
        t->delivering = false;
        if (image != NULL && forced) {
            state = mclw_hal_critical_enter();
            if (t->busy_windows != 0u) {
                t->busy_windows--;
            }
            command_busy_settle(t);
            mclw_hal_critical_exit(state);
        }
    }
    state = mclw_hal_critical_enter();
    if (t->queue_count != 0u) {
        mclw_hal_work_post(work);
    }
    mclw_hal_critical_exit(state);
    mclw_hal_unlock();
}

static void __not_in_flash_func(spi_target_dma_handler)(void) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *t = &targets[i];

        if (t->owner == NULL || t->rw_dma < 0 || !dma_irqn_get_channel_status(1u, (uint)t->rw_dma)) {
            continue;
        }
        dma_irqn_acknowledge_channel(1u, (uint)t->rw_dma);
        intake(t);
    }
    mclw_hal_critical_exit(state);
}

static void stream_fill(struct mclw_spi_target *t, uint8_t *buf) {
    struct mclw_stream *s = t->stream;
    const uint8_t *frame = s->frame[s->active];
    uint16_t length = s->length > MCLW_STREAM_FRAME_MAX ? MCLW_STREAM_FRAME_MAX : s->length;

    memset(buf, wire_byte(t, s->filler), SPI_BUFFER_BYTES);
    if (s->direction == MCLW_STREAM_OUT) {
        for (uint16_t i = 0; i < length; i++) {
            buf[i] = wire_byte(t, frame[i]);
        }
    }
}

// The next window's frame is whichever buffer d_cfg names when its
// release loads D. A publish fills a buffer that neither D is reading nor
// d_cfg names, then names it: no window serves a half-written frame. With
// no window open the next one is already loaded with the frame before, so
// the reset program loads it again.
static void __not_in_flash_func(stream_notify)(struct mclw_stream *s, void *ctx) {
    struct mclw_spi_target *t = ctx;
    uint32_t state = mclw_hal_critical_enter();

    (void)s;
    if (t->image == SPI_IMAGE_STREAM) {
        uint32_t reading = dma_channel_hw_addr((uint)t->tx_dma)->read_addr;
        uint32_t named = t->engine->d_cfg[0];
        uint8_t pick = 0;

        for (uint8_t i = 0; i < SPI_TX_BUFFERS; i++) {
            uint32_t base = address_of(t->tx_buf[i]);
            bool read = reading >= base && reading <= base + SPI_BUFFER_BYTES;

            if (!read && base != named) {
                pick = i;
                break;
            }
        }
        stream_fill(t, t->tx_buf[pick]);
        __compiler_memory_barrier();
        t->engine->d_cfg[0] = address_of(t->tx_buf[pick]);
        if (reset_idle(t) && data_parked(t, SPI_ENTRY_WAIT)) {
            kick(t);
        } else if (!pin_high(t->cs)) {
            t->stats.stale_windows++;
        }
    }
    mclw_hal_critical_exit(state);
}

static void dma_handler_install(void *ctx) {
    bool want = *(const bool *)ctx;

    if (want) {
        irq_add_shared_handler(DMA_IRQ_1, spi_target_dma_handler,
            PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_priority(DMA_IRQ_1, 0);
        irq_set_enabled(DMA_IRQ_1, true);
    } else {
        irq_remove_handler(DMA_IRQ_1, spi_target_dma_handler);
    }
}

static void dma_handler_sync(struct spi_bus *bus, bool want) {
    if (want == bus->dma_installed) {
        return;
    }
    mclw_hal_work_call(dma_handler_install, &want);
    bus->dma_installed = want;
}

static void poll_targets(void *ctx) {
    (void)ctx;
    mclw_hal_spi_target_poll();
}

static void release_tables(struct mclw_spi_target *t) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);

    for (size_t i = 0; i < SPI_TABLE_SLOTS; i++) {
        if (table_owner[i] == tag) {
            table_owner[i] = 0;
        }
    }
    t->banks = 0;
}

static bool claim_tables(struct mclw_spi_target *t, uint8_t banks) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);
    uint8_t taken = 0;

    for (size_t i = 0; i < SPI_TABLE_SLOTS && taken < banks; i++) {
        if (table_owner[i] == 0u) {
            table_owner[i] = tag;
            t->table[taken] = (uint8_t)i;
            taken++;
        }
    }
    if (taken < banks) {
        release_tables(t);
        return false;
    }
    t->banks = banks;
    return true;
}

// The command image's block and, for its pages, two adjacent slots within
// one 64 KiB region: the register list composes a page's address from its
// bits 15..8.
static bool claim_command_tables(struct mclw_spi_target *t) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);

    for (uint8_t i = 0; i + 1u < SPI_TABLE_SLOTS; i++) {
        if (table_owner[i] != 0u || table_owner[i + 1u] != 0u ||
            (address_of(&table_pool[i]) >> 16) != (address_of(&table_pool[i + 2u]) - 1u) >> 16) {
            continue;
        }
        table_owner[i] = tag;
        table_owner[i + 1u] = tag;
        if (!claim_tables(t, 1u)) {
            return false;
        }
        t->table[1] = i;
        t->table[2] = (uint8_t)(i + 1u);
        t->banks = 3;
        return true;
    }
    return false;
}

static void build_tables(struct mclw_spi_target *t) {
    const struct mclw_regmap *map = t->map;

    for (uint8_t b = 0; b < t->banks; b++) {
        struct spi_descriptor *table = table_pool[t->table[b]].descriptors;
        const uint8_t *base = map->value[b];

        for (uint32_t command = 0; command < SPI_BANK_BYTES; command++) {
            if ((command & t->framing.read_bit_mask) != 0u) {
                uint32_t address = command & t->framing.address_mask;

                table[command].count = SPI_BANK_BYTES - address;
                table[command].source = (uint32_t)(uintptr_t)(base + address);
            } else {
                table[command].count = SPI_BUFFER_BYTES;
                table[command].source = (uint32_t)(uintptr_t)t->tx_buf[0];
            }
        }
    }
}

// SCK, MOSI and MISO are the bus, shared by every target on it, so the
// engine owns them; only the chip select belongs to the instance.
static int bus_claim_pins(struct spi_bus *bus, uint8_t sck, uint8_t mosi, uint8_t miso) {
    const uint8_t pins[3] = { sck, mosi, miso };
    size_t claimed;

    for (claimed = 0; claimed < 3u; claimed++) {
        int err = mclw_hal_pin_claim(pins[claimed], bus);

        if (err != 0) {
            while (claimed > 0u) {
                claimed--;
                mclw_hal_pin_release(pins[claimed], bus);
            }
            return err;
        }
    }
    bus->sck = sck;
    bus->mosi = mosi;
    bus->miso = miso;
    return 0;
}

static void bus_release_pins(struct spi_bus *bus) {
    const uint8_t pins[3] = { bus->sck, bus->mosi, bus->miso };

    uint base = pio_get_gpio_base(SPI_PIO);

    if (bus->sck >= base && bus->sck - base < 32u) {
        hw_clear_bits(&SPI_PIO->input_sync_bypass, 1u << (bus->sck - base));
    }
    for (size_t i = 0; i < 3u; i++) {
        gpio_set_function(pins[i], GPIO_FUNC_SIO);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
        mclw_hal_pin_release(pins[i], bus);
    }
}

static void release_cs(const struct mclw_spi_target *t) {
    gpio_set_function(t->cs, GPIO_FUNC_SIO);
    gpio_set_dir(t->cs, GPIO_IN);
    gpio_disable_pulls(t->cs);
    mclw_hal_pin_release(t->cs, t->owner);
}

static bool bus_class_conflict(const struct spi_bus *bus, const struct mclw_spi_target *t,
    uint8_t cls) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct mclw_spi_target *o = &targets[i];

        if (o != t && o->bus == bus && mode_class(o->mode) != cls) {
            return true;
        }
    }
    return false;
}

static void bus_set_class(struct spi_bus *bus, uint8_t cls) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *o = &targets[i];

        if (o->bus == bus && o->image != SPI_IMAGE_NONE) {
            engine_stop(o);
        }
    }
    program_patch(bus, bus->program_sck, cls);
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *o = &targets[i];

        if (o->bus == bus && o->image != SPI_IMAGE_NONE) {
            engine_common(o);
            if (o->image == SPI_IMAGE_REGMAP) {
                regmap_lists(o);
            } else if (o->image == SPI_IMAGE_STREAM) {
                stream_lists(o);
            } else {
                command_lists(o);
            }
            engine_start(o);
        }
    }
}

static bool bus_dummy_conflict(const struct spi_bus *bus, const struct mclw_spi_target *t,
    uint16_t dummy) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct mclw_spi_target *o = &targets[i];

        if (o != t && o->bus == bus && o->image == SPI_IMAGE_REGMAP &&
            dummy_instruction(&o->framing, o->map->filler) != dummy) {
            return true;
        }
    }
    return false;
}

static void unclaim_channels(struct mclw_spi_target *t) {
    int8_t *channels[] = { &t->tx_dma, &t->rx_dma, &t->loader_dma, &t->rw_dma, &t->rl_dma };

    for (size_t c = 0; c < sizeof(channels) / sizeof(channels[0]); c++) {
        if (*channels[c] >= 0) {
            dma_channel_unclaim((uint)*channels[c]);
        }
        *channels[c] = -1;
    }
}

struct mclw_spi_target *mclw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso,
    uint8_t cs, uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct spi_bus *bus = &spi_bus;
    struct mclw_spi_target *t = NULL;
    bool claimed_bus = false;
    uint8_t cls;
    int sm;

    if (err == NULL) {
        return NULL;
    }
    if (owner == NULL || mode > 3 || sck == mosi || sck == miso || sck == cs ||
        mosi == miso || mosi == cs || miso == cs) {
        *err = -EINVAL;
        return NULL;
    }
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        if (targets[i].owner == NULL) {
            t = &targets[i];
            break;
        }
    }
    if (t == NULL) {
        *err = -EBUSY;
        return NULL;
    }
    cls = mode_class(mode);
    if (bus->open_count != 0u && (sck != bus->sck || mosi != bus->mosi || miso != bus->miso ||
        cls != bus->program_class)) {
        *err = -ENOSPC;
        return NULL;
    }

    if (bus->open_count == 0u) {
        *err = bus_claim_pins(bus, sck, mosi, miso);
        if (*err != 0) {
            return NULL;
        }
        claimed_bus = true;
    }
    *err = mclw_hal_pin_claim(cs, owner);
    if (*err != 0) {
        if (claimed_bus) {
            bus_release_pins(bus);
        }
        return NULL;
    }

    if (!mclw_pio_window(SPI_PIO, MCLW_PIO_PIN(sck) | MCLW_PIO_PIN(mosi) | MCLW_PIO_PIN(miso) |
        MCLW_PIO_PIN(cs))) {
        *err = -EINVAL;
        goto fail_pins;
    }
    sm = pio_claim_unused_sm(SPI_PIO, false);
    if (sm < 0) {
        *err = -EBUSY;
        goto fail_pins;
    }
    t->sm = (uint8_t)sm;
    sm = pio_claim_unused_sm(SPI_PIO, false);
    if (sm < 0) {
        pio_sm_unclaim(SPI_PIO, t->sm);
        *err = -EBUSY;
        goto fail_pins;
    }
    t->monitor_sm = (uint8_t)sm;
    t->tx_dma = (int8_t)dma_claim_unused_channel(false);
    t->rx_dma = (int8_t)dma_claim_unused_channel(false);
    t->loader_dma = (int8_t)dma_claim_unused_channel(false);
    t->rw_dma = (int8_t)dma_claim_unused_channel(false);
    t->rl_dma = (int8_t)dma_claim_unused_channel(false);
    if (t->tx_dma < 0 || t->rx_dma < 0 || t->loader_dma < 0 || t->rw_dma < 0 || t->rl_dma < 0) {
        *err = -ENOMEM;
        goto fail_dma;
    }
    *err = bus_load_program(bus, sck, cls);
    if (*err != 0) {
        goto fail_dma;
    }

    t->bus = bus;
    t->cs = cs;
    t->mode = mode;
    t->msb_first = msb_first;
    t->image = SPI_IMAGE_NONE;
    t->map = NULL;
    t->stream = NULL;
    t->command = NULL;
    t->engine = &engines[t - targets];
    t->status = status_pages[t - targets];
    t->busy_pin = -1;
    t->busy = false;
    t->busy_lower_pending = false;
    t->running = false;
    t->queue_first = 0;
    t->queue_count = 0;
    t->shadow_pending = false;
    t->record_work = (struct mclw_work){ .fn = command_deliver, .ctx = t, .pending = false, .next = NULL };
    t->banks = 0;
    t->bank = 0;
    memset(&t->stats, 0, sizeof(t->stats));

    pio_gpio_init(SPI_PIO, sck);
    pio_gpio_init(SPI_PIO, mosi);
    pio_gpio_init(SPI_PIO, miso);
    gpio_set_function(cs, GPIO_FUNC_SIO);
    gpio_set_dir(cs, GPIO_IN);
    gpio_pull_up(cs);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, miso, 1u, false);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, sck, 1u, false);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, mosi, 1u, false);
    {
        pio_sm_config c = pio_get_default_sm_config();

        sm_config_set_in_pins(&c, cs);
        sm_config_set_wrap(&c, bus->program_offset + SPI_MONITOR_ENTRY,
            bus->program_offset + SPI_MONITOR_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->monitor_sm, bus->program_offset + SPI_MONITOR_ENTRY, &c);
        pio_sm_set_enabled(SPI_PIO, t->monitor_sm, true);
    }

    t->owner = owner;
    if (bus->open_count++ == 0u && !bus->poll_registered) {
        bus->poll_registered = mclw_hal_poll_register(poll_targets, NULL) == 0;
    }
    dma_handler_sync(bus, true);
    *err = 0;
    return t;

fail_dma:
    unclaim_channels(t);
    pio_sm_unclaim(SPI_PIO, t->sm);
    pio_sm_unclaim(SPI_PIO, t->monitor_sm);
fail_pins:
    if (claimed_bus) {
        bus_release_pins(bus);
    }
    mclw_hal_pin_release(cs, owner);
    return NULL;
}

void mclw_hal_spi_target_disarm(struct mclw_spi_target *t) {
    uint32_t state;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    state = mclw_hal_critical_enter();
    if (t->image != SPI_IMAGE_NONE) {
        engine_stop(t);
        if (t->stream != NULL) {
            t->stream->notify = NULL;
            t->stream->notify_ctx = NULL;
        }
        if (t->command != NULL) {
            t->command->notify = NULL;
            t->command->notify_ctx = NULL;
        }
        if (t->busy_pin >= 0) {
            pio_sm_set_set_pins(SPI_PIO, t->monitor_sm, 0u, 0u);
            gpio_set_function((uint)t->busy_pin, GPIO_FUNC_SIO);
            t->busy_pin = -1;
        }
        release_tables(t);
        t->image = SPI_IMAGE_NONE;
        t->map = NULL;
        t->stream = NULL;
        t->command = NULL;
        t->busy = false;
        t->queue_first = 0;
        t->queue_count = 0;
        t->busy_windows = 0;
        t->busy_orphaned = false;
        t->busy_lower_pending = false;
        t->shadow_pending = false;
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_target_close(struct mclw_spi_target *t) {
    struct spi_bus *bus;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    bus = t->bus;
    mclw_hal_spi_target_disarm(t);

    sm_enable(t->sm, false);
    pio_sm_unclaim(SPI_PIO, t->sm);
    pio_sm_set_enabled(SPI_PIO, t->monitor_sm, false);
    pio_sm_unclaim(SPI_PIO, t->monitor_sm);
    unclaim_channels(t);
    if (bus->open_count == 1u) {
        bus_release_pins(bus);
    }
    release_cs(t);
    t->owner = NULL;
    t->bus = NULL;
    bus->open_count--;
    if (bus->open_count == 0u) {
        dma_handler_sync(bus, false);
        bus_unload_program(bus);
        if (bus->poll_registered) {
            mclw_hal_poll_unregister(poll_targets, NULL);
            bus->poll_registered = false;
        }
    }
}

int mclw_hal_spi_target_set_mode(struct mclw_spi_target *t, uint8_t mode) {
    struct spi_bus *bus;
    uint8_t cls;
    uint32_t state;

    if (t == NULL || t->owner == NULL || mode > 3) {
        return -EINVAL;
    }
    bus = t->bus;
    cls = mode_class(mode);
    if (cls != bus->program_class && bus_class_conflict(bus, t, cls)) {
        return -ENOSPC;
    }
    state = mclw_hal_critical_enter();
    t->mode = mode;
    if (cls != bus->program_class) {
        bus_set_class(bus, cls);
    }
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_regmap(struct mclw_spi_target *t, struct mclw_regmap *map,
    const struct mclw_regmap_framing *framing) {
    uint8_t banks;
    uint32_t state;
    uint16_t dummy;

    if (t == NULL || t->owner == NULL || map == NULL || framing == NULL) {
        return -EINVAL;
    }
    if (!t->msb_first) {
        return -ENOTSUP;
    }
    if (framing->read_dummy_bytes > 1u) {
        return -ENOTSUP;
    }
    if (framing->read_dummy_bytes != 0u && map->filler != 0x00u && map->filler != 0xffu) {
        return -ENOTSUP;
    }
    if (!framing->auto_increment && framing->read_dummy_bytes != 0u) {
        return -ENOTSUP;
    }
    dummy = dummy_instruction(framing, map->filler);
    if (bus_dummy_conflict(t->bus, t, dummy)) {
        return -ENOSPC;
    }
    mclw_hal_spi_target_disarm(t);
    banks = map->bank_count == 0 ? 1 : map->bank_count;
    if (banks > MCLW_REGMAP_BANKS_MAX) {
        banks = MCLW_REGMAP_BANKS_MAX;
    }
    state = mclw_hal_critical_enter();
    if (!claim_tables(t, banks)) {
        mclw_hal_critical_exit(state);
        return -ENOMEM;
    }
    t->map = map;
    t->framing = *framing;
    t->image = SPI_IMAGE_REGMAP;
    memset(t->tx_buf[0], map->filler, sizeof(t->tx_buf[0]));
    build_tables(t);
    memset(&t->stats, 0, sizeof(t->stats));
    if (dummy != t->bus->program_dummy) {
        program_patch_dummy(t->bus, dummy);
    }
    t->bank = map->active_bank < t->banks ? map->active_bank : 0u;
    t->engine->bank_base = address_of(table_pool[t->table[t->bank]].descriptors) >> SPI_TABLE_SHIFT;
    configure_sm(t, SPI_IMAGE_REGMAP);
    load_y(t, t->engine->bank_base);
    engine_common(t);
    regmap_lists(t);
    engine_start(t);
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_stream(struct mclw_spi_target *t,
    struct mclw_stream *s) {
    uint32_t state;

    if (t == NULL || t->owner == NULL || s == NULL) {
        return -EINVAL;
    }
    if (s->length > MCLW_STREAM_FRAME_MAX) {
        return -EINVAL;
    }
    mclw_hal_spi_target_disarm(t);
    state = mclw_hal_critical_enter();
    t->stream = s;
    t->image = SPI_IMAGE_STREAM;
    memset(&t->stats, 0, sizeof(t->stats));
    s->notify = stream_notify;
    s->notify_ctx = t;
    configure_sm(t, SPI_IMAGE_STREAM);
    engine_common(t);
    stream_fill(t, t->tx_buf[0]);
    t->engine->d_cfg[0] = address_of(t->tx_buf[0]);
    stream_lists(t);
    engine_start(t);
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_command(struct mclw_spi_target *t, struct mclw_command_image *image,
    struct mclw_gpio *busy) {
    uint32_t state;

    if (t == NULL || t->owner == NULL || image == NULL) {
        return -EINVAL;
    }
    if (!t->msb_first) {
        return -ENOTSUP;
    }
    if (image->page_count > MCLW_COMMAND_PAGES_MAX ||
        image->response_count > MCLW_COMMAND_RESPONSES_MAX) {
        return -EINVAL;
    }
    if ((image->immediate_response && busy != NULL) ||
        (image->short_register_read && (!image->immediate_response || image->has_memory_read))) {
        return -EINVAL;
    }
    if (busy != NULL && !mclw_pio_window(SPI_PIO, MCLW_PIO_PIN(mclw_hal_gpio_pin(busy)))) {
        return -EINVAL;
    }
    mclw_hal_spi_target_disarm(t);
    state = mclw_hal_critical_enter();
    if (!claim_command_tables(t)) {
        mclw_hal_critical_exit(state);
        return -ENOMEM;
    }
    t->command = image;
    t->image = SPI_IMAGE_COMMAND;
    t->queue_first = 0;
    t->queue_count = 0;
    t->busy_windows = 0;
    t->busy_orphaned = false;
    t->busy_lower_pending = false;
    t->shadow_pending = false;
    memset(&t->stats, 0, sizeof(t->stats));
    command_build(t);
    if (busy != NULL) {
        uint8_t pin = mclw_hal_gpio_pin(busy);

        t->busy_pin = (int8_t)pin;
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_nop());
        pio_sm_set_set_pins(SPI_PIO, t->monitor_sm, pin, 1u);
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, 1u));
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pindirs, 1u));
        pio_gpio_init(SPI_PIO, pin);
        t->busy = true;
    }
    image->notify = command_notify;
    image->notify_ctx = t;
    configure_sm(t, SPI_IMAGE_COMMAND);
    t->primed = t->status[SPI_STATUS_SERVED];
    load_y(t, (uint32_t)t->primed << 24);
    engine_common(t);
    command_lists(t);
    engine_start(t);
    mclw_hal_critical_exit(state);
    return 0;
}

void mclw_hal_spi_target_set_busy(struct mclw_spi_target *t, bool busy) {
    uint32_t state;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    state = mclw_hal_critical_enter();
    if (t->image == SPI_IMAGE_COMMAND && t->busy_pin >= 0) {
        // Windows the reset program recorded before this were taken while
        // BUSY was still high.
        intake(t);
        if (busy) {
            t->busy_lower_pending = false;
            pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, 1u));
            t->busy = true;
        } else {
            busy_lower(t);
        }
    }
    mclw_hal_critical_exit(state);
}

// The worker's poll: windows the DMA interrupt has not taken yet, a
// pending image copy once no window reads it, and a reset program that is
// parked on a token it was not credited for.
void mclw_hal_spi_target_poll(void) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *t = &targets[i];
        uint32_t state;

        if (t->owner == NULL || t->image == SPI_IMAGE_NONE) {
            continue;
        }
        state = mclw_hal_critical_enter();
        intake(t);
        if (t->running) {
            const dma_channel_hw_t *rw = dma_channel_hw_addr((uint)t->rw_dma);
            bool parked = (rw->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u &&
                rw->read_addr == address_of(&SPI_PIO->rxf[t->monitor_sm]);

            if (parked && !pio_sm_is_rx_fifo_empty(SPI_PIO, t->monitor_sm)) {
                if (++t->stuck_polls >= 2u) {
                    dma_debug_hw->ch[t->rw_dma].dbg_ctdreq = 0;
                    t->stats.missed_windows++;
                    t->busy_orphaned = true;
                    t->stuck_polls = 0;
                }
            } else {
                t->stuck_polls = 0;
            }
        }
        // A busy window the queue had no room for, or whose record was
        // lost, leaves the busy bits served with nothing to deliver that
        // would lower them; the host polling them keeps the queue full, so
        // this cannot wait for it. Only then: the reset program raises the
        // bits as a busy window ends, before its record reaches
        // command_take, and lowering them in that gap served the model's
        // idle status ahead of the erase or program it had yet to see.
        if (t->image == SPI_IMAGE_COMMAND && t->busy_image && t->busy_orphaned && t->busy_windows == 0u &&
            t->status[SPI_STATUS_SELECT] == SPI_STATUS_BUSY) {
            t->busy_orphaned = false;
            command_busy_settle(t);
        }
        mclw_hal_critical_exit(state);
        if (t->image == SPI_IMAGE_COMMAND && t->shadow_pending && t->queue_count == 0u && mclw_hal_try_lock()) {
            if (t->shadow_pending) {
                command_notify(t->command, t);
            }
            mclw_hal_unlock();
        }
    }
}

void mclw_hal_spi_target_stats(const struct mclw_spi_target *t,
    struct mclw_spi_target_stats *out) {
    uint32_t state;

    if (out == NULL) {
        return;
    }
    if (t == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    state = mclw_hal_critical_enter();
    *out = t->stats;
    mclw_hal_critical_exit(state);
}

uint8_t mclw_hal_spi_target_mode(const struct mclw_spi_target *t) {
    if (t == NULL) {
        return 0u;
    }
    return t->mode;
}

uint32_t mclw_hal_spi_target_max_clock_hz(const struct mclw_spi_target *t) {
    if (t == NULL) {
        return 0u;
    }
    switch (t->image) {
        case SPI_IMAGE_REGMAP:
            return SPI_REGMAP_CLOCK_HZ;
        case SPI_IMAGE_STREAM:
            return SPI_STREAM_CLOCK_HZ;
        case SPI_IMAGE_COMMAND:
            if (t->command != NULL && t->command->immediate_response) {
                return t->command->response_count > 1u ? SPI_COMMAND_IMMEDIATE_LOOKUP_CLOCK_HZ
                                                       : SPI_COMMAND_IMMEDIATE_CLOCK_HZ;
            }
            return t->command != NULL && t->command->has_memory_read ? SPI_COMMAND_MEMORY_CLOCK_HZ
                                                                     : SPI_COMMAND_CLOCK_HZ;
        case SPI_IMAGE_NONE:
        default:
            return 0u;
    }
}
