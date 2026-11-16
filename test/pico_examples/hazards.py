# SPDX-License-Identifier: Apache-2.0 OR MIT
# pico-examples programs that harm a real bench, and why. The hardware
# platform never runs them; the emulator runs them all.

HARDWARE_HAZARDS: dict[str, str] = {
    # otp/hello_otp/hello_otp.c writes OTP rows 0x400 and 0x410: permanent,
    # so it would pass once per chip and never again.
    "hello_otp": "burns OTP rows on the DUT for good",
    # These need `picotool otp load` of a signing key, which turns on secure
    # boot for good (encrypted/*, bootloaders/encrypted/*).
    "enc_bootloader": "needs secure boot turned on in the DUT's OTP",
    "hello_encrypted": "needs secure boot turned on in the DUT's OTP",
    "hello_encrypted_mbedtls": "needs secure boot turned on in the DUT's OTP",
    "hello_serial_enc": "needs secure boot turned on in the DUT's OTP",
}


# pico-examples programs one bench cannot judge although the program is
# fine, and why: by target, the harness boards (the fixture's MicroPython
# board) and the DUT builds (example.json "board") it holds on. The other
# bench still judges them, as does the emulator unless EMULATOR_LIMITS
# (emulation.py) names them.
BENCH_LIMITS: dict[str, tuple[tuple[str, ...], tuple[str, ...], str]] = {
    # pio/spi/spi_flash.c, Pico 2 build: the program raises chip select
    # after Write Enable and after the erase and lowers it in the very next
    # instruction, so CS stays high for 1-2 cycles of the DUT's 150 MHz
    # clock (7-13 ns, measured on the bench), against the W25Q16JV's 50 ns
    # tSHSL2 after an erase, program or write. Fixture 1's 125 MHz RP2040
    # harness misses some of those rises and takes two windows for one: 3
    # of 15 bench runs passed (devices/w25q/README.md). The carrier's
    # 150 MHz RP2350 harness sees them all.
    "pio_spi_flash": (("fixture1_pico", "rpi_pi_pico"), ("pico2",),
                      "holds chip select high for 1-2 DUT cycles (7-13 ns) between commands, under the "
                      "W25Q16JV's 50 ns tSHSL2, and fixture 1's 125 MHz RP2040 harness misses some of those "
                      "rises (3 of 15 bench runs passed)"),
}


# Why `target`'s `board` build is not judged on the bench whose harness
# runs `harness_board`, or None where it is.
def bench_limit(target: str, harness_board: str, board: str | None) -> str | None:
    limit = BENCH_LIMITS.get(target)
    if limit is None:
        return None
    harness_boards, boards, reason = limit
    return reason if harness_board in harness_boards and board in boards else None
