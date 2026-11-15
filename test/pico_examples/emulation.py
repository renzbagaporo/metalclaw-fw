# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program needs of the emulated bench beyond the bench
# scripts' defaults, as the Renode bench scripts' per-session variables, and,
# emulator-only, wires inside the emulated DUT that stand in for a harness
# relay that cannot keep up there. The emulator platform sets them at bench
# start (test/rig/platforms/emulator/bench.py); the hardware platform never
# reads this.

from dataclasses import dataclass


@dataclass(frozen=True)
class StandIn:
    # A zero-latency wire between DUT pins `a` and `b` inside the emulated
    # DUT, used in place of the harness part named `replaces` (parts.py),
    # which is then not attached.
    a: int
    b: int
    replaces: str

    @property
    def wire(self) -> str:
        return f"jumper_gp{self.a}_gp{self.b}"


@dataclass(frozen=True)
class Emulation:
    # The bench's synchronisation quantum in seconds, as Renode takes it.
    quantum: str | None = None
    # The latency of every wire between the boards, in whole us; it must
    # exceed two quanta.
    wire_latency_us: int | None = None
    # I2C0 between the boards: "bus", byte level (the scripts' default), or
    # "wires", pin-level delayed wires (with the carrier's 5.1 k pull-ups).
    i2c0_wiring: str | None = None
    stand_ins: tuple[StandIn, ...] = ()

    # What a result says when it was reached with stand-ins, never by the
    # harness alone: the suffix of its test name.
    @property
    def stand_in_label(self) -> str:
        return "stand-in-" + "+".join(s.wire for s in self.stand_ins) if self.stand_ins else ""


DEFAULT = Emulation()

# A jumper the harness relays (gpio_link's follow) crosses the boards twice,
# out and back, so its edges reach the DUT two wire latencies late: the
# bench runs at a 1 us quantum with 3 us wires for it.
RELAYED = Emulation(quantum="0.000001", wire_latency_us=3)

# slave_mem_i2c.c's I2C0 target and I2C1 controller on the DUT's own pins:
# the harness's open-drain join cannot keep up across the emulated wires, so
# pin-level I2C0 and two wires inside the DUT take its place.
I2C_STAND_INS = Emulation(i2c0_wiring="wires", stand_ins=(StandIn(4, 6, "sda"), StandIn(5, 7, "scl")))

EMULATION: dict[str, Emulation] = {
    "pio_uart_rx": RELAYED,
    "pio_uart_rx_intr": RELAYED,
    "pio_uart_dma": RELAYED,
    "pio_manchester_encoding": RELAYED,
    "pio_differential_manchester": RELAYED,
    "pwm_measure_duty_cycle": RELAYED,
    "pio_clocked_input": RELAYED,
    "slave_mem_i2c": I2C_STAND_INS,
    "slave_mem_i2c_burst": I2C_STAND_INS,
}


# For a DUT image's example.json, or None for a rig image.
def emulation_for(metadata: dict | None) -> Emulation:
    return EMULATION.get(metadata["target"], DEFAULT) if metadata else DEFAULT
