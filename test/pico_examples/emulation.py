# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program needs of the emulated bench beyond the bench
# scripts' defaults, as the Renode bench scripts' per-session variables, and
# the programs the emulator cannot judge with Metalclaw in the loop. The
# emulator platform sets the needs at bench start
# (test/rig/platforms/emulator/bench.py) and skips the programs it cannot
# judge; the hardware platform never reads this.

from dataclasses import dataclass, replace


@dataclass(frozen=True)
class Unwired:
    # DUT pins left with no wire to the harness: the bench script's wires on
    # them are disposed of before the emulation starts, so what the DUT
    # drives there reaches nothing. For a program judged inside the DUT
    # whose pins would otherwise flood the wires between the boards.
    pins: tuple[int, ...] = ()

    # What a result reached with them is labelled with.
    @property
    def label(self) -> str:
        return "unwired_" + "_".join(f"gp{p}" for p in self.pins) if self.pins else ""

    # Monitor commands after the bench script, given the script's wires on
    # each DUT pin.
    def monitor_commands(self, dut_wires: dict) -> tuple[str, ...]:
        wires = dict.fromkeys(w for p in self.pins for w in dut_wires.get(p, ()))
        return tuple(f"{w} Dispose" for w in wires)


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
    unwired: Unwired = Unwired()


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

# uart/gps_uart/gps_uart.c:20-23: uart1 at 9600 on GP4 (TX) and GP5 (RX),
# which the default byte-level I2C0 bus leaves unwired; a UART at 9600
# baud takes the default 300 us wires as they are.
GP4_GP5_WIRES = Emulation(i2c0_wiring="wires")

# pwm/hello_pwm/hello_pwm.c:16-29: slice 0 on GP0 and GP1 at clk_sys / 4,
# about 31 MHz, which the wires between the boards cannot carry: the
# harness never re-enumerates with its probe once they flood (3 of 3
# sessions, both benches). The program is judged by its registers over
# SWD, inside the DUT, so its pins reach nothing in the emulator.
HELLO_PWM_UNWIRED = Emulation(unwired=Unwired((0, 1)))

EMULATION: dict[str, Emulation] = {
    "pio_uart_rx": RELAYED_ON_GP4_GP5,
    "pio_uart_rx_intr": RELAYED_ON_GP4_GP5,
    "pio_uart_dma": RELAYED_ON_GP4_GP5,
    "pio_manchester_encoding": RELAYED,
    "pio_differential_manchester": RELAYED,
    "pwm_measure_duty_cycle": RELAYED_ON_GP4_GP5,
    "pio_onewire": RELAYED,
    "dht": RELAYED,
    "gps_uart": GP4_GP5_WIRES,
    "hello_pwm": HELLO_PWM_UNWIRED,
}

# Programs the emulator cannot judge with Metalclaw in the loop, and why;
# the emulator platform skips each with its reason. The wires between the
# emulated boards deliver an edge a fixed latency after its sender drove
# it, so a program whose timing needs the harness within less than a round
# trip of them fails there for want of the emulator, not of the harness.
# Each waits on a Renode change that puts the harness's pins in the DUT's
# time domain.
_WIRES = "the delayed wires between the boards (300 us; 3 us at best with RELAYED)"
_PENDING = "pending a Renode change that puts the harness's pins in the DUT's time domain"
EMULATOR_LIMITS: dict[str, str] = {
    # i2c/slave_mem_i2c: the DUT's I2C0 target and I2C1 controller, joined
    # by the harness's open-drain jumpers (parts.py _i2c_jumpers).
    **dict.fromkeys(("slave_mem_i2c", "slave_mem_i2c_burst"),
                    f"{_WIRES} cannot carry the harness's open-drain I2C join; {_PENDING}"),
    # pio/clocked_input/clocked_input.c:23-30, 37: SPI0 at clk_sys / 6, half
    # periods of about 24 ns, from GP2 and GP3 into the PIO's GP5 and GP4.
    "pio_clocked_input": f"{_WIRES} cannot carry the 21 MHz SPI jumpers; {_PENDING}",
    # pio/spi/spi_flash.c:106-128: the PIO SPI controller at 1 MHz in mode 0
    # (spi_cpha0).
    "pio_spi_flash": f"the PIO controller samples MISO on its own clock edges, so a harness flash answers a round "
                     f"trip of {_WIRES} late; {_PENDING}",
    # spi/spi_master_slave/spi_slave/spi_slave.c:72-88: the DUT answers each
    # byte on GP19 in the window the harness's spi_page_controller clocks.
    "spi_slave": f"the harness's controller samples the DUT's MISO a round trip of {_WIRES} late and reads ff; "
                 f"{_PENDING}",
}


# For a DUT image's example.json, or None for a rig image.
def emulation_for(metadata: dict | None) -> Emulation:
    return EMULATION.get(metadata["target"], DEFAULT) if metadata else DEFAULT
