# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program needs of the emulated bench beyond the bench
# scripts' defaults, as the Renode bench scripts' per-session variables, and
# the programs the emulator cannot judge with Metalclaw in the loop. The
# emulator platform sets the needs at bench start
# (test/rig/platforms/emulator/bench.py) and skips the programs it cannot
# judge; the hardware platform never reads this.

from dataclasses import dataclass, replace


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

# spi/mpu9250_spi/mpu9250_spi.c:43-46: SPI0 with MISO on GP4 and CS on GP5,
# which the default byte-level I2C0 bus leaves unwired, so the part never
# sees CS and the program reads 0; the pin-level I2C0 wires carry them.
MPU9250_SPI_WIRES = GP4_GP5_WIRES

# i2c/slave_mem_i2c/slave_mem_i2c.c and pio/i2c/i2c_bus_scan.c: I2C carried
# on its pins, the DUT's I2C0 target and I2C1 controller joined by the
# harness's open-drain jumpers on GP4-7, and a PIO controller on GP2 and GP3
# against a harness I2C target there. Renode's RP2 I2C peripheral carries
# its bus on its SDA and SCL pads when no byte-level bus is connected, so
# these take the pin-level I2C0 wires; two machines' delayed wires still
# cannot carry the open-drain join (WIRE_LIMITS).
I2C_PIN_WIRES = Emulation(i2c0_wiring="wires")

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
    "mpu9250_spi": MPU9250_SPI_WIRES,
    "slave_mem_i2c": I2C_PIN_WIRES,
    "slave_mem_i2c_burst": I2C_PIN_WIRES,
    "pio_i2c_bus_scan": I2C_PIN_WIRES,
    # pio/clocked_input/clocked_input.c:23-30: the PIO reads GP4 and GP5,
    # where the harness's relays return SPI0's GP2 and GP3.
    "pio_clocked_input": RELAYED_ON_GP4_GP5,
}

# Programs the emulator cannot judge with Metalclaw in the loop, and why;
# the emulator platform skips each with its reason (EMULATOR_LIMITS). The
# wires between the emulated boards deliver an edge a fixed latency after
# its sender drove it, so a program whose timing needs the harness within
# less than a round trip of them fails there for want of the emulator, not
# of the harness. Each waits on a Renode change that puts the harness's
# pins in the DUT's time domain.
_WIRES = "the delayed wires between the boards (300 us; 3 us at best with RELAYED)"
_PENDING = "pending a Renode change that puts the harness's pins in the DUT's time domain"
WIRE_LIMITS: dict[str, str] = {
    # i2c/slave_mem_i2c: the DUT's I2C0 target and I2C1 controller, joined
    # by the harness's open-drain jumpers (parts.py _i2c_jumpers).
    **dict.fromkeys(("slave_mem_i2c", "slave_mem_i2c_burst"),
                    f"{_WIRES} cannot carry the harness's open-drain I2C join; {_PENDING}"),
    # pio/clocked_input/clocked_input.c:23-30, 37: SPI0 at clk_sys / 6, half
    # periods of about 24 ns, from GP2 and GP3 into the PIO's GP5 and GP4.
    # pio/i2c/i2c_bus_scan.c:6-7, 35: a PIO I2C controller on GP2 and GP3.
    "pio_i2c_bus_scan": f"{_WIRES} cannot carry the PIO controller's open-drain I2C to the harness's target; "
                        f"{_PENDING}",
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

# Programs that cannot complete without OTP keys, and a second image, that
# the emulated bench does not provision: its DUT's OTP starts blank, and a
# session flashes the one image it judges. Each decrypts with the AES key
# and IV salt `picotool otp load otp.json` writes, from the OTP pages the
# build names (encrypted/hello_encrypted/README.md,
# bootloaders/encrypted/README.md).
_NO_KEY = "the emulated DUT's OTP is blank, so the AES key and IV salt `picotool otp load otp.json` writes are missing"
UNPROVISIONED: dict[str, str] = {
    # encrypted/hello_encrypted/CMakeLists.txt:72-90, 118-138: no_flash
    # images that pico_encrypt_binary wraps in a stage that decrypts them
    # with the key from OTP before hello_encrypted.c:44-51 runs.
    "hello_encrypted": f"decrypts itself before main with a key from OTP, and {_NO_KEY}",
    "hello_encrypted_mbedtls": f"decrypts itself before main with a key from OTP page 29, and {_NO_KEY}",
    # bootloaders/encrypted/enc_bootloader.c:157-247: finds the encrypted
    # image in its partition (enc-pt.json), decrypts it with the key from
    # OTP page 29 and chains into it.
    "enc_bootloader": f"decrypts and chains into hello_serial_enc, which a session that flashes enc_bootloader does "
                      f"not hold, and {_NO_KEY}",
    # bootloaders/encrypted/CMakeLists.txt:66-89: encrypted for
    # enc_bootloader to decrypt into SRAM (README.md: load the bootloader
    # first, then this image).
    "hello_serial_enc": f"runs only as decrypted by enc_bootloader, which a session that flashes hello_serial_enc "
                        f"does not hold, and {_NO_KEY}",
}

# Programs whose emulation runs so far behind real time that the harness
# does not finish setting up within the rig's deadlines. Each program runs
# correctly; the emulator pays for every event it makes.
SPEED_LIMITS: dict[str, str] = {
    # hstx/spi_lcd/hstx_spi_lcd.c: one HSTX FIFO word per pixel byte.
    "hstx_spi_lcd": "emulator speed: about 100 times slower than real time, from about six million HSTX register "
                    "accesses per virtual second",
    # pwm/hello_pwm/hello_pwm.c:16-29: slice 0 at clk_sys / 4 with TOP 3 on
    # GP0 and GP1, a compare match or wrap every few cycles, each a clock
    # event and a pad change whether or not anything is wired to the pad.
    "hello_pwm": "emulator speed: about 1,600 times slower than real time, from tens of millions of PWM compare and "
                 "wrap events per virtual second on GP0 and GP1",
    # pio/hub75/hub75.c:16-22: the PIO clocks the panel's data and control on 14
    # pins from boot.
    "pio_hub75": "emulator speed: about 1,100 times slower than real time, from the PIO's pin changes on the panel's "
                 "14 pins",
}

# multicore/multicore_doorbell/multicore_doorbell.c:16, 33-42: core 1 enables the
# doorbell interrupt, then waits on exit_core0, which is not volatile, so GCC
# 14 loads it once before a `b .`. On silicon core 0 rings every few cycles,
# and the interrupts tail-chain until the exit doorbell's handler has set the
# flag, so that one load reads 1. The emulated cores run on separate host
# threads, so core 1 returns between rings, reads 0 and spins for good.
CORE_ORDER_LIMITS: dict[str, str] = {
    "multicore_doorbell": "core 1 reads the non-volatile exit_core0 once before spinning, and only silicon's "
                          "back-to-back doorbell interrupts hold it off until the flag is set; the emulated cores "
                          "do not interleave that tightly",
}

EMULATOR_LIMITS: dict[str, str] = {**WIRE_LIMITS, **UNPROVISIONED, **SPEED_LIMITS, **CORE_ORDER_LIMITS}
# What still applies with both chips in one machine, where no delayed wire
# lies between the harness and the DUT.
ONE_MACHINE_LIMITS: dict[str, str] = {**UNPROVISIONED, **SPEED_LIMITS, **CORE_ORDER_LIMITS}


# For a DUT image's example.json, or None for a rig image.
def emulation_for(metadata: dict | None) -> Emulation:
    return EMULATION.get(metadata["target"], DEFAULT) if metadata else DEFAULT
