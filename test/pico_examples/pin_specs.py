# SPDX-License-Identifier: Apache-2.0 OR MIT
# Pin behaviour that shows a pico-examples program completed, for programs
# judged by their pins rather than (or as well as) their console: the pins
# each one drives, watched over SWD (pins.py). Criteria come from each
# program's source; times are the program's own delays with a margin for
# one poll, and counts the changes those delays make in the watch, less one
# at either end of it (toggles).

from dataclasses import dataclass

from test.pico_examples.pins import Levels, Morse, Segments, Toggle


@dataclass(frozen=True)
class Press:
    # The board's BOOTSEL button held from `at` to `release`, seconds into
    # the watch.
    at: float
    release: float


@dataclass(frozen=True)
class PinSpec:
    seconds: float
    # The checks: Toggle, Morse, Segments, Levels (pins.py).
    toggles: tuple
    source: str = ""
    # What the case does while it watches: (seconds, bytes) typed on the
    # console, and presses of the BOOTSEL button.
    send: tuple[tuple[float, bytes], ...] = ()
    presses: tuple[Press, ...] = ()


@dataclass(frozen=True)
class BootselSpec:
    # Resets on RUN: held low `low_s`, `gap_s` between them. After one, the
    # program blinks `led` at least `edges` times in `seconds`; after two
    # the bootrom presents USB and the LED stops. Each step's first reset
    # comes `settle_s` of bench time after the last start, past the window
    # in which it would count as the second.
    led: int
    low_s: float
    gap_s: float
    seconds: float
    edges: int
    source: str = ""
    settle_s: float = 0.3


def half(seconds: float) -> tuple[float, float]:
    # A half period with room for one poll either side and timer jitter.
    return (seconds * 0.92 - 0.004, seconds * 1.08 + 0.004)


def toggles(seconds: float, half: float, first: float = 0.0) -> int:
    # The changes a pin that first changes `first` seconds into the program
    # and every `half` after makes in a watch of `seconds`, less one for the
    # watch's start, which may come just after a change or before the
    # program's first, and one for its end, which the program's start
    # moves.
    return round((seconds - first) / half) - 2


# PICO_DEFAULT_LED_PIN is GP25 on both pico and pico2 (pico-sdk
# src/boards/include/boards/pico.h, pico2.h).
LED = 25

PIN_SPECS: dict[str, PinSpec] = {
    # blink/blink.c:46-51: LED on, sleep_ms(LED_DELAY_MS), off, sleep, forever;
    # LED_DELAY_MS 250 (blink.c:16).
    "blink": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25), (0.24, 0.27)),), "blink/blink.c"),
    # blink_simple/blink_simple.c:36-41: the same loop, LED_DELAY_MS 250 (:10).
    "blink_simple": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25), (0.24, 0.27)),), "blink_simple/blink_simple.c"),
    # binary_info/blink_any/blink_any.c:32-34, 70-75: LED_PIN (bi_ptr, 25 by
    # default) toggles every 250 ms.
    "blink_any": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25), half(0.25)),), "binary_info/blink_any/blink_any.c"),
    # async_context/simple_at_time_worker/simple_at_time_worker.c:22-33, 66: an at-time worker
    # toggles the LED every 500 ms, turning it on first, 500 ms in
    # (led_state true, :35-37).
    "simple_at_time_worker": PinSpec(4.0, (Toggle(LED, toggles(4.0, 0.5, 0.5), half(0.5)),),
                                     "async_context/simple_at_time_worker/simple_at_time_worker.c"),
    # status_led/status_blink/status_blink.c:10-24: 250 ms half periods.
    "status_blink": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25), half(0.25)),),
                            "status_led/status_blink/status_blink.c"),
    # system/hello_double_tap/hello_double_tap.c:20-25: 250 ms half periods,
    # from the end of the double-reset check's 200 ms (BOOTSEL_SPECS).
    "hello_double_tap": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25, 0.2), half(0.25)),),
                                "system/hello_double_tap/hello_double_tap.c"),
    # usb/device/dev_hid_composite/main.c:45, 298-312: TinyUSB's board LED
    # blinks every 250 ms while unmounted, its first write at 250 ms the
    # off it already shows; the DUT's USB reaches no host on either bench,
    # and the emulator imports it only for the USB cases, which come after.
    "dev_hid_composite": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.25, 0.5), half(0.25)),),
                                 "usb/device/dev_hid_composite/main.c"),
    # bootloaders/uart/uart_binary.c:12-14, 79-85 (pico2, no_flash): 500 ms half periods.
    "uart_binary": PinSpec(4.0, (Toggle(LED, toggles(4.0, 0.5), half(0.5)),), "bootloaders/uart/uart_binary.c"),
    # low_power/low_power_dormant/low_power_clksrc.c:17-21, 52: a 500 ms
    # repeating timer toggles the LED, the first time 500 ms in.
    "low_power_clksrc": PinSpec(4.0, (Toggle(LED, toggles(4.0, 0.5, 0.5), half(0.5)),),
                                "low_power/low_power_dormant/low_power_clksrc.c"),
    # pio/pio_blink/blink.c:30-57, 118-142: PIO square waves for 60 s, GP3 at
    # 4 Hz, GP4 3 Hz, GP5 2 Hz, GP6 1 Hz. The watcher reads the DUT's own
    # pins, so GP3 counts on fixture 1 too. blink_pin_forever (:156-165)
    # claims each pin as a PIO output driven low, prints, then pushes the
    # count: on the carrier's pulled-up lines the claim is a change a printf
    # before the first toggle, so each pin is judged from its second change.
    "pio_blink": PinSpec(4.0, tuple(Toggle(pin, toggles(4.0, h), half(h), judged_from=1) for pin, h in
                                    ((3, 1 / 8), (4, 1 / 6), (5, 1 / 4), (6, 1 / 2))), "pio/pio_blink/blink.c"),
    # picoboard/blinky/blinky.c:11, 42-77: "Hello world" in Morse on the
    # LED at DOT_PERIOD_MS 100, then 1 s off; the whole message takes about
    # 12.4 s, so two pauses and the message between them fit in 27 s.
    "picoboard_blinky": PinSpec(27.0, (Morse(LED, "HELLO WORLD", 0.1),), "picoboard/blinky/blinky.c"),
    # gpio/hello_7segment/hello_7segment.c:28-29, 46-90: segments A..G on
    # GP2..GP8, lit low (OUTOVER inverted), the next digit every 250 ms, up
    # while GP9 is low, down while high; parts.py presses GP9 2 s of every
    # 4. The watcher reads the DUT's pads, so the segments on GP2 and GP3
    # count on fixture 1 too. 9 s is 36 changes, less the program's first
    # digit and the watch's end: 34. Each level holds at least 4 of the 9
    # s, 16 changes, less the two its stretches' ends may cut: 14 each way.
    "hello_7segment": PinSpec(9.0, (Segments(2, 9, toggles(9.0, 0.25), 14),), "gpio/hello_7segment/hello_7segment.c"),
    # pio/hello_pio/hello.c:48-55: PIO blinks the LED at 500 ms half periods
    # while no key has come in, checked once a second; a key at 3 s ends it
    # by 4 s at the latest, and nothing changes the LED after.
    "hello_pio": PinSpec(7.0, (Toggle(LED, 6, half(0.5), until=4.2),), "pio/hello_pio/hello.c",
                         send=((3.0, b"q"),)),
    # picoboard/button/button.c:26-58, 66-74: every 10 ms the LED shows the
    # BOOTSEL button, read from QSPI_SS while it floats. No harness pin
    # reaches the Pico's BOOTSEL (on QSPI_SS, behind 1 kOhm) on either
    # bench, so the rig presses it on QSPI_SS's input path (pins.bootsel).
    "picoboard_button": PinSpec(
        7.0, (Levels(LED, ((0.0, 1.0, 0), (1.0, 2.5, 1), (2.5, 4.0, 0), (4.0, 5.5, 1), (5.5, 7.0, 0))),),
        "picoboard/button/button.c", presses=(Press(1.0, 2.5), Press(4.0, 5.5))),
}

BOOTSEL_SPECS: dict[str, BootselSpec] = {
    # system/hello_double_tap: pico_bootsel_via_double_reset
    # (pico-sdk src/rp2_common/pico_bootsel_via_double_reset) arms a flag
    # that survives RUN (RAM on RP2040, POWMAN CHIP_RESET.DOUBLE_TAP on
    # RP2350), waits PICO_BOOTSEL_VIA_DOUBLE_RESET_TIMEOUT_MS 200, and
    # clears it; a reset that finds it set calls rom_reset_usb_boot_extra
    # with ACTIVITY_LED 25 (CMakeLists.txt). The second reset comes 100 ms
    # after the first is let go: past the boot to that check, inside its
    # window. One reset is followed by the 250 ms blink (hello_double_tap.c:20-25).
    # Each step waits 300 ms first: a reset within 200 ms of the start
    # before it is that start's second.
    "hello_double_tap": BootselSpec(LED, 0.01, 0.1, 2.0, toggles(2.0, 0.25, 0.2),
                                    "system/hello_double_tap/hello_double_tap.c"),
}


# DUT words that show what a program set up or left, read over SWD: the
# registers behind pin edges too fast for a watch, or OTP rows a program
# wrote. Each word read once the program has run `after` bench seconds from
# a restart, masked, must be `expected` or one of the values in it.
# Addresses and fields are the pico-sdk 2.3.0 hardware_regs headers'
# (src/<chip>/hardware_regs).
@dataclass(frozen=True)
class Register:
    address: int
    mask: int
    expected: int | tuple[int, ...]
    what: str

    def check(self, value: int) -> str | None:
        allowed = self.expected if isinstance(self.expected, tuple) else (self.expected,)
        got = value & self.mask
        if got in allowed:
            return None
        return (f"{self.what}: 0x{self.address:08x} & 0x{self.mask:x} is 0x{got:x}, expected "
                + " or ".join(f"0x{v:x}" for v in allowed))


@dataclass(frozen=True)
class RegisterSpec:
    # By the DUT's chip (example.json "chip"), for each chip the program
    # builds for.
    registers: dict
    after: float = 0.05
    source: str = ""


# By chip: IO_BANK0 (GPIOn_CTRL at 0x04 + 8n, FUNCSEL bits 4:0), PWM
# (CH0_CSR 0x00, CH0_CC 0x0c, CH0_TOP 0x10), PIO0 (CTRL 0x000, SM0_CLKDIV
# 0x0c8, SM0_INSTR 0x0d8, SM0_PINCTRL 0x0dc) and CLOCKS (CLK_GPOUTn_CTRL at
# 0x0c * n, DIV after it), at the bases addressmap.h gives.
BASES = {
    "rp2040": {"io_bank0": 0x40014000, "pwm": 0x40050000, "pio0": 0x50200000, "clocks": 0x40008000},
    "rp2350": {"io_bank0": 0x40028000, "pwm": 0x400A8000, "pio0": 0x50200000, "clocks": 0x40010000},
}
FUNCSEL = 0x1F
FUNCSEL_PWM, FUNCSEL_PIO0 = 4, 6
# IO_BANK0_GPIOn_CTRL_FUNCSEL_VALUE_CLOCKS_GPOUT_n.
FUNCSEL_GPCK = {"rp2040": 8, "rp2350": 9}


def _funcsel(chip: str, gpio: int, value: int, name: str) -> Register:
    return Register(BASES[chip]["io_bank0"] + 0x04 + 8 * gpio, FUNCSEL, value, f"GP{gpio} FUNCSEL {name}")


def _hello_pwm(chip: str) -> tuple[Register, ...]:
    # pwm/hello_pwm/hello_pwm.c:16-29: GP0 and GP1 to PWM (slice 0), TOP 3,
    # channel A level 1 and B level 3, the slice enabled.
    pwm = BASES[chip]["pwm"]
    return (_funcsel(chip, 0, FUNCSEL_PWM, "PWM"), _funcsel(chip, 1, FUNCSEL_PWM, "PWM"),
            Register(pwm + 0x00, 0x1, 0x1, "PWM CH0_CSR.EN"),
            Register(pwm + 0x10, 0xFFFF, 3, "PWM CH0_TOP"),
            Register(pwm + 0x0C, 0xFFFFFFFF, (3 << 16) | 1, "PWM CH0_CC B 3, A 1"))


def _pio_squarewave(chip: str) -> tuple[Register, ...]:
    # pio/squarewave/squarewave.c:37-67: squarewave.pio into PIO0's
    # instruction memory from 0, SM0 at clk_sys / 2.5 (CLKDIV INT 2, FRAC
    # 0x80), SET pins GP0 only (PINCTRL SET_COUNT 1, the whole register
    # written), GP0 to PIO0, SM0 enabled. Instruction memory cannot be read;
    # SM0_INSTR is the instruction at its PC, which past the first is one of
    # the loop's: set pins, 1 [1]; set pins, 0; jmp 1 (generated/squarewave.pio.h).
    pio = BASES[chip]["pio0"]
    return (Register(pio + 0x000, 0x1, 0x1, "PIO0 CTRL.SM_ENABLE SM0"),
            Register(pio + 0x0C8, 0xFFFFFF00, 0x00028000, "PIO0 SM0_CLKDIV 2.5"),
            Register(pio + 0x0DC, 0xFFFFFFFF, 1 << 26, "PIO0 SM0_PINCTRL SET_COUNT 1, SET_BASE 0"),
            Register(pio + 0x0D8, 0xFFFF, (0xE101, 0xE000, 0x0001), "PIO0 SM0_INSTR in squarewave's loop"),
            _funcsel(chip, 0, FUNCSEL_PIO0, "PIO0"))


# clocks/hello_gpout/hello_gpout.c:17-24: clock_gpio_init (pico-sdk
# hardware_clocks/clocks.c:269-286) writes CLK_GPOUTn_CTRL as AUXSRC << 5
# with ENABLE (bit 11), DIV as 10 << DIV_INT_LSB (8 on RP2040, 16 on RP2350),
# and the pin's FUNCSEL to its GPOUT. AUXSRC: clk_sys, clk_usb, clk_adc,
# and clk_rtc on RP2040 or clk_peri on RP2350, per chip's clocks.h.
GPOUT_SOURCES = {"rp2040": (0x6, 0x7, 0x8, 0x9), "rp2350": (0x8, 0x9, 0xA, 0xC)}
GPOUT_PINS = (21, 23, 24, 25)
DIV_INT_LSB = {"rp2040": 8, "rp2350": 16}


def _hello_gpout(chip: str) -> tuple[Register, ...]:
    clocks, registers = BASES[chip]["clocks"], []
    for n, (pin, source) in enumerate(zip(GPOUT_PINS, GPOUT_SOURCES[chip])):
        registers += [Register(clocks + 0x0C * n, 0x9E0, 0x800 | source << 5, f"CLK_GPOUT{n}_CTRL ENABLE, AUXSRC"),
                      Register(clocks + 0x0C * n + 0x04, 0xFFFFFFFF, 10 << DIV_INT_LSB[chip], f"CLK_GPOUT{n}_DIV 10"),
                      _funcsel(chip, pin, FUNCSEL_GPCK[chip], f"GPOUT{n}")]
    return tuple(registers)


# RP2350 datasheet 13.6.2: the modified Hamming code an ECC row carries in
# bits 21:16, s_otp_calculate_ecc() as the datasheet gives it.
_OTP_ECC_PARITY = (0b0000001010110101011011, 0b0000000011011001101101, 0b0000001100011110001110,
                   0b0000000000011111110000, 0b0000001111100000000000, 0b0111111111111111111111)


def otp_ecc_row(data: int) -> int:
    row = data
    for i, mask in enumerate(_OTP_ECC_PARITY):
        row |= (bin(row & mask).count("1") & 1) << (16 + i)
    return row


def _raw_rows(data: bytes) -> list[int]:
    # A raw OTP access moves four bytes a row, the top one not stored.
    return [int.from_bytes(data[i:i + 3], "little") for i in range(0, len(data), 4)]


# otp/hello_otp/hello_otp.c on rows it found empty (:35-40): "Hello from
# OTP" as ECC rows from 0x400 (:44-46), then raw rewrites of those rows that
# only set bits, 'H' to 'x' in row 0x400 and 'T' to 't' in row 0x406
# (:64-73); the ECC rewrite as "hello from otp" (:92-94) would clear bits of
# row 0x400, the first it writes, so it changes nothing. Raw rows from 0x410
# take "Hel\0lo \0fro\0m O\0TP" (:110-112), then its odd characters as 'o'
# and even ones as '~', which only sets bits (:135-144). Rows read through
# OTP_DATA_RAW (OTP_DATA_RAW_BASE + 4 x row), 24 bits with the top 8 zero
# (datasheet 13.1).
OTP_DATA_RAW = 0x40134000
OTP_ECC_ROW, OTP_RAW_ROW = 0x400, 0x410


def _hello_otp_rows() -> dict[int, int]:
    text = b"Hello from OTP\0\0"
    ecc = [otp_ecc_row(int.from_bytes(text[i:i + 2], "little")) for i in range(0, len(text), 2)]
    ecc[0] |= ord("x") & ~ord("H")
    ecc[6] |= ord("t") & ~ord("T")
    written = b"Hel\0lo \0fro\0m O\0TP\0"
    over = bytes((ord("o") if c % 2 else ord("~")) if c else 0 for c in written)
    raw = [a | b for a, b in zip(_raw_rows(written), _raw_rows(over))]
    rows = {OTP_ECC_ROW + i: r for i, r in enumerate(ecc)}
    rows.update({OTP_RAW_ROW + i: r for i, r in enumerate(raw)})
    return rows


def _hello_otp(chip: str) -> tuple[Register, ...]:
    return tuple(Register(OTP_DATA_RAW + 4 * row, 0xFFFFFFFF, value, f"OTP row 0x{row:03x}")
                 for row, value in _hello_otp_rows().items())


def _per_chip(build) -> dict:
    return {chip: build(chip) for chip in BASES}


REGISTER_SPECS: dict[str, RegisterSpec] = {
    "hello_pwm": RegisterSpec(_per_chip(_hello_pwm), source="pwm/hello_pwm/hello_pwm.c"),
    "pio_squarewave": RegisterSpec(_per_chip(_pio_squarewave), source="pio/squarewave/squarewave.c"),
    "hello_gpout": RegisterSpec(_per_chip(_hello_gpout), source="clocks/hello_gpout/hello_gpout.c"),
    # Built for RP2350 only (otp/CMakeLists.txt). Every run a case sees
    # finds the rows the bench's boot run wrote (targets.py), which leaves
    # page 0x10 unlocked: the lock (:166-170) comes after the writes.
    "hello_otp": RegisterSpec({"rp2350": _hello_otp("rp2350")}, source="otp/hello_otp/hello_otp.c"),
}
