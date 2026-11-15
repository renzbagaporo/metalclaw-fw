// SPDX-License-Identifier: Apache-2.0 OR MIT
// The byte-per-window SPI peers: one PIO state machine and two DMA
// channels each.
//
//   target      waits for chip select low, pulls the next byte, shifts it
//               out on MISO a bit per SCK fall while sampling MOSI on every
//               rise, and goes back to waiting for chip select high. Both
//               channels run on 256-byte rings for as long as the peer is
//               open, so window k shifts out byte k mod 256 of the loaded
//               ring and its MOSI byte lands at the same place.
//   controller  pulls a byte, drops chip select, clocks eight bits with
//               SCK on the side-set pin (MOSI set up while SCK is low, MISO
//               sampled as it rises), raises chip select and keeps it high
//               for a bit time before the next byte.
//
// A byte through an 8-bit DMA write lands in every lane of the TX FIFO
// word, as the bus fabric replicates narrow writes (pico-examples'
// pio/spi/pio_spi.c relies on it the same way), so with the OUT shift to
// the left its top byte goes out MSB first; IN shifts left and
// pushes at 8 bits, so the received byte is the word's low byte.

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
// bits under its mode field, and 2^28 bytes is over half an hour of
// back-to-back windows at 1 MHz.
#define TARGET_DMA_COUNT 0x0fffffffu

#define TARGET_LENGTH 9u
#define TARGET_BIT 4u
#define CONTROLLER_LENGTH 7u
#define CONTROLLER_BIT 3u
// Cycles per SCK half period: OUT [3] low, IN [2] and JMP high.
#define CONTROLLER_HALF 4u
#define CONTROLLER_DELAY 3u
#define CONTROLLER_GAP 7u

struct mclw_spi_peer {
    bool open;
    bool controller;
    struct mclw_gpio *pins[4];
    PIO pio;
    uint8_t sm;
    uint8_t offset;
    int8_t tx_dma;
    int8_t rx_dma;
    uint16_t words[TARGET_LENGTH];
    pio_program_t program;
    // The controller's bytes before the transfer in progress, and its length.
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

static void encode_target(struct mclw_spi_peer *p) {
    uint sck = p->pins[PIN_SCK]->pin % 32u;
    uint cs = p->pins[PIN_CS]->pin % 32u;
    uint16_t *w = p->words;

    w[0] = (uint16_t)pio_encode_wait_gpio(true, cs);
    w[1] = (uint16_t)pio_encode_wait_gpio(false, cs);
    w[2] = (uint16_t)pio_encode_pull(false, true);
    w[3] = (uint16_t)pio_encode_set(pio_x, 7u);
    w[4] = (uint16_t)pio_encode_out(pio_pins, 1u);
    w[5] = (uint16_t)pio_encode_wait_gpio(true, sck);
    w[6] = (uint16_t)pio_encode_in(pio_pins, 1u);
    w[7] = (uint16_t)pio_encode_wait_gpio(false, sck);
    w[8] = (uint16_t)pio_encode_jmp_x_dec(TARGET_BIT);
    p->program.length = TARGET_LENGTH;
}

static uint16_t sck_side(uint value) {
    return (uint16_t)pio_encode_sideset(1u, value);
}

static void encode_controller(struct mclw_spi_peer *p) {
    uint16_t *w = p->words;

    w[0] = (uint16_t)pio_encode_pull(false, true) | sck_side(0);
    w[1] = (uint16_t)(pio_encode_set(pio_pins, 0u) | sck_side(0) | pio_encode_delay(CONTROLLER_DELAY));
    w[2] = (uint16_t)pio_encode_set(pio_x, 7u) | sck_side(0);
    w[3] = (uint16_t)(pio_encode_out(pio_pins, 1u) | sck_side(0) | pio_encode_delay(CONTROLLER_DELAY));
    w[4] = (uint16_t)(pio_encode_in(pio_pins, 1u) | sck_side(1) | pio_encode_delay(CONTROLLER_DELAY - 1u));
    w[5] = (uint16_t)pio_encode_jmp_x_dec(CONTROLLER_BIT) | sck_side(1);
    w[6] = (uint16_t)(pio_encode_set(pio_pins, 1u) | sck_side(0) | pio_encode_delay(CONTROLLER_GAP));
    p->program.length = CONTROLLER_LENGTH;
}

static uint64_t pin_mask(const struct mclw_spi_peer *p) {
    uint64_t mask = 0u;

    for (size_t i = 0; i < 4u; i++) {
        mask |= MCLW_PIO_PIN(p->pins[i]->pin);
    }
    return mask;
}

// PIO0 first, then a third block if the chip has one; PIO1 last, since the
// SPI target engine needs all of its instruction memory.
static int acquire(struct mclw_spi_peer *p) {
    static const uint8_t order[] = { 0u, 2u, 1u };

    for (size_t i = 0; i < sizeof(order); i++) {
        if (order[i] >= NUM_PIOS) {
            continue;
        }
        PIO pio = pio_get_instance(order[i]);
        if (!pio_can_add_program(pio, &p->program) || !mclw_pio_window(pio, pin_mask(p))) {
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
        p->pins[i]->held = false;
        mclw_rp2_gpio_restore(p->pins[i]);
    }
    p->open = false;
}

static void configure(struct mclw_spi_peer *p, uint32_t hz) {
    pio_sm_config c = pio_get_default_sm_config();
    uint sck = p->pins[PIN_SCK]->pin;
    uint mosi = p->pins[PIN_MOSI]->pin;
    uint miso = p->pins[PIN_MISO]->pin;
    uint cs = p->pins[PIN_CS]->pin;

    sm_config_set_wrap(&c, p->offset, p->offset + p->program.length - 1u);
    sm_config_set_out_shift(&c, false, false, 32u);
    sm_config_set_in_shift(&c, false, true, 8u);
    if (p->controller) {
        uint64_t outputs = MCLW_PIO_PIN(sck) | MCLW_PIO_PIN(mosi) | MCLW_PIO_PIN(cs);

        sm_config_set_sideset(&c, 1u, false, false);
        sm_config_set_sideset_pins(&c, sck);
        sm_config_set_set_pins(&c, cs, 1u);
        sm_config_set_out_pins(&c, mosi, 1u);
        sm_config_set_in_pins(&c, miso);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)(2u * CONTROLLER_HALF * hz));
        pio_sm_set_pins_with_mask64(p->pio, p->sm, MCLW_PIO_PIN(cs), outputs);
        pio_sm_set_pindirs_with_mask64(p->pio, p->sm, outputs, outputs);
        pio_gpio_init(p->pio, sck);
        pio_gpio_init(p->pio, mosi);
        pio_gpio_init(p->pio, cs);
    } else {
        sm_config_set_out_pins(&c, miso, 1u);
        sm_config_set_in_pins(&c, mosi);
        pio_sm_set_pins_with_mask64(p->pio, p->sm, 0u, MCLW_PIO_PIN(miso));
        pio_sm_set_pindirs_with_mask64(p->pio, p->sm, MCLW_PIO_PIN(miso), MCLW_PIO_PIN(miso));
        pio_gpio_init(p->pio, miso);
    }
    pio_sm_init(p->pio, p->sm, p->offset, &c);
}

static void start_target_dma(struct mclw_spi_peer *p) {
    dma_channel_config c = dma_channel_get_default_config((uint)p->tx_dma);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, true));
    dma_channel_configure((uint)p->tx_dma, &c, &p->pio->txf[p->sm], tx_ring, TARGET_DMA_COUNT, true);

    c = dma_channel_get_default_config((uint)p->rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, false));
    dma_channel_configure((uint)p->rx_dma, &c, rx_ring, &p->pio->rxf[p->sm], TARGET_DMA_COUNT, true);
}

static bool pins_free(struct mclw_gpio *const pins[4]) {
    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] == NULL || pins[i]->link != NULL || pins[i]->square || pins[i]->held) {
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
        if (pins[i]->output != drives[i]) {
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
    p->program.instructions = p->words;
    p->program.origin = -1;
    p->tx_dma = -1;
    p->rx_dma = -1;
    if (controller) {
        encode_controller(p);
    } else {
        encode_target(p);
    }
    *err = acquire(p);
    if (*err != 0) {
        return NULL;
    }
    p->tx_dma = (int8_t)dma_claim_unused_channel(false);
    p->rx_dma = (int8_t)dma_claim_unused_channel(false);
    p->open = true;
    for (size_t i = 0; i < 4u; i++) {
        pins[i]->held = true;
    }
    if (p->tx_dma < 0 || p->rx_dma < 0) {
        release(p);
        *err = -ENOSPC;
        return NULL;
    }
    configure(p, hz);
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

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *p, const uint8_t *out, uint8_t *in, size_t len) {
    dma_channel_config c;

    if (p == NULL || !p->open || !p->controller || len == 0u) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(p)) {
        return -EBUSY;
    }
    p->done = mclw_hal_spi_peer_count(p);
    p->length = (uint32_t)len;

    c = dma_channel_get_default_config((uint)p->rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, false));
    dma_channel_configure((uint)p->rx_dma, &c, in, &p->pio->rxf[p->sm], len, true);

    c = dma_channel_get_default_config((uint)p->tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm, true));
    dma_channel_configure((uint)p->tx_dma, &c, &p->pio->txf[p->sm], out, len, true);
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
