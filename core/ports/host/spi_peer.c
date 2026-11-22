// SPDX-License-Identifier: Apache-2.0 OR MIT
// SPI peers for the host port (metalclaw/hal/spi_peer.h). A target answers
// the DUT's controller a frame at a time: the emulator hands over each
// frame of the DUT's SPI block, whatever its chip select does between
// them. A controller clocks a DUT whose block is the target a frame at a
// time at its own rate: each frame an spi_clock action (with `cs_hold` for
// one its window continues after), each answer an spi_clocked request.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_peer.h"

#include "host.h"

#define PEERS_MAX 2u

struct mclw_spi_peer {
    bool open;
    struct mclw_spi_peer_format format;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
    uint8_t tx[MCLW_SPI_PEER_RING];
    uint8_t rx[MCLW_SPI_PEER_RING];
    uint32_t count;
    // Controller: the transfer in flight, the frame period, and how many
    // frames have come back.
    bool controller;
    uint32_t frame_ns;
    uint8_t *in;
    size_t len;
    size_t answered;
};

static struct mclw_spi_peer peers[PEERS_MAX];

static uint16_t reverse(uint16_t v, uint8_t bits) {
    uint16_t r = 0;

    for (uint8_t i = 0; i < bits; i++) {
        r = (uint16_t)((r << 1) | ((v >> i) & 1u));
    }
    return r;
}

// A frame's value as the peer's ring holds it, high byte first.
static uint16_t ring_frame(const uint8_t *ring, uint32_t at, uint8_t bits) {
    return bits == 16u ? (uint16_t)((ring[at] << 8) | ring[(at + 1u) % MCLW_SPI_PEER_RING]) : ring[at];
}

static void ring_store(uint8_t *ring, uint32_t at, uint8_t bits, uint16_t v) {
    if (bits == 16u) {
        ring[at] = (uint8_t)(v >> 8);
        ring[(at + 1u) % MCLW_SPI_PEER_RING] = (uint8_t)v;
    } else {
        ring[at] = (uint8_t)v;
    }
}

// The emulator's frame is what the DUT's block shifted, in its own bit
// order: a peer set LSB first sees it reversed against an MSB-first DUT, as
// it would on the wire. A frame with no MISO pin (a half-duplex DUT), or a
// peer with none, matches on SCK and MOSI alone.
bool mclw_host_spi_peer_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint16_t mosi, uint8_t bits,
    uint16_t *miso) {
    bool taken = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < PEERS_MAX && !taken; i++) {
        struct mclw_spi_peer *p = &peers[i];

        if (!p->open || p->controller || p->sck != sck || p->mosi != mosi_pin ||
            (miso_pin != MCLW_PIN_NONE && p->miso != MCLW_PIN_NONE && p->miso != miso_pin) ||
            bits != p->format.bits) {
            continue;
        }
        uint32_t width = bits / 8u;
        uint32_t at = (p->count * width) % MCLW_SPI_PEER_RING;
        uint16_t out = ring_frame(p->tx, at, bits);

        *miso = p->miso == MCLW_PIN_NONE ? (uint16_t)(bits == 16u ? 0xffffu : 0xffu) :
            (p->format.lsb_first ? reverse(out, bits) : out);
        ring_store(p->rx, at, bits, p->format.lsb_first ? reverse(mosi, bits) : mosi);
        p->count++;
        taken = true;
    }
    mclw_hal_critical_exit(state);
    return taken;
}

struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err) {
    struct mclw_spi_peer *p = NULL;

    (void)cs;
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < PEERS_MAX && p == NULL; i++) {
        if (!peers[i].open) {
            p = &peers[i];
        }
    }
    if (p != NULL) {
        memset(p, 0, sizeof(*p));
        p->sck = mclw_hal_gpio_pin(sck);
        p->mosi = mclw_hal_gpio_pin(mosi);
        p->miso = miso != NULL ? mclw_hal_gpio_pin(miso) : MCLW_PIN_NONE;
        p->format = (struct mclw_spi_peer_format){ .mode = 0u, .bits = 8u, .lsb_first = false };
        p->open = true;
    }
    mclw_hal_critical_exit(state);
    if (p == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    // The pins are the DUT's SPI block's, which the emulator serves by
    // frame: the gpio bindings that hold them drive nothing.
    if (miso != NULL) {
        mclw_hal_gpio_release(miso);
    }
    *err = 0;
    return p;
}

void mclw_host_spi_peer_clocked(uint8_t sck, int miso) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < PEERS_MAX; i++) {
        struct mclw_spi_peer *p = &peers[i];

        if (!p->open || !p->controller || p->sck != sck || p->answered >= p->len) {
            continue;
        }
        if (p->in != NULL) {
            uint16_t v = miso >= 0 ? (uint16_t)miso : 0xffffu;
            uint8_t bits = p->format.bits;

            if (p->miso == MCLW_PIN_NONE) {
                v = 0xffffu;
            } else if (p->format.lsb_first) {
                v = reverse(v, bits);
            }
            if (bits == 16u) {
                p->in[2u * p->answered] = (uint8_t)(v >> 8);
                p->in[2u * p->answered + 1u] = (uint8_t)v;
            } else {
                p->in[p->answered] = (uint8_t)v;
            }
        }
        p->answered++;
        p->count++;
        break;
    }
    mclw_hal_critical_exit(state);
}

struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    struct mclw_spi_peer *p;

    p = mclw_hal_spi_peer_target(sck, mosi, miso, cs, err);
    if (p == NULL) {
        return NULL;
    }
    // A byte's eight clocks and as long again between windows.
    p->controller = true;
    p->frame_ns = hz != 0u ? (uint32_t)(16000000000ull / hz) : 16000u;
    return p;
}

void mclw_hal_spi_peer_close(struct mclw_spi_peer *peer) {
    uint32_t state = mclw_hal_critical_enter();

    peer->open = false;
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_peer_load(struct mclw_spi_peer *peer, const uint8_t *bytes, size_t len) {
    uint32_t state = mclw_hal_critical_enter();

    memset(peer->tx, 0, sizeof(peer->tx));
    memcpy(peer->tx, bytes, len < MCLW_SPI_PEER_RING ? len : MCLW_SPI_PEER_RING);
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_peer_received(const struct mclw_spi_peer *peer, size_t at, uint8_t *bytes, size_t len) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < len; i++) {
        bytes[i] = peer->rx[(at + i) % MCLW_SPI_PEER_RING];
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_peer_restart(struct mclw_spi_peer *peer) {
    uint32_t state = mclw_hal_critical_enter();

    peer->count = 0;
    mclw_hal_critical_exit(state);
}

// The emulator hands over whole frames, which its SPI block clocks in the
// mode the DUT set; the peer has no edges of its own to place, only its
// frame width and bit order.
int mclw_hal_spi_peer_set_format(struct mclw_spi_peer *peer, const struct mclw_spi_peer_format *format) {
    if (peer == NULL || !peer->open || format == NULL || format->mode > 3u ||
        (format->bits != 8u && format->bits != 16u)) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(peer)) {
        return -EBUSY;
    }
    peer->format = *format;
    if (!peer->controller) {
        mclw_hal_spi_peer_restart(peer);
    }
    return 0;
}

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t frames,
    size_t window) {
    uint64_t at = mclw_host_event_ns();
    uint8_t bits;

    if (!peer->controller || frames == 0u || frames > MCLW_SPI_PEER_FRAMES_MAX || window == 0u) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(peer)) {
        return -EBUSY;
    }
    bits = peer->format.bits;
    uint32_t state = mclw_hal_critical_enter();
    peer->in = in;
    peer->len = frames;
    peer->answered = 0;
    mclw_hal_critical_exit(state);
    for (size_t i = 0; i < frames; i++) {
        uint16_t v = bits == 16u ? (uint16_t)((out[2u * i] << 8) | out[2u * i + 1u]) : out[i];
        bool hold = (i + 1u) % window != 0u && i + 1u != frames;

        at += peer->frame_ns * (bits / 8u);
        if (peer->format.lsb_first) {
            v = reverse(v, bits);
        }
        if (!mclw_host_event_spi_clock(peer->sck, peer->mosi, peer->miso, v, bits, hold, at)) {
            state = mclw_hal_critical_enter();
            peer->len = 0;
            mclw_hal_critical_exit(state);
            return -ENODEV;
        }
    }
    return 0;
}

bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *peer) {
    return peer->controller && peer->answered < peer->len;
}

uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *peer) {
    return peer->count;
}
