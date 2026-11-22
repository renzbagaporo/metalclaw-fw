// SPDX-License-Identifier: Apache-2.0 OR MIT
// An SPI peer on four pins a model holds as gpio bindings, MISO optional
// (a half-duplex link). Frames of 8 or 16 bits, MSB or LSB first, in any of
// the four SPI modes (8-bit frames, MSB first, mode 0 when it opens). A
// window is one chip-select assertion, of any number of frames: an ARM
// PL022, the RP2040's and RP2350's SPI block, raises its chip select
// between every two frames in Motorola SPI with SPH 0, and other blocks hold
// it low for a whole transfer. A register-map target, which reads the first
// byte of a window as a command, answers neither.
//
// target      the DUT clocks. Every frame shifts out the next frame of a
//             MCLW_SPI_PEER_RING-byte ring the model loads, and the frame
//             the DUT sent lands at the same place in a receive ring. The
//             sck, mosi and cs pins are inputs, miso an output. A window
//             ends on a frame boundary.
// controller  the harness clocks a transfer the model starts, at its own
//             rate, in windows of as many frames as the model asks. sck,
//             mosi and cs are outputs, miso an input.
//
// A 16-bit frame is two bytes in the rings and buffers, its high byte
// first. No CPU is in the frame path of either.

#ifndef MCLW_HAL_SPI_PEER_H
#define MCLW_HAL_SPI_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_SPI_PEER_RING 256u
// The most frames one controller transfer takes.
#define MCLW_SPI_PEER_FRAMES_MAX 256u

struct mclw_gpio;
struct mclw_spi_peer;

// mode: CPOL in bit 1 (SCK's idle level), CPHA in bit 0 (0: a bit out
// before SCK's first edge and sampled on it; 1: out on the first edge,
// sampled on the second). bits: 8 or 16.
struct mclw_spi_peer_format {
    uint8_t mode;
    uint8_t bits;
    bool lsb_first;
};

// `miso` may be NULL.
struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err);
struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err);
void mclw_hal_spi_peer_close(struct mclw_spi_peer *peer);

// A target restarts as restart() does; a controller takes it only between
// transfers. -EINVAL for a format outside the above, -EBUSY while a
// controller transfer runs.
int mclw_hal_spi_peer_set_format(struct mclw_spi_peer *peer, const struct mclw_spi_peer_format *format);

// Target. What the next frames shift out, from ring position 0 (`len`
// bytes, the rest of the ring zero), and the received bytes from a
// position. Restarting puts both rings back at position 0 and the count at
// zero, keeping what they hold; a frame open at the time is lost.
void mclw_hal_spi_peer_load(struct mclw_spi_peer *peer, const uint8_t *bytes, size_t len);
void mclw_hal_spi_peer_received(const struct mclw_spi_peer *peer, size_t at, uint8_t *bytes, size_t len);
void mclw_hal_spi_peer_restart(struct mclw_spi_peer *peer);

// Controller. Clocks `frames` frames of `out` in windows of `window` frames
// (the last one shorter if they do not divide), storing what came back in
// `in` as it comes; both must stay valid until busy() is false. -EBUSY
// while one runs, -EINVAL for no frames or more than
// MCLW_SPI_PEER_FRAMES_MAX.
int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t frames,
    size_t window);
bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *peer);

// Frames exchanged since the peer opened or restarted.
uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *peer);

#endif
