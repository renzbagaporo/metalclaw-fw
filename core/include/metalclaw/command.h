// SPDX-License-Identifier: Apache-2.0 OR MIT
// An image for parts whose SPI protocol is framed by the first byte of each
// chip-select window, the opcode, and that answer with a status byte while
// the host clocks the command header (Semtech SX126x, SX128x). Per window:
//
//   write opcodes   opcode, parameters...            the part clocks status
//   response reads  opcode, NOP, response...         status on the NOP
//   register read   opcode, addr[15:8], addr[7:0], NOP, registers from addr...
//   buffer read     opcode, offset, NOP, buffer bytes from offset, wrapping
//   memory read     opcode, addr[23:16], addr[15:8], addr[7:0], registers
//                   from addr[15:0]... (a serial NOR flash's Read Data)
//   immediate read  opcode, response... (a serial NOR flash's Read JEDEC
//                   ID, a CAN controller's status and RX buffer reads):
//                   the responses of an image with immediate responses
//                   have no NOP, and such an image has no BUSY line
//   short register  opcode, addr[7:0], registers of page 0 from addr...
//   read            (a CAN controller's READ), for an image with
//                   immediate responses
//
// Every byte that carries no data is the status byte, the first one
// included. The engine serves all of it from hardware. The model is called
// only after the window, with what the host sent, and it may then take as
// long as it likes: the engine raises the part's BUSY line on chip-select
// release, and the host waits for BUSY low before the next window. The model
// edits the image, publishes it, and lowers BUSY through the engine.
//
// Registers live in up to MCLW_COMMAND_PAGES_MAX pages of 256 bytes,
// named by address bits 15..8. Reads of a page the image does not hold
// return the image's filler, zero unless set; writes are the model's
// business, since they arrive as windows like every other command. A memory
// read has no NOP: its first data byte leaves one bit time after its last
// address bit, which lowers the clock the engine can serve it at. Address
// bits 23..16 reach the model but select nothing.
//
// A part without a BUSY line (a NOR flash) reports its own busy state in
// the status byte, and its host polls that. For such a part the image names
// the opcodes that start an internal operation and the status bits that say
// so; the engine sets those bits from the release of such a window until
// the model has taken it, and every such window before it, so a host that
// polls at once never sees the part idle before the model has taken the
// command. From then on the status is the model's, as it last published it.

#ifndef MCLW_COMMAND_H
#define MCLW_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_COMMAND_PAGES_MAX 7
#define MCLW_COMMAND_PAGE_SIZE 256
#define MCLW_COMMAND_BUFFER_SIZE 256
#define MCLW_COMMAND_RESPONSES_MAX 8
#define MCLW_COMMAND_RESPONSE_MAX 16
// The longest window a model is handed: the opcode and this many bytes.
#define MCLW_COMMAND_WINDOW_MAX 264

enum mclw_command_dirty {
    MCLW_COMMAND_DIRTY_STATUS = 1u << 0,
    MCLW_COMMAND_DIRTY_RESPONSES = 1u << 1,
    MCLW_COMMAND_DIRTY_PAGES = 1u << 2,
    MCLW_COMMAND_DIRTY_BUFFER = 1u << 3,
};

struct mclw_command_response {
    uint8_t opcode;
    uint8_t bytes[MCLW_COMMAND_RESPONSE_MAX];
};

struct mclw_command_image;

// One window, after chip-select release. `bytes` starts with the opcode;
// for a write opcode the parameters follow, for a register or buffer read
// the address bytes the host sent. `len` is 0 for a window too short to
// carry an opcode (a bare chip-select pulse, as hosts use to wake a part).
// `busy` says BUSY was still high when the window began. Runs on the
// worker under the runtime lock; the bytes are only valid for the call.
typedef void (*mclw_command_on_window_t)(struct mclw_command_image *image, const uint8_t *bytes,
    size_t len, bool busy, void *ctx);
typedef void (*mclw_command_notify_t)(struct mclw_command_image *image, void *ctx);

struct mclw_command_image {
    uint8_t status;
    uint8_t read_register_opcode;
    uint8_t read_buffer_opcode;
    bool has_memory_read;
    uint8_t read_memory_opcode;
    bool immediate_response;
    bool short_register_read;
    uint8_t filler;
    uint8_t busy_status;
    uint8_t ready_status;
    uint8_t busy_opcodes[256 / 8];
    uint8_t response_count;
    struct mclw_command_response responses[MCLW_COMMAND_RESPONSES_MAX];
    uint8_t page_count;
    uint8_t page_number[MCLW_COMMAND_PAGES_MAX];
    uint8_t pages[MCLW_COMMAND_PAGES_MAX][MCLW_COMMAND_PAGE_SIZE];
    uint8_t buffer[MCLW_COMMAND_BUFFER_SIZE];
    // What changed since the engine last took a copy. Only
    // mclw_command_* and the engine touch it.
    volatile uint8_t dirty;
    mclw_command_on_window_t on_window;
    void *on_window_ctx;
    // Set by the engine at arm; called by mclw_command_publish.
    mclw_command_notify_t notify;
    void *notify_ctx;
};

// Clears everything, the hook included. The opcode framings and the
// response opcodes and pages declared before arming are what the engine
// serves; their contents may change at any time after.
void mclw_command_init(struct mclw_command_image *image, uint8_t read_register_opcode,
    uint8_t read_buffer_opcode);
void mclw_command_set_on_window(struct mclw_command_image *image, mclw_command_on_window_t fn,
    void *ctx);
// A memory read opcode, which takes the place of the register or buffer
// read on that opcode; a part with neither names the memory opcode for both.
void mclw_command_set_memory_read(struct mclw_command_image *image, uint8_t opcode);
// The image's responses follow their opcodes at once, with no NOP. Every
// window's byte after the opcode is then served once the opcode is decoded
// rather than ahead of it, which lowers the clock the engine serves the
// image at, more for several responses than for one; the engine takes such
// an image only with no BUSY line.
void mclw_command_set_immediate_response(struct mclw_command_image *image);
// The register read takes one address byte, addr[7:0], and reads page 0
// from it, with no NOP: only with immediate responses.
void mclw_command_set_short_register_read(struct mclw_command_image *image);
// What a read of a page the image does not hold returns.
void mclw_command_set_filler(struct mclw_command_image *image, uint8_t filler);
// The status bits the engine sets at the release of a window that began
// with `opcode`, until the model has taken that window.
void mclw_command_set_busy_status(struct mclw_command_image *image, uint8_t bits);
// For a part whose status shows ready rather than busy (an AT45's
// RDY/BUSY, set when ready): the bits the engine clears instead, over the
// same span.
void mclw_command_set_ready_status(struct mclw_command_image *image, uint8_t bits);
void mclw_command_add_busy_opcode(struct mclw_command_image *image, uint8_t opcode);
bool mclw_command_busy_opcode(const struct mclw_command_image *image, uint8_t opcode);
// -ENOSPC when the image is full.
int mclw_command_add_page(struct mclw_command_image *image, uint8_t page);
int mclw_command_add_response(struct mclw_command_image *image, uint8_t opcode);

void mclw_command_set_status(struct mclw_command_image *image, uint8_t status);
// Copies up to MCLW_COMMAND_RESPONSE_MAX bytes; the rest of the
// response reads as zero. -ENOENT for an opcode that was never added.
int mclw_command_set_response(struct mclw_command_image *image, uint8_t opcode, const uint8_t *bytes,
    size_t len);
// NULL for an address in a page the image does not hold.
uint8_t *mclw_command_register(struct mclw_command_image *image, uint16_t address);
uint8_t mclw_command_get_register(const struct mclw_command_image *image, uint16_t address);
// Writes the bytes that fall in held pages; returns how many did.
size_t mclw_command_set_registers(struct mclw_command_image *image, uint16_t address,
    const uint8_t *bytes, size_t len);
// Wraps at the end of the buffer, as the part does.
void mclw_command_write_buffer(struct mclw_command_image *image, uint8_t offset, const uint8_t *bytes,
    size_t len);
void mclw_command_read_buffer(const struct mclw_command_image *image, uint8_t offset, uint8_t *bytes,
    size_t len);

// Hands the edits so far to the engine: at once if no window is open,
// otherwise after the open one, before its on_window. Under the runtime
// lock.
void mclw_command_publish(struct mclw_command_image *image);

#endif
