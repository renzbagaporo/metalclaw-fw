<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# rp2

The DUT board is `rpi_pico2/rp2350a/m33`, a Raspberry Pi Pico 2 running an
in-tree Zephyr board definition with no MetalClaw changes: the smoke image
builds on the stock board with no overlay, because it binds nothing. A device
that needs DUT-side wiring brings its own overlay from
`devices/<name>/test/dut/rp2.overlay`, which the rig applies on top of the
board for the image that exercises that device. There are none yet.
