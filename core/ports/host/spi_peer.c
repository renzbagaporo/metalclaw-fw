// SPDX-License-Identifier: Apache-2.0 OR MIT
// SPI peers for the host port (metalclaw/hal/spi_peer.h). A target answers
// the DUT's controller a byte a frame: the emulator hands over each frame
// of the DUT's SPI block, which in mode 0 raises its chip select between
// every two frames, so each frame is one window. The controller side,
// which clocks a DUT that is the target, needs the emulator to clock the
// DUT's block from the harness and is not here yet.

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
};

static struct mclw_spi_peer peers[PEERS_MAX];

bool mclw_host_spi_peer_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint8_t *miso) {
    bool taken = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < PEERS_MAX && !taken; i++) {
        struct mclw_spi_peer *p = &peers[i];

        if (!p->open || p->sck != sck || p->mosi != mosi_pin || p->miso != miso_pin) {
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

struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    (void)sck;
    (void)mosi;
    (void)miso;
    (void)cs;
    (void)hz;
    *err = -ENOTSUP;
    return NULL;
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

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t len) {
    (void)peer;
    (void)out;
    (void)in;
    (void)len;
    return -ENOTSUP;
}

bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *peer) {
    (void)peer;
    return false;
}

uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *peer) {
    return peer->count;
}
