# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program needs of the emulated bench beyond the bench
# scripts' defaults, as the Renode bench scripts' per-session variables, and
# the programs the emulator cannot judge with Metalclaw in the loop. The
# emulator platform sets the needs at bench start
# (test/rig/platforms/emulator/bench.py) and skips the programs it cannot
# judge; the hardware platform never reads this.

from dataclasses import dataclass


@dataclass(frozen=True)
class Emulation:
    # I2C0 between the chips: "bus", byte level (the bench scripts'
    # default), or "wires", pin-level wires on GP4 and GP5 (with the
    # carrier's 5.1 k pull-ups).
    i2c0_wiring: str | None = None


DEFAULT = Emulation()

# A program that takes DUT GP4 and GP5 as GPIO, PIO, UART or SPI pins, not
# I2C: the bench scripts' default byte-level I2C0 bus leaves those pads
# unwired, and only the pin-level I2C0 wires reach the carrier's two harness
# branches of each.
GP4_GP5_WIRES = Emulation(i2c0_wiring="wires")

# I2C carried on its pins: Renode's RP2 I2C peripheral drives its SDA and
# SCL pads when no byte-level bus is connected, so a program whose I2C
# reaches the harness through a jumper or a PIO controller takes the
# pin-level I2C0 wires.
I2C_PIN_WIRES = Emulation(i2c0_wiring="wires")

EMULATION: dict[str, Emulation] = {
    # pio/uart_rx/uart_rx.c:26-27 and uart_rx_intr.c:33-34: the PIO
    # receives on GP3 from a UART TX on GP4, which the harness relays.
    "pio_uart_rx": GP4_GP5_WIRES,
    "pio_uart_rx_intr": GP4_GP5_WIRES,
    # pio/uart_dma/uart_dma.c:19-21: sends on GP4 and receives on GP5.
    "pio_uart_dma": GP4_GP5_WIRES,
    # pwm/measure_duty_cycle/measure_duty_cycle.c:16-17: a PWM output on GP2
    # measured by slice 2's B input on GP5.
    "pwm_measure_duty_cycle": GP4_GP5_WIRES,
    # pio/clocked_input/clocked_input.c:23-30: the PIO reads GP4 and GP5,
    # where the harness's relays return SPI0's GP2 and GP3.
    "pio_clocked_input": GP4_GP5_WIRES,
    # uart/gps_uart/gps_uart.c:20-23: uart1 at 9600 on GP4 (TX) and GP5
    # (RX).
    "gps_uart": GP4_GP5_WIRES,
    # spi/mpu9250_spi/mpu9250_spi.c:43-46: SPI0 with MISO on GP4 and CS on
    # GP5; without the wires the part never sees CS and the program reads 0.
    "mpu9250_spi": GP4_GP5_WIRES,
    # i2c/slave_mem_i2c: the DUT's I2C0 target and I2C1 controller, joined
    # by the harness's open-drain jumpers on GP4-7 (parts.py _i2c_jumpers);
    # pio/i2c/i2c_bus_scan.c:6-7, 35: a PIO I2C controller on GP2 and GP3
    # against a harness I2C target there.
    "slave_mem_i2c": I2C_PIN_WIRES,
    "slave_mem_i2c_burst": I2C_PIN_WIRES,
    "pio_i2c_bus_scan": I2C_PIN_WIRES,
}

# The programs the emulator cannot judge with Metalclaw in the loop, and
# why: the emulator platform skips each with its reason before the bench
# starts (EMULATOR_LIMITS).

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

# uart/lcd_uart/lcd_uart.c:29-31, 101: lcd_init writes 0xFE a few
# instructions after gpio_set_function hands GP8 to UART1. Until then the
# pin is undriven and reads low on the fixture (the harness's pull-up
# against the RP2350 pad's pull-down; measured, 2026-11-18), so the part's
# receiver takes a frame error and waits for an idle-high line before the
# next start bit, as a real UART's 16x idle detection does. On silicon the
# code after gpio_set_function runs cold from XIP flash, and its cache
# misses hold the line high long enough. The emulator models no XIP fetch
# latency, so there the line is high for about 1 us, under one 13 us sample
# at 9600 baud, and the part takes 0x42 alone. Re-judge it if the emulator
# gains XIP miss timing.
RACE_LIMITS: dict[str, str] = {
    "lcd_uart": "the emulator models no XIP flash fetch latency, so lcd_init's first byte follows UART1 taking the "
                "undriven-low GP8 by about 1 us, under a receiver's idle detection; silicon's cache misses give it "
                "margin",
}

EMULATOR_LIMITS: dict[str, str] = {**UNPROVISIONED, **SPEED_LIMITS, **CORE_ORDER_LIMITS, **RACE_LIMITS}


# For a DUT image's example.json, or None for a rig image.
def emulation_for(metadata: dict | None) -> Emulation:
    return EMULATION.get(metadata["target"], DEFAULT) if metadata else DEFAULT
