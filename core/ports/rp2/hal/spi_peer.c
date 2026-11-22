// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SPI peers: one PIO state machine and two DMA channels each.
//
//   target      waits for chip select low, then for each frame pulls the
//               next one, shifts it out on MISO a bit at a time and samples
//               MOSI between, on SCK's edges as the mode has them (mode 0:
//               out ahead of each rise, sampled on it); after a frame, a
//               chip select still low starts the next, a high one ends the
//               window. Both channels run on 256-byte rings for as long as
//               the peer is open, so frame k shifts out ring frame k and
//               its MOSI frame lands at the same place.
//   controller  for each frame pulls a word the CPU staged, drops chip
//               select (a no-op inside a window), clocks the frame's bits
//               with SCK on the side-set pin (in mode 0 MOSI set up while
//               SCK is idle, MISO sampled as it goes active), then shifts
//               out the word's window flag: set, it raises chip select and
//               keeps it high for a bit time before the next window.
//
// A frame through a narrow DMA write lands in every lane of the TX FIFO
// word, as the bus fabric replicates narrow writes (pico-examples'
// pio/spi/pio_spi.c relies on it the same way): with the OUT shift to the
// left (MSB first) its top lane goes out, to the right (LSB first) its low
// one. IN pushes at the frame's width; MSB first the frame is the word's low
// lane, LSB first its top lane, which a narrow read of the FIFO at that
// lane's address takes. A 16-bit frame's DMA swaps its bytes, so memory
// holds it high byte first.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/spi_peer.h"
#include "gpio_internal.h"
#include "pio_window.h"

#define RING_BITS 8u
// A count no transfer here runs out of: the RP2350's TRANS_COUNT is 28
// bits under its mode field, and 2^28 frames is over half an hour of
// back-to-back 8-bit frames at 1 MHz.
#define TARGET_DMA_COUNT 0x0fffffffu

#define TARGET_LENGTH 11u
#define TARGET_FRAME 2u
#define TARGET_BIT 4u
#define CONTROLLER_LENGTH 9u
#define CONTROLLER_BIT 3u
// Cycles per SCK half period: OUT [3] in one, IN [2] and JMP in the other.
#define CONTROLLER_HALF 4u
#define CONTROLLER_DELAY 3u
#define CONTROLLER_GAP 7u

struct mclw_spi_peer {
    bool open;
    bool controller;
    struct mclw_spi_peer_format format;
    uint32_t hz;
    struct mclw_gpio *pins[4];
    PIO pio;
    uint8_t sm;
    uint8_t offset;
    int8_t tx_dma;
    int8_t rx_dma;
    uint16_t words[TARGET_LENGTH];
    pio_program_t program;
    // The controller's frames before the transfer in progress, and its
    // length.
    uint32_t done;
    uint32_t length;
};

enum {
    PIN_SCK,
    PIN_MOSI,
    PIN_MISO,
    PIN_CS,
};

static struct mclw_spi_peer slots[2];
static uint8_t tx_ring[MCLW_SPI_PEER_RING] __attribute__((aligned(MCLW_SPI_PEER_RING)));
static uint8_t rx_ring[MCLW_SPI_PEER_RING] __attribute__((aligned(MCLW_SPI_PEER_RING)));
// The controller's frames as the program shifts them: the frame, then the
// window flag.
static uint32_t staged[MCLW_SPI_PEER_FRAMES_MAX];

static bool mode_cpol(uint8_t mode) {
    return (mode & 2u) != 0u;
}

static bool mode_cpha(uint8_t mode) {
    return (mode & 1u) != 0u;
}

static bool has_miso(const struct mclw_spi_peer *p) {
    return p->pins[PIN_MISO] != NULL;
}

// A bit: SCK's first edge takes it to the active level (!CPOL), its second
// back to idle. CPHA 0 shifts out ahead of the first edge and samples on
// it; CPHA 1 shifts out on the first and samples on the second. With no
// MISO the bit goes nowhere.
static void encode_target(struct mclw_spi_peer *p) {
    uint sck = p->pins[PIN_SCK]->pin % 32u;
    uint cs = p->pins[PIN_CS]->pin % 32u;
    bool active = !mode_cpol(p->format.mode);
    uint16_t out = (uint16_t)(has_miso(p) ? pio_encode_out(pio_pins, 1u) : pio_encode_out(pio_null, 1u));
    uint16_t *w = p->words;

    w[0] = (uint16_t)pio_encode_wait_gpio(true, cs);
    w[1] = (uint16_t)pio_encode_wait_gpio(false, cs);
    w[2] = (uint16_t)pio_encode_pull(false, true);
    w[3] = (uint16_t)pio_encode_set(pio_x, p->format.bits - 1u);
    if (mode_cpha(p->format.mode)) {
        w[4] = (uint16_t)pio_encode_wait_gpio(active, sck);
        w[5] = out;
        w[6] = (uint16_t)pio_encode_wait_gpio(!active, sck);
        w[7] = (uint16_t)pio_encode_in(pio_pins, 1u);
    } else {
        w[4] = out;
        w[5] = (uint16_t)pio_encode_wait_gpio(active, sck);
        w[6] = (uint16_t)pio_encode_in(pio_pins, 1u);
        w[7] = (uint16_t)pio_encode_wait_gpio(!active, sck);
    }
    w[8] = (uint16_t)pio_encode_jmp_x_dec(TARGET_BIT);
    // Chip select high (the JMP pin): the window is over.
    w[9] = (uint16_t)pio_encode_jmp_pin(0u);
    w[10] = (uint16_t)pio_encode_jmp(TARGET_FRAME);
    p->program.length = TARGET_LENGTH;
}

static uint16_t sck_side(uint value) {
    return (uint16_t)pio_encode_sideset(1u, value);
}

// Each bit is the OUT's half period then the IN's and the JMP's: CPHA 0
// holds SCK idle through the OUT and takes it active as IN samples, CPHA 1
// takes it active with the OUT and back to idle as IN samples. A frame's
// window flag clear, the next frame follows under the same chip select.
static void encode_controller(struct mclw_spi_peer *p) {
    uint idle = mode_cpol(p->format.mode) ? 1u : 0u;
    uint out_level = mode_cpha(p->format.mode) ? !idle : idle;
    uint in_level = mode_cpha(p->format.mode) ? idle : !idle;
    uint16_t in = (uint16_t)(has_miso(p) ? pio_encode_in(pio_pins, 1u) : pio_encode_in(pio_null, 1u));
    uint16_t *w = p->words;

    w[0] = (uint16_t)pio_encode_pull(false, true) | sck_side(idle);
    w[1] = (uint16_t)(pio_encode_set(pio_pins, 0u) | sck_side(idle) | pio_encode_delay(CONTROLLER_DELAY));
    w[2] = (uint16_t)pio_encode_set(pio_x, p->format.bits - 1u) | sck_side(idle);
    w[3] = (uint16_t)(pio_encode_out(pio_pins, 1u) | sck_side(out_level) | pio_encode_delay(CONTROLLER_DELAY));
    w[4] = (uint16_t)(in | sck_side(in_level) | pio_encode_delay(CONTROLLER_DELAY - 1u));
    w[5] = (uint16_t)pio_encode_jmp_x_dec(CONTROLLER_BIT) | sck_side(in_level);
    w[6] = (uint16_t)pio_encode_out(pio_x, 1u) | sck_side(idle);
    w[7] = (uint16_t)pio_encode_jmp_not_x(0u) | sck_side(idle);
    w[8] = (uint16_t)(pio_encode_set(pio_pins, 1u) | sck_side(idle) | pio_encode_delay(CONTROLLER_GAP));
    p->program.length = CONTROLLER_LENGTH;
}

static void encode(struct mclw_spi_peer *p) {
    if (p->controller) {
        encode_controller(p);
    } else {
        encode_target(p);
    }
}

static uint64_t pin_mask(const struct mclw_spi_peer *p) {
    uint64_t mask = 0u;

    for (size_t i = 0; i < 4u; i++) {
        if (p->pins[i] != NULL) {
            mask |= MCLW_PIO_PIN(p->pins[i]->pin);
        }
    }
    return mask;
}

static int acquire(struct mclw_spi_peer *p) {
    for (size_t n = 0; n < MCLW_PIO_TRIES; n++) {
        PIO pio = mclw_pio_try(n);
        if (pio == NULL || !pio_can_add_program(pio, &p->program) || !mclw_pio_window(pio, pin_mask(p))) {
            continue;
        }
        int sm = pio_claim_unused_sm(pio, false);
        if (sm < 0) {
            continue;
        }
        p->pio = pio;
        p->sm = (uint8_t)sm;
        p->offset = (uint8_t)pio_add_program(pio, &p->program);
        return 0;
    }
    return -ENOSPC;
}

static void release(struct mclw_spi_peer *p) {
    pio_sm_set_enabled(p->pio, p->sm, false);
    pio_remove_program(p->pio, &p->program, p->offset);
    pio_sm_unclaim(p->pio, p->sm);
    if (p->tx_dma >= 0) {
        dma_channel_abort((uint)p->tx_dma);
        dma_channel_unclaim((uint)p->tx_dma);
    }
    if (p->rx_dma >= 0) {
        dma_channel_abort((uint)p->rx_dma);
        dma_channel_unclaim((uint)p->rx_dma);
    }
    for (size_t i = 0; i < 4u; i++) {
        if (p->pins[i] != NULL) {
            p->pins[i]->held = false;
            mclw_rp2_gpio_restore(p->pins[i]);
        }
    }
    p->open = false;
}

static void configure(struct mclw_spi_peer *p) {
    pio_sm_config c = pio_get_default_sm_config();
    uint sck = p->pins[PIN_SCK]->pin;
    uint mosi = p->pins[PIN_MOSI]->pin;
    uint cs = p->pins[PIN_CS]->pin;
    bool right = p->format.lsb_first;

    sm_config_set_wrap(&c, p->offset, p->offset + p->program.length - 1u);
    sm_config_set_out_shift(&c, right, false, 32u);
    sm_config_set_in_shift(&c, right, true, p->format.bits);
    if (p->controller) {
        uint64_t outputs = MCLW_PIO_PIN(sck) | MCLW_PIO_PIN(mosi) | MCLW_PIO_PIN(cs);
        uint64_t high = MCLW_PIO_PIN(cs) | (mode_cpol(p->format.mode) ? MCLW_PIO_PIN(sck) : 0u);

        sm_config_set_sideset(&c, 1u, false, false);
        sm_config_set_sideset_pins(&c, sck);
        sm_config_set_set_pins(&c, cs, 1u);
        sm_config_set_out_pins(&c, mosi, 1u);
        if (has_miso(p)) {
            sm_config_set_in_pins(&c, p->pins[PIN_MISO]->pin);
        }
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)(2u * CONTROLLER_HALF * p->hz));
        pio_sm_set_pins_with_mask64(p->pio, p->sm, high, outputs);
        pio_sm_set_pindirs_with_mask64(p->pio, p->sm, outputs, outputs);
        pio_gpio_init(p->pio, sck);
        pio_gpio_init(p->pio, mosi);
        pio_gpio_init(p->pio, cs);
    } else {
        sm_config_set_in_pins(&c, mosi);
        sm_config_set_jmp_pin(&c, cs);
        if (has_miso(p)) {
            uint miso = p->pins[PIN_MISO]->pin;

            sm_config_set_out_pins(&c, miso, 1u);
            pio_sm_set_pins_with_mask64(p->pio, p->sm, 0u, MCLW_PIO_PIN(miso));
            pio_sm_set_pindirs_with_mask64(p->pio, p->sm, MCLW_PIO_PIN(miso), MCLW_PIO_PIN(miso));
            pio_gpio_init(p->pio, miso);
        }
    }
    pio_sm_init(p->pio, p->sm, p->offset, &c);
}

static enum dma_channel_transfer_size frame_size(const struct mclw_spi_peer *p) {
    return p->format.bits == 16u ? DMA_SIZE_16 : DMA_SIZE_8;
}

// The RX FIFO lane a frame sits in: the low one MSB first, the top one LSB
// first.
static const volatile void *rx_lane(const struct mclw_spi_peer *p) {
    const volatile uint8_t *f = (const volatile uint8_t *)&p->pio->rxf[p->sm];

    return p->format.lsb_first ? f + (p->format.bits == 16u ? 2u : 3u) : f;
}

static void start_target_dma(struct mclw_spi_peer *p) {
    dma_channel_config c = dma_channel_get_default_config((uint)p->tx_dma);

    channel_config_set_transfer_data_size(&c, frame_size(p));
    channel_config_set_bswap(&c, p->format.bits == 16u);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, true));
    dma_channel_configure((uint)p->tx_dma, &c, &p->pio->txf[p->sm], tx_ring, TARGET_DMA_COUNT, true);

    c = dma_channel_get_default_config((uint)p->rx_dma);
    channel_config_set_transfer_data_size(&c, frame_size(p));
    channel_config_set_bswap(&c, p->format.bits == 16u);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, false));
    dma_channel_configure((uint)p->rx_dma, &c, rx_ring, rx_lane(p), TARGET_DMA_COUNT, true);
}

static bool pins_free(struct mclw_gpio *const pins[4]) {
    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] == NULL) {
            if (i != PIN_MISO) {
                return false;
            }
            continue;
        }
        if (pins[i]->link != NULL || pins[i]->square || pins[i]->held) {
            return false;
        }
        for (size_t j = i + 1u; j < 4u; j++) {
            if (pins[j] != NULL && pins[i]->pin == pins[j]->pin) {
                return false;
            }
        }
    }
    return true;
}

static bool format_ok(const struct mclw_spi_peer_format *f) {
    return f->mode <= 3u && (f->bits == 8u || f->bits == 16u);
}

static struct mclw_spi_peer *open_peer(bool controller, struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    struct mclw_spi_peer *p = &slots[controller ? 1 : 0];
    struct mclw_gpio *const pins[4] = { sck, mosi, miso, cs };
    // What each role drives: the target only MISO, the controller the rest.
    const bool drives[4] = { controller, controller, !controller, controller };

    if (err == NULL) {
        return NULL;
    }
    if (!pins_free(pins) || (controller && hz == 0u)) {
        *err = -EINVAL;
        return NULL;
    }
    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] != NULL && pins[i]->output != drives[i]) {
            *err = -EINVAL;
            return NULL;
        }
    }
    if (p->open) {
        *err = -EBUSY;
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    memcpy(p->pins, pins, sizeof(p->pins));
    p->controller = controller;
    p->hz = hz;
    p->format = (struct mclw_spi_peer_format){ .mode = 0u, .bits = 8u, .lsb_first = false };
    p->program.instructions = p->words;
    p->program.origin = -1;
    p->tx_dma = -1;
    p->rx_dma = -1;
    encode(p);
    *err = acquire(p);
    if (*err != 0) {
        return NULL;
    }
    p->tx_dma = (int8_t)dma_claim_unused_channel(false);
    p->rx_dma = (int8_t)dma_claim_unused_channel(false);
    p->open = true;
    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] != NULL) {
            pins[i]->held = true;
        }
    }
    if (p->tx_dma < 0 || p->rx_dma < 0) {
        release(p);
        *err = -ENOSPC;
        return NULL;
    }
    configure(p);
    if (!controller) {
        memset(tx_ring, 0, sizeof(tx_ring));
        memset(rx_ring, 0, sizeof(rx_ring));
        start_target_dma(p);
    }
    pio_sm_set_enabled(p->pio, p->sm, true);
    return p;
}

struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err) {
    return open_peer(false, sck, mosi, miso, cs, 0u, err);
}

struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    return open_peer(true, sck, mosi, miso, cs, hz, err);
}

void mclw_hal_spi_peer_close(struct mclw_spi_peer *p) {
    if (p != NULL && p->open) {
        release(p);
    }
}

void mclw_hal_spi_peer_restart(struct mclw_spi_peer *p) {
    if (p == NULL || !p->open || p->controller) {
        return;
    }
    pio_sm_set_enabled(p->pio, p->sm, false);
    dma_channel_abort((uint)p->tx_dma);
    dma_channel_abort((uint)p->rx_dma);
    pio_sm_clear_fifos(p->pio, p->sm);
    pio_sm_restart(p->pio, p->sm);
    pio_sm_exec(p->pio, p->sm, pio_encode_jmp(p->offset));
    start_target_dma(p);
    pio_sm_set_enabled(p->pio, p->sm, true);
}

// The program is the same length in every format, so it goes back where it
// was.
int mclw_hal_spi_peer_set_format(struct mclw_spi_peer *p, const struct mclw_spi_peer_format *format) {
    if (p == NULL || !p->open || format == NULL || !format_ok(format)) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(p)) {
        return -EBUSY;
    }
    if (memcmp(format, &p->format, sizeof(*format)) == 0) {
        return 0;
    }
    pio_sm_set_enabled(p->pio, p->sm, false);
    if (!p->controller) {
        dma_channel_abort((uint)p->tx_dma);
        dma_channel_abort((uint)p->rx_dma);
    }
    pio_remove_program(p->pio, &p->program, p->offset);
    p->format = *format;
    encode(p);
    pio_add_program_at_offset(p->pio, &p->program, p->offset);
    pio_sm_clear_fifos(p->pio, p->sm);
    configure(p);
    if (!p->controller) {
        start_target_dma(p);
    }
    pio_sm_set_enabled(p->pio, p->sm, true);
    return 0;
}

void mclw_hal_spi_peer_load(struct mclw_spi_peer *p, const uint8_t *bytes, size_t len) {
    if (p == NULL || !p->open || p->controller) {
        return;
    }
    if (len > MCLW_SPI_PEER_RING) {
        len = MCLW_SPI_PEER_RING;
    }
    memset(tx_ring, 0, sizeof(tx_ring));
    memcpy(tx_ring, bytes, len);
    mclw_hal_spi_peer_restart(p);
}

void mclw_hal_spi_peer_received(const struct mclw_spi_peer *p, size_t at, uint8_t *bytes, size_t len) {
    (void)p;
    for (size_t i = 0; i < len; i++) {
        bytes[i] = rx_ring[(at + i) % MCLW_SPI_PEER_RING];
    }
}

// A frame as the program shifts it: MSB first its bits from the word's top,
// then the window flag; LSB first from the bottom, then the flag.
static uint32_t stage_word(const struct mclw_spi_peer *p, uint32_t frame, bool last) {
    uint32_t bits = p->format.bits;

    if (p->format.lsb_first) {
        return frame | ((last ? 1u : 0u) << bits);
    }
    return (frame << (32u - bits)) | ((last ? 1u : 0u) << (31u - bits));
}

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *p, const uint8_t *out, uint8_t *in, size_t frames,
    size_t window) {
    dma_channel_config c;
    size_t width = p != NULL ? p->format.bits / 8u : 1u;

    if (p == NULL || !p->open || !p->controller || frames == 0u || frames > MCLW_SPI_PEER_FRAMES_MAX ||
        window == 0u) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(p)) {
        return -EBUSY;
    }
    for (size_t i = 0; i < frames; i++) {
        uint32_t frame = width == 2u ? ((uint32_t)out[2u * i] << 8) | out[2u * i + 1u] : out[i];

        staged[i] = stage_word(p, frame, (i + 1u) % window == 0u || i + 1u == frames);
    }
    p->done = mclw_hal_spi_peer_count(p);
    p->length = (uint32_t)frames;

    c = dma_channel_get_default_config((uint)p->rx_dma);
    channel_config_set_transfer_data_size(&c, frame_size(p));
    channel_config_set_bswap(&c, p->format.bits == 16u);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, false));
    dma_channel_configure((uint)p->rx_dma, &c, in, rx_lane(p), frames, true);

    c = dma_channel_get_default_config((uint)p->tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, true));
    dma_channel_configure((uint)p->tx_dma, &c, &p->pio->txf[p->sm], staged, frames, true);
    return 0;
}

bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *p) {
    return p != NULL && p->open && p->controller && dma_channel_is_busy((uint)p->rx_dma);
}

uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *p) {
    uint32_t left;

    if (p == NULL || !p->open) {
        return 0u;
    }
    left = dma_channel_hw_addr((uint)p->rx_dma)->transfer_count & TARGET_DMA_COUNT;
    if (p->controller) {
        return p->done + (p->length - (left < p->length ? left : p->length));
    }
    return TARGET_DMA_COUNT - left;
}
