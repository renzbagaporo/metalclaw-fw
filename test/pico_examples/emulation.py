# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program needs of the emulated bench beyond the bench
# scripts' defaults, as the Renode bench scripts' per-session variables, and,
# emulator-only, stand-ins inside the emulated DUT for a harness part that
# cannot keep up there. The emulator platform sets them at bench start
# (test/rig/platforms/emulator/bench.py); the hardware platform never reads
# this.

from dataclasses import dataclass, replace


@dataclass(frozen=True)
class StandIn:
    # A zero-latency wire between DUT pins `a` and `b` inside the emulated
    # DUT, used in place of the harness part named `replaces` (parts.py),
    # which is then not attached.
    a: int
    b: int
    replaces: str
    # The Renode type the stand-in needs in the package, if any beyond the
    # bench scripts' own.
    renode_type = None

    @property
    def wire(self) -> str:
        return f"jumper_gp{self.a}_gp{self.b}"

    # What a result reached with it is labelled with.
    @property
    def label(self) -> str:
        return self.wire

    @property
    def pins(self) -> tuple[int, ...]:
        return (self.a, self.b)

    # Monitor commands after the bench script, for the DUT's `dut_machine`.
    def monitor_commands(self, dut_machine: str) -> tuple[str, ...]:
        return (f'emulation CreateDigitalWire "{self.wire}" false', f'mach set "{dut_machine}"',
                f"{self.wire} Connect gpio {self.a}", f"{self.wire} Connect gpio {self.b}")


@dataclass(frozen=True)
class FlashStandIn:
    # Renode's RP2_PinSPIFlash in the emulated DUT, a W25Q-style flash that
    # follows the DUT's own CS, SCK and MOSI pads and drives its MISO pad
    # with no latency, used in place of the harness part named `replaces`.
    # A PIO SPI controller samples MISO a few cycles after its own clock
    # edge, where a harness flash answers one round trip of the wires later.
    # No harness part may take these pins: the harness then never drives
    # them, and the bench script's wires on them carry only the DUT's own
    # levels.
    cs: int
    sck: int
    mosi: int
    miso: int
    replaces: str
    name: str = "spiFlash"
    renode_type = "RP2_PinSPIFlash"

    @property
    def label(self) -> str:
        return f"pin_spi_flash_gp{self.cs}"

    @property
    def pins(self) -> tuple[int, ...]:
        return (self.cs, self.sck, self.mosi, self.miso)

    def monitor_commands(self, dut_machine: str) -> tuple[str, ...]:
        return (f'mach set "{dut_machine}"',
                f'machine LoadPlatformDescriptionFromString "{self.name}: SPI.{self.renode_type} @ none '
                f'{{ gpio: gpio; csPin: {self.cs}; sckPin: {self.sck}; mosiPin: {self.mosi}; '
                f'misoPin: {self.miso} }}"')


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
    stand_ins: tuple[StandIn | FlashStandIn, ...] = ()

    # What a result says when it was reached with stand-ins, never by the
    # harness alone: the suffix of its test name.
    @property
    def stand_in_label(self) -> str:
        return "stand-in-" + "+".join(s.label for s in self.stand_ins) if self.stand_ins else ""


DEFAULT = Emulation()

# A jumper the harness relays (gpio_link's follow) crosses the boards twice,
# out and back, so its edges reach the DUT two wire latencies late: the
# bench runs at a 1 us quantum with 3 us wires for it. A 1-Wire or DHT part
# answers its program within 10-40 us, which the same wires carry.
RELAYED = Emulation(quantum="0.000001", wire_latency_us=3)

# The same, for a program that takes DUT GP4 and GP5 as GPIO, PIO or UART
# pins, not I2C: the bench scripts' default byte-level I2C0 bus leaves
# those pads unwired, and only the pin-level I2C0 wires reach the
# carrier's two harness branches of each.
RELAYED_ON_GP4_GP5 = replace(RELAYED, i2c0_wiring="wires")

# slave_mem_i2c.c's I2C0 target and I2C1 controller on the DUT's own pins:
# the harness's open-drain join cannot keep up across the emulated wires, so
# pin-level I2C0 and two wires inside the DUT take its place.
I2C_STAND_INS = Emulation(i2c0_wiring="wires", stand_ins=(StandIn(4, 6, "sda"), StandIn(5, 7, "scl")))

# pio/clocked_input/clocked_input.c:23-30, 37: SPI0 clocks eight bytes at
# clk_sys / 6, about 21 MHz, from GP2 (clock) and GP3 (data) into the PIO's
# GP5 and GP4: half periods of about 24 ns, which no harness relay across
# the emulated wires carries, so two wires inside the DUT take the jumpers'
# place. GP4 and GP5 then reach no harness pin: the default I2C0 bus leaves
# their pads unwired.
CLOCKED_INPUT_STAND_INS = Emulation(stand_ins=(StandIn(2, 5, "clock"), StandIn(3, 4, "data")))

# pio/spi/spi_flash.c:106-128: the PIO SPI controller on spi_default's pins
# at 1 MHz samples MISO on its own clock edges (spi_cpha0), so a harness
# flash, whose answer comes one round trip of the wires later, can never
# answer it in time.
PIO_SPI_FLASH_STAND_IN = Emulation(stand_ins=(FlashStandIn(cs=17, sck=18, mosi=19, miso=16, replaces="flash"),))

# uart/gps_uart/gps_uart.c:20-23: uart1 at 9600 on GP4 (TX) and GP5 (RX),
# which the default byte-level I2C0 bus leaves unwired; a UART at 9600
# baud takes the default 300 us wires as they are.
GP4_GP5_WIRES = Emulation(i2c0_wiring="wires")

EMULATION: dict[str, Emulation] = {
    "pio_uart_rx": RELAYED_ON_GP4_GP5,
    "pio_uart_rx_intr": RELAYED_ON_GP4_GP5,
    "pio_uart_dma": RELAYED_ON_GP4_GP5,
    "pio_manchester_encoding": RELAYED,
    "pio_differential_manchester": RELAYED,
    "pwm_measure_duty_cycle": RELAYED_ON_GP4_GP5,
    "pio_onewire": RELAYED,
    "dht": RELAYED,
    "pio_clocked_input": CLOCKED_INPUT_STAND_INS,
    "pio_spi_flash": PIO_SPI_FLASH_STAND_IN,
    "gps_uart": GP4_GP5_WIRES,
    "slave_mem_i2c": I2C_STAND_INS,
    "slave_mem_i2c_burst": I2C_STAND_INS,
}


# For a DUT image's example.json, or None for a rig image.
def emulation_for(metadata: dict | None) -> Emulation:
    return EMULATION.get(metadata["target"], DEFAULT) if metadata else DEFAULT
