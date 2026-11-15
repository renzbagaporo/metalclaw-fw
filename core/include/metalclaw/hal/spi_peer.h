// SPDX-License-Identifier: Apache-2.0 OR MIT
// A byte-wide SPI peer on four pins a model holds as gpio bindings: SPI
// mode 0, MSB first, one byte per chip-select window. That is how an ARM
// PL022, the RP2040's and RP2350's SPI block, frames Motorola SPI with SPO
// 0 and SPH 0: it raises its chip select between every two frames. A
// register-map target, which reads the first byte of a window as a
// command, cannot answer that.
//
// target      the DUT clocks. Each window shifts out the next byte of a
//             MCLW_SPI_PEER_RING-byte ring the model loads, and the byte
//             the DUT sent lands at the same place in a receive ring. The
//             sck, mosi and cs pins are inputs, miso an output.
// controller  the harness clocks a transfer the model starts, at its own
//             rate, one window per byte. sck, mosi and cs are outputs,
//             miso an input.
//
// No CPU is in the byte path of either.

#ifndef MCLW_HAL_SPI_PEER_H
#define MCLW_HAL_SPI_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_SPI_PEER_RING 256u

struct mclw_gpio;
struct mclw_spi_peer;

struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err);
struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err);
void mclw_hal_spi_peer_close(struct mclw_spi_peer *peer);

// Target. What the next windows shift out, from ring position 0 (`len`
// bytes, the rest of the ring zero), and the received byte at a position.
// Restarting puts both rings back at position 0 and the count at zero; a
// window open at the time is lost.
void mclw_hal_spi_peer_load(struct mclw_spi_peer *peer, const uint8_t *bytes, size_t len);
void mclw_hal_spi_peer_received(const struct mclw_spi_peer *peer, size_t at, uint8_t *bytes, size_t len);
void mclw_hal_spi_peer_restart(struct mclw_spi_peer *peer);

// Controller. Clocks `len` bytes of `out`, storing what came back in `in`;
// both must stay valid until busy() is false. -EBUSY while one runs.
int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t len);
bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *peer);

// Bytes exchanged since the peer opened or restarted.
uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *peer);

#endif
