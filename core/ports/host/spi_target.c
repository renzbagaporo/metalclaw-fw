// SPDX-License-Identifier: Apache-2.0 OR MIT
// SPI targets for the host port, served a frame at a time from the bridge:
// the emulated DUT's controller hands over each MOSI byte and takes the
// MISO byte of the same frame, and its chip select arrives as a pin level
// (BRIDGE.md). The rp2 port's engine runs the same images in DMA; this is
// that engine in software, byte for byte (metalclaw/hal/spi_target.h):
//
//   - a register map, framed by its command byte: filler while the command
//     and any dummy bytes are clocked, then the image from the address;
//     the reads' side effects and the writes land at the window's end;
//   - a stream: the published frame out, or the window's bytes in;
//   - a command image: the window's first byte is the status, and the
//     second too unless responses are immediate; the opcode picks what
//     follows (rp2 spi_target.c, command_build): status on every byte of a
//     write, a response from byte 2 (1 when immediate), the buffer from
//     byte 3 at the offset in byte 1, a register read from byte 4 at the
//     address in bytes 1-2 (the short form from byte 2 at byte 1's), a
//     memory read from byte 4 at bytes 2-3 of its address. Each window
//     reaches the model's on_window on the worker, as the rp2 engine's
//     record has it: the opcode, then a write's bytes, a buffer read's
//     offset, or a read's address bytes.
//
// A MISO byte depends only on the MOSI bytes before it, as on the wire.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"

#include "host.h"

#define SPI_TARGETS_MAX 8u
#define SPI_QUEUE 16u
#define WINDOW_BYTES (MCLW_COMMAND_WINDOW_MAX + 4u)

enum image_kind {
    IMAGE_NONE,
    IMAGE_REGMAP,
    IMAGE_STREAM,
    IMAGE_COMMAND,
};

enum list {
    LIST_WRITE,
    LIST_BUSY,
    LIST_RESPONSE,
    LIST_BUFFER,
    LIST_REGISTER,
    LIST_MEMORY,
};

struct queued {
    uint8_t bytes[WINDOW_BYTES];
    uint16_t len;
    bool busy;
    bool forced;
};

struct mclw_spi_target {
    const void *owner;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
    uint8_t cs;
    uint8_t mode;
    bool msb_first;
    uint8_t kind;
    struct mclw_regmap *map;
    struct mclw_regmap_framing framing;
    struct mclw_stream *stream;
    struct mclw_command_image *image;
    // What the command engine serves: the image as last published.
    struct mclw_command_image shadow;
    struct mclw_gpio *busy_gpio;
    // A busy pin's state: high from a window's release until set_busy().
    bool busy;
    // Busy-opcode windows not yet taken by the model; while any wait, the
    // status served is the busy one.
    uint32_t forced_pending;
    // The open window.
    bool selected;
    uint8_t in[WINDOW_BYTES];
    uint32_t count;
    uint8_t list;
    uint8_t stream_frame;
    // Windows for the worker.
    struct queued queue[SPI_QUEUE];
    uint32_t queue_first;
    uint32_t queue_count;
    struct mclw_spi_target_stats stats;
};

static struct mclw_spi_target targets[SPI_TARGETS_MAX];
static bool poll_registered;

static uint8_t command_status(const struct mclw_spi_target *t) {
    const struct mclw_command_image *s = &t->shadow;

    if (t->forced_pending != 0u) {
        return (uint8_t)((s->status | s->busy_status) & (uint8_t)~s->ready_status);
    }
    return s->status;
}

static uint8_t command_list(const struct mclw_command_image *s, uint8_t opcode) {
    uint8_t list = LIST_WRITE;

    if ((s->busy_status != 0u || s->ready_status != 0u) && mclw_command_busy_opcode(s, opcode)) {
        list = LIST_BUSY;
    }
    for (uint8_t i = 0; i < s->response_count; i++) {
        if (s->responses[i].opcode == opcode) {
            list = LIST_RESPONSE;
        }
    }
    if (opcode == s->read_buffer_opcode) {
        list = LIST_BUFFER;
    }
    if (opcode == s->read_register_opcode) {
        list = LIST_REGISTER;
    }
    if (s->has_memory_read && opcode == s->read_memory_opcode) {
        list = LIST_MEMORY;
    }
    return list;
}

// A byte of the image's address space: a held page's, or the filler.
static uint8_t command_byte(const struct mclw_command_image *s, uint16_t address) {
    return mclw_command_get_register(s, address);
}

static uint8_t command_response(const struct mclw_command_image *s, uint8_t opcode, uint32_t index) {
    for (uint8_t i = 0; i < s->response_count; i++) {
        if (s->responses[i].opcode == opcode) {
            return index < MCLW_COMMAND_RESPONSE_MAX ? s->responses[i].bytes[index] : s->filler;
        }
    }
    return s->filler;
}

static uint8_t command_miso(struct mclw_spi_target *t, uint32_t k) {
    const struct mclw_command_image *s = &t->shadow;
    uint32_t start;

    if (k == 0u || (k == 1u && !s->immediate_response)) {
        return command_status(t);
    }
    switch (t->list) {
        case LIST_RESPONSE:
            start = s->immediate_response ? 1u : 2u;
            return command_response(s, t->in[0], k - start);
        case LIST_BUFFER:
            return k < 3u ? command_status(t) : s->buffer[(uint8_t)(t->in[1] + (k - 3u))];
        case LIST_REGISTER:
            if (s->short_register_read) {
                return command_byte(s, (uint16_t)(t->in[1] + (k - 2u)));
            }
            return k < 4u ? command_status(t)
                          : command_byte(s, (uint16_t)(((uint16_t)t->in[1] << 8 | t->in[2]) + (k - 4u)));
        case LIST_MEMORY:
            return k < 4u ? command_status(t)
                          : command_byte(s, (uint16_t)(((uint16_t)t->in[2] << 8 | t->in[3]) + (k - 4u)));
        default:
            return command_status(t);
    }
}

static uint8_t regmap_miso(const struct mclw_spi_target *t, uint32_t k) {
    const struct mclw_regmap *map = t->map;
    uint8_t command = t->in[0];
    uint32_t dummies = t->framing.read_dummy_bytes;
    uint32_t address;

    if (k == 0u || (command & t->framing.read_bit_mask) == 0u || k - 1u < dummies) {
        return map->filler;
    }
    address = (uint32_t)(command & t->framing.address_mask) + (t->framing.auto_increment ? k - 1u - dummies : 0u);
    return address < MCLW_REGMAP_BANK_SIZE ? map->value[map->active_bank][address] : map->filler;
}

static void enqueue(struct mclw_spi_target *t, bool forced) {
    struct queued *q;
    uint32_t len = 1u;

    if (t->busy_gpio != NULL && t->queue_count != 0u) {
        t->stats.stale_windows++;
        return;
    }
    if (t->queue_count >= SPI_QUEUE) {
        t->stats.missed_windows++;
        t->stats.lost_windows++;
        return;
    }
    q = &t->queue[(t->queue_first + t->queue_count) % SPI_QUEUE];
    q->bytes[0] = t->in[0];
    switch (t->list) {
        case LIST_WRITE:
        case LIST_BUSY:
            len = t->count < MCLW_COMMAND_WINDOW_MAX + 1u ? t->count : MCLW_COMMAND_WINDOW_MAX + 1u;
            if (t->count > MCLW_COMMAND_WINDOW_MAX + 1u) {
                t->stats.long_windows++;
            }
            break;
        case LIST_BUFFER:
            len = t->count >= 2u ? 2u : 1u;
            break;
        case LIST_REGISTER:
            len = t->shadow.short_register_read ? (t->count >= 2u ? 2u : 1u) : (t->count >= 3u ? 3u : t->count);
            break;
        case LIST_MEMORY:
            len = t->count >= 4u ? 4u : t->count;
            break;
        default:
            break;
    }
    memcpy(q->bytes, t->in, len);
    q->len = (uint16_t)len;
    q->busy = t->busy;
    q->forced = forced;
    if (forced) {
        t->forced_pending++;
    }
    t->queue_count++;
    t->stats.transactions++;
    t->stats.bytes += len;
}

static void deliver(struct mclw_spi_target *t) {
    while (t->queue_count != 0u) {
        uint32_t state = mclw_hal_critical_enter();
        struct queued q = t->queue[t->queue_first];
        struct mclw_command_image *image = t->kind == IMAGE_COMMAND ? t->image : NULL;

        t->queue_first = (t->queue_first + 1u) % SPI_QUEUE;
        t->queue_count--;
        mclw_hal_critical_exit(state);
        if (image != NULL && image->on_window != NULL) {
            image->on_window(image, q.bytes, q.len, q.busy, image->on_window_ctx);
        }
        if (q.forced) {
            state = mclw_hal_critical_enter();
            if (t->forced_pending != 0u) {
                t->forced_pending--;
            }
            mclw_hal_critical_exit(state);
        }
    }
}

static void poll_targets(void *ctx) {
    (void)ctx;
    if (!mclw_hal_try_lock()) {
        return;
    }
    for (size_t i = 0; i < SPI_TARGETS_MAX; i++) {
        if (targets[i].owner != NULL && targets[i].queue_count != 0u) {
            deliver(&targets[i]);
        }
    }
    mclw_hal_unlock();
}

void mclw_hal_spi_target_poll(void) {
    poll_targets(NULL);
}

static void window_end(struct mclw_spi_target *t) {
    switch (t->kind) {
        case IMAGE_REGMAP: {
            uint8_t command = t->in[0];
            uint32_t dummies = t->framing.read_dummy_bytes;
            uint8_t address = (uint8_t)(command & t->framing.address_mask);

            if (t->count == 0u) {
                t->stats.short_windows++;
                break;
            }
            t->stats.transactions++;
            t->stats.bytes += t->count;
            if ((command & t->framing.read_bit_mask) != 0u) {
                uint32_t payload = t->count - 1u;
                uint32_t served = payload > dummies ? payload - dummies : 0u;

                if (address + served > MCLW_REGMAP_BANK_SIZE) {
                    t->stats.long_windows++;
                    served = MCLW_REGMAP_BANK_SIZE - address;
                }
                mclw_regmap_bus_read_burst(t->map, address, served, t->framing.auto_increment);
            } else {
                uint32_t payload = t->count - 1u;

                if (payload > MCLW_REGMAP_BANK_SIZE) {
                    t->stats.long_windows++;
                    payload = MCLW_REGMAP_BANK_SIZE;
                }
                mclw_regmap_bus_write_burst(t->map, address, &t->in[1], payload, t->framing.auto_increment);
            }
            break;
        }
        case IMAGE_STREAM: {
            struct mclw_stream *s = t->stream;

            t->stats.transactions++;
            t->stats.bytes += t->count;
            if (t->count < s->length) {
                t->stats.short_windows++;
            } else if (t->count > s->length) {
                t->stats.long_windows++;
            }
            if (s->direction == MCLW_STREAM_IN && s->on_frame != NULL && t->count != 0u) {
                s->on_frame(s, t->in, t->count < MCLW_STREAM_FRAME_MAX ? t->count : MCLW_STREAM_FRAME_MAX,
                    s->on_frame_ctx);
            }
            break;
        }
        case IMAGE_COMMAND:
            if (t->count == 0u) {
                t->stats.short_windows++;
                break;
            }
            enqueue(t, t->list == LIST_BUSY);
            break;
        default:
            break;
    }
}

// The DUT's chip select on `pin`: a fall opens a window, a rise ends it.
void mclw_host_spi_cs(uint8_t pin, bool level) {
    struct mclw_spi_target *ended = NULL;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SPI_TARGETS_MAX; i++) {
        struct mclw_spi_target *t = &targets[i];

        if (t->owner == NULL || t->cs != pin) {
            continue;
        }
        if (!level && !t->selected) {
            t->selected = true;
            t->count = 0;
            t->list = LIST_WRITE;
            if (t->kind == IMAGE_STREAM) {
                t->stream_frame = t->stream->active;
            }
        } else if (level && t->selected) {
            t->selected = false;
            if (t->kind != IMAGE_NONE) {
                window_end(t);
            }
            ended = t;
        }
    }
    mclw_hal_critical_exit(state);
    if (ended != NULL && ended->kind == IMAGE_COMMAND) {
        // As the rp2 engine raises BUSY within cycles of every release.
        if (ended->busy_gpio != NULL) {
            ended->busy = true;
            mclw_hal_gpio_write(ended->busy_gpio, true);
        }
        if (mclw_hal_try_lock()) {
            deliver(ended);
            mclw_hal_unlock();
        }
    }
}

// One frame on the bus with `sck`: the selected target's MISO byte, and
// the MOSI byte it takes. -ENODEV when no target there is selected.
int mclw_host_spi_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint8_t *miso) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_spi_target *t = NULL;

    for (size_t i = 0; i < SPI_TARGETS_MAX && t == NULL; i++) {
        if (targets[i].owner != NULL && targets[i].selected && targets[i].sck == sck &&
            targets[i].mosi == mosi_pin && targets[i].miso == miso_pin) {
            t = &targets[i];
        }
    }
    if (t == NULL) {
        mclw_hal_critical_exit(state);
        return -ENODEV;
    }
    uint32_t k = t->count;

    switch (t->kind) {
        case IMAGE_REGMAP:
            *miso = regmap_miso(t, k);
            break;
        case IMAGE_STREAM:
            *miso = t->stream->direction == MCLW_STREAM_OUT && k < t->stream->length
                ? t->stream->frame[t->stream_frame][k] : t->stream->filler;
            break;
        case IMAGE_COMMAND:
            *miso = command_miso(t, k);
            break;
        default:
            *miso = 0xff;
            break;
    }
    if (k < WINDOW_BYTES) {
        t->in[k] = mosi;
    }
    if (k == 0u && t->kind == IMAGE_COMMAND) {
        t->list = command_list(&t->shadow, mosi);
    }
    t->count = k + 1u;
    mclw_hal_critical_exit(state);
    return 0;
}

static void command_notify(struct mclw_command_image *image, void *ctx) {
    struct mclw_spi_target *t = ctx;
    uint32_t state = mclw_hal_critical_enter();

    t->shadow = *image;
    image->dirty = 0;
    mclw_hal_critical_exit(state);
}

struct mclw_spi_target *mclw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs,
    uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct mclw_spi_target *t = NULL;

    if (owner == NULL || mode > 3u) {
        *err = -EINVAL;
        return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_targets, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
    }
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < SPI_TARGETS_MAX && t == NULL; i++) {
        if (targets[i].owner == NULL) {
            t = &targets[i];
        }
    }
    mclw_hal_critical_exit(state);
    if (t == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    // A bus's three lines may be another target's too; only chip select is
    // this target's alone.
    *err = mclw_hal_pin_claim(cs, owner);
    if (*err != 0) {
        return NULL;
    }
    state = mclw_hal_critical_enter();
    memset(t, 0, sizeof(*t));
    t->sck = sck;
    t->mosi = mosi;
    t->miso = miso;
    t->cs = cs;
    t->mode = mode;
    t->msb_first = msb_first;
    t->owner = owner;
    mclw_hal_critical_exit(state);
    // The window is the DUT's chip select, as the emulator reports it.
    mclw_host_event_watch(cs);
    return t;
}

void mclw_hal_spi_target_close(struct mclw_spi_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    const void *owner = t->owner;

    mclw_hal_spi_target_disarm(t);
    uint32_t state = mclw_hal_critical_enter();
    t->owner = NULL;
    mclw_hal_critical_exit(state);
    mclw_hal_pin_release(t->cs, owner);
}

int mclw_hal_spi_target_set_mode(struct mclw_spi_target *t, uint8_t mode) {
    if (mode > 3u) {
        return -EINVAL;
    }
    t->mode = mode;
    return 0;
}

uint8_t mclw_hal_spi_target_mode(const struct mclw_spi_target *t) {
    return t->mode;
}

int mclw_hal_spi_target_arm_regmap(struct mclw_spi_target *t, struct mclw_regmap *map,
    const struct mclw_regmap_framing *framing) {
    if (t == NULL || t->owner == NULL || map == NULL || framing == NULL || framing->read_dummy_bytes > 1u) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    t->kind = IMAGE_REGMAP;
    t->map = map;
    t->framing = *framing;
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_stream(struct mclw_spi_target *t, struct mclw_stream *stream) {
    if (t == NULL || t->owner == NULL || stream == NULL || stream->length > MCLW_STREAM_FRAME_MAX) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    t->kind = IMAGE_STREAM;
    t->stream = stream;
    stream->notify = NULL;
    stream->notify_ctx = NULL;
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_command(struct mclw_spi_target *t, struct mclw_command_image *image,
    struct mclw_gpio *busy) {
    if (t == NULL || t->owner == NULL || image == NULL) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    t->kind = IMAGE_COMMAND;
    t->image = image;
    t->busy_gpio = busy;
    t->busy = false;
    t->forced_pending = 0;
    t->queue_count = 0;
    image->notify = command_notify;
    image->notify_ctx = t;
    t->shadow = *image;
    image->dirty = 0;
    mclw_hal_critical_exit(state);
    if (busy != NULL) {
        mclw_hal_gpio_write(busy, false);
    }
    return 0;
}

void mclw_hal_spi_target_set_busy(struct mclw_spi_target *t, bool busy) {
    t->busy = busy;
    if (t->busy_gpio != NULL) {
        mclw_hal_gpio_write(t->busy_gpio, busy);
    }
}

void mclw_hal_spi_target_disarm(struct mclw_spi_target *t) {
    uint32_t state = mclw_hal_critical_enter();

    if (t->kind == IMAGE_COMMAND && t->image != NULL) {
        t->image->notify = NULL;
        t->image->notify_ctx = NULL;
    }
    t->kind = IMAGE_NONE;
    t->map = NULL;
    t->stream = NULL;
    t->image = NULL;
    t->busy_gpio = NULL;
    t->queue_count = 0;
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_target_stats(const struct mclw_spi_target *t, struct mclw_spi_target_stats *out) {
    uint32_t state = mclw_hal_critical_enter();

    *out = t->stats;
    mclw_hal_critical_exit(state);
}

// No clock limit of its own: the frames come one at a time.
uint32_t mclw_hal_spi_target_max_clock_hz(const struct mclw_spi_target *t) {
    (void)t;
    return 0;
}
