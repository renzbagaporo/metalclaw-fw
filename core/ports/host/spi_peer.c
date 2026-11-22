// SPDX-License-Identifier: Apache-2.0 OR MIT
// SPI peers for the host port (metalclaw/hal/spi_peer.h). A target answers
// the DUT's controller a byte a frame: the emulator hands over each frame
// of the DUT's SPI block, which in mode 0 raises its chip select between
// every two frames, so each frame is one window. A controller clocks a DUT
// whose block is the target a frame per window at its own rate: each
// frame an spi_clock action, each answer an spi_clocked request.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_peer.h"

#include "host.h"

#define PEERS_MAX 2u

struct mclw_spi_peer {
    bool open;
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

bool mclw_host_spi_peer_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint8_t *miso) {
    bool taken = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < PEERS_MAX && !taken; i++) {
        struct mclw_spi_peer *p = &peers[i];

        if (!p->open || p->controller || p->sck != sck || p->mosi != mosi_pin || p->miso != miso_pin) {
            continue;
        }
        uint32_t at = p->count % MCLW_SPI_PEER_RING;

        *miso = p->tx[at];
        p->rx[at] = mosi;
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
        p->miso = mclw_hal_gpio_pin(miso);
        p->open = true;
    }
    mclw_hal_critical_exit(state);
    if (p == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    // The pins are the DUT's SPI block's, which the emulator serves by
    // frame: the gpio bindings that hold them drive nothing.
    mclw_hal_gpio_release(miso);
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
            p->in[p->answered] = miso >= 0 ? (uint8_t)miso : 0xff;
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

    memset(peer->rx, 0, sizeof(peer->rx));
    peer->count = 0;
    mclw_hal_critical_exit(state);
}

// The emulator hands over whole frames, which its SPI block clocks in the
// mode the DUT set; the peer has no edges of its own to place.
int mclw_hal_spi_peer_set_mode(struct mclw_spi_peer *peer, uint8_t mode) {
    if (peer == NULL || !peer->open || mode > 3u) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(peer)) {
        return -EBUSY;
    }
    if (!peer->controller) {
        mclw_hal_spi_peer_restart(peer);
    }
    return 0;
}

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t len) {
    uint64_t at = mclw_host_event_ns();

    if (!peer->controller) {
        return -EINVAL;
    }
    if (mclw_hal_spi_peer_busy(peer)) {
        return -EBUSY;
    }
    uint32_t state = mclw_hal_critical_enter();
    peer->in = in;
    peer->len = len;
    peer->answered = 0;
    mclw_hal_critical_exit(state);
    for (size_t i = 0; i < len; i++) {
        at += peer->frame_ns;
        if (!mclw_host_event_spi_clock(peer->sck, peer->mosi, peer->miso, out[i], at)) {
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
