# SPDX-License-Identifier: Apache-2.0 OR MIT
# The display models built for the host with the firmware's warnings, behind
# stand-ins for the sink engines (display_models_check.c), each driven by a
# transcription of the bus traffic of the pico-examples program that uses
# it (pico-examples 0d62f75, the file and lines cited per program). What the
# model then holds must pass the checks parts.py makes on the bench, and a
# frame with one thing wrong must not.

import subprocess
from pathlib import Path

import pytest

from test.unit.programs import displays
from test.unit.programs.parts import PARTS
from test.unit.test_part_models import strict_options

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
SOURCES = ("core/src/sink.c", "devices/ssd130x/ssd130x.c", "devices/ssd130x/ssd130x_core.c",
           "devices/ht16k33/ht16k33.c", "devices/hd44780/hd44780_pcf8574.c", "devices/hd44780/hd44780_core.c",
           "devices/max7219/max7219.c", "devices/st7789/st7789v.c")
INCLUDES = ("core/include", "devices/ssd130x", "devices/ht16k33", "devices/hd44780", "devices/max7219",
            "devices/st7789")


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    exe = tmp_path_factory.mktemp("display_models") / "display_models_check"
    includes = [arg for d in INCLUDES for arg in ("-I", ROOT / d)]
    cmd = ["cc", "-O1", *strict_options(), "-fsanitize=undefined", "-fno-sanitize-recover=all", *includes,
           HERE / "display_models_check.c", *(ROOT / s for s in SOURCES), "-o", exe]
    build = subprocess.run(cmd, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return exe


class Part:
    # What the harness REPL's instance gives a check expression: properties
    # as attributes, bools as bools and arrays as tuples, and regions.
    def __init__(self, text: str):
        self._regions = {}
        for line in text.splitlines():
            kind, name, rest = line.split(" ", 2)
            if kind == "region":
                self._regions[name] = bytes.fromhex(rest)
                continue
            vtype, *values = rest.split(" ")
            got = tuple(bool(int(v)) if vtype == "bool" else int(v) for v in values)
            setattr(self, name, got[0] if len(got) == 1 else got)

    def read_region(self, name, offset, length):
        return self._regions[name][offset:offset + length]


def run(check, model, binding, script, *args):
    got = subprocess.run([check, model, binding, *args], input="\n".join(script) + "\n", capture_output=True,
                         text=True, timeout=60)
    assert got.returncode == 0, got.stdout + got.stderr
    return Part(got.stdout)


def judge(target, part):
    (attached,) = PARTS[target]
    return [c.description for c in attached.checks if not c.test(eval(c.expression, {"part": part}))]


def hexes(data) -> str:
    return " ".join(f"{b:02x}" for b in data)


# i2c/ssd1306_i2c/ssd1306_i2c.c: SSD1306_send_cmd() is a transaction of
# control byte 0x80 and the command (line 107), SSD1306_send_buf() one of
# 0x40 and the buffer (120-126), render() the area's 21h and 22h commands
# first (176-189).
def ssd1306_script(runs=((True, 3), (False, 0))):
    out = []

    def cmd(*cs):
        out.extend(f"i 80 {c:02x}" for c in cs)

    def render(buf, c0, c1, p0, p1):
        cmd(0x21, c0, c1, 0x22, p0, p1)
        out.append("i 40 " + hexes(buf))

    for whole, lines in runs:
        # SSD1306_init(), lines 132-174, for 128 x 32.
        cmd(0xAE, 0x20, 0x00, 0x40, 0xA1, 0xA8, 31, 0xC8, 0xD3, 0x00, 0xDA, 0x02, 0xD5, 0x80, 0xD9, 0xF1,
            0xDB, 0x30, 0x81, 0xFF, 0xA4, 0xA6, 0x8D, 0x14, 0x2E, 0xAF)
        buf = bytearray(512)
        render(buf, 0, 127, 0, 3)
        for _ in range(3):
            cmd(0xA5, 0xA4)
        for start in (0, 31, 62):
            render(displays.RASPBERRY, start, start + 25, 0, 3)
        # SSD1306_scroll(true), lines 178-189.
        cmd(0x26, 0x00, 0x00, 0x00, 0x03, 0x00, 0xFF, 0x2F)
        if not whole:
            break
        cmd(0x26, 0x00, 0x00, 0x00, 0x03, 0x00, 0xFF, 0x2E)
        # Some of the line drawing, which leaves pixels set across pages 0-3.
        for x in range(lines):
            buf[x] = buf[128 + x] = 0xFF
            render(buf, 0, 127, 0, 3)
        cmd(0xA7, 0xA6)
    return out


def test_ssd1306_shows_the_raspberries_after_a_restart(check):
    part = run(check, "ssd1306", "i2c", ssd1306_script())
    assert judge("ssd1306_i2c", part) == []


def test_ssd1306_check_sees_a_missing_raspberry_column(check):
    script = ssd1306_script()
    script.append("i 80 21")
    script += ["i 80 40", "i 80 40", "i 80 22", "i 80 00", "i 80 00", "i 40 00"]
    part = run(check, "ssd1306", "i2c", script)
    assert judge("ssd1306_i2c", part) == ["the three raspberries at columns 0, 31 and 62 of pages 0-3, nothing else",
                                          "SSD1306_init()'s settings, scrolling, and the last raspberry's render "
                                          "area"]


# spi/ssd1309_spi/ssd1309_spi.c: the PL022 owns chip select and raises it
# after every byte, so each byte is a window; send_command/send_data set D/C
# first (lines 45-55), display_char() sends a glyph's 8 bytes (66-77).
def ssd1309_spi_script(counts=(20, 8)):
    out = []

    def spi(dc, data):
        out.extend(f"s {dc} {b:02x}" for b in data)

    def cursor(row, col):
        spi(0, (0xB0 + (row & 7), (col * 8) & 0x0F, 0x10 + (((col * 8) & 0xF0) >> 4)))

    def string(text):
        for ch in text.upper():
            spi(1, displays.SSD1309_FONT[ch])

    for count in counts:
        out += ["r 0", "p", "r 1"]
        spi(0, (0xAF,))
        for row in range(8):
            cursor(row, 0)
            spi(1, bytes(128))
        cursor(0, 0)
        cursor(0, 0)
        string("abcdefghijklmnop")
        cursor(1, 3)
        string("qrstuvwxyz")
        for n in range(count):
            cursor(3, 7)
            string(str(n))
    return out


def test_ssd1309_spi_shows_its_text_and_counter_after_a_restart(check):
    part = run(check, "ssd1309", "spi", ssd1309_spi_script(), "reset")
    assert judge("ssd1309_spi", part) == []
    assert displays.page(part.read_region("gddram", 0, 1024), 3)[56:64] == displays.SSD1309_FONT["7"]


def test_ssd1309_spi_check_needs_the_counter_moving():
    frame = bytearray(1024)
    frame[0:128] = displays.text_cells(displays.SSD1309_FONT, "ABCDEFGHIJKLMNOP")
    frame[128 + 24:128 + 104] = displays.text_cells(displays.SSD1309_FONT, "QRSTUVWXYZ")
    frame[384 + 56:384 + 64] = displays.SSD1309_FONT["1"]
    assert not displays.ssd1309_spi_frame(bytes(frame), least=2)
    frame[384 + 56:384 + 72] = displays.text_cells(displays.SSD1309_FONT, "12")
    assert displays.ssd1309_spi_frame(bytes(frame), least=2)
    frame[128 + 30] ^= 1
    assert not displays.ssd1309_spi_frame(bytes(frame), least=2)


# spi/ssd1309_stdout_spi/ssd1309_stdout_spi.c: display_reset() (lines 92-109)
# sends AFh 20h 00h with D/C low, the 20 ms timer the 1024-byte frame buffer
# by DMA with D/C high (54-68, 179-185), each byte a window as above;
# fb_out_chars (112-142) and the snake (210-233) edit the frame buffer.
def ssd1309_stdout_script(steps=200, refresh_every=4):
    out = ["r 0", "p", "r 1"]
    out += ["s 0 af", "s 0 20", "s 0 00"]
    fb = bytearray(1024)
    cursor = 2 * 8
    for ch in "Hello, World":
        fb[cursor:cursor + 8] = displays.STDOUT_FONT[ch]
        cursor += 8

    def refresh():
        out.extend(f"s 1 {b:02x}" for b in fb)

    def pixel(x, y, on):
        if 0 <= x < 128 and 0 <= y < 64:
            if on:
                fb[x + (y // 8) * 128] |= 1 << (y % 8)
            else:
                fb[x + (y // 8) * 128] &= ~(1 << (y % 8)) & 0xFF

    refresh()
    hx, hy, hdx, hdy = 63, 63, 1, 1
    tx, ty, tdx, tdy = 9, 9, 1, 1
    for step in range(steps):
        pixel(hx, hy, True)
        pixel(tx, ty, False)
        if hx + hdx < 0 or hx + hdx >= 128:
            hdx = -hdx
        hx += hdx
        if hy + hdy <= 8 or hy + hdy >= 64:
            hdy = -hdy
        hy += hdy
        if tx + tdx < 0 or tx + tdx >= 128:
            tdx = -tdx
        tx += tdx
        if ty + tdy <= 8 or ty + tdy >= 64:
            tdy = -tdy
        ty += tdy
        if step % refresh_every == 0:
            refresh()
    return out


def test_ssd1309_stdout_shows_hello_world_and_the_snake(check):
    part = run(check, "ssd1309", "spi", ssd1309_stdout_script(), "reset")
    assert judge("ssd1309_stdout_spi", part) == []
    lit = sum(bin(b).count("1") for b in part.read_region("gddram", 128, 896))
    assert 40 <= lit <= 54


# i2c/ht16k33_i2c/ht16k33_i2c.c: ht16k33_init() is three one-byte
# transactions (90-94); ht16k33_display_set() writes the digit's RAM address
# and the pattern, low byte first (97-105).
def ht16k33_script(steps=14, partial=0):
    out = ["i 21", "i a0", "i 81"]
    out += [f"i {2 * d:02x} 00 00" for d in range(4)]
    text = displays.HT16K33_SCROLL

    def show(start, digits=5):
        for d, ch in enumerate(text[start:start + 5][:digits]):
            pattern = displays.ht16k33_pattern(ch)
            out.append(f"i {2 * d:02x} {pattern & 0xFF:02x} {pattern >> 8:02x}")

    for i in range(steps):
        show(i)
    show(steps, partial)
    return out


@pytest.mark.parametrize("partial", [0, 2])
def test_ht16k33_shows_the_scroll(check, partial):
    part = run(check, "ht16k33", "i2c", ht16k33_script(partial=partial))
    assert judge("ht16k33_i2c", part) == []


def test_ht16k33_ignores_writes_in_standby(check):
    part = run(check, "ht16k33", "i2c", ["i 00 ff ff", "i 81", "i 21", "i 02 3f 0c"])
    assert part.read_region("display", 0, 4) == bytes((0, 0, 0x3F, 0x0C))
    assert (part.oscillator, part.display_on, part.dimming) == (True, False, 15)


def test_ht16k33_check_rejects_a_wrong_digit():
    shown = bytearray(16)
    for d, ch in enumerate("come"):
        p = displays.ht16k33_pattern(ch)
        shown[2 * d:2 * d + 2] = (p & 0xFF, p >> 8)
    assert displays.ht16k33_scrolling(bytes(shown))
    shown[4] ^= 1
    assert not displays.ht16k33_scrolling(bytes(shown))


# i2c/lcd_1602_i2c/lcd_1602_i2c.c: every PCF8574 write is a one-byte
# transaction (73-77); a byte goes as two nibbles, each written, then with
# E (bit 2) set and cleared (79-97), RS in bit 0, the backlight in bit 3.
def lcd_script(screens=1, cut_after=None):
    out = []

    def write(v):
        out.append(f"i {v:02x}")

    def send(val, mode):
        for nibble in ((val & 0xF0), ((val << 4) & 0xF0)):
            b = mode | nibble | 0x08
            write(b)
            write(b | 0x04)
            write(b & ~0x04)
            if cut_after is not None and len(out) >= cut_after:
                return True
        return False

    def init():
        for v in (0x03, 0x03, 0x03, 0x02, 0x06, 0x28, 0x0C, 0x01):
            send(v, 0)

    init()
    for m in range(0, 2 * screens, 2):
        for line in range(2):
            msg = displays.LCD_MESSAGES[m + line]
            send((0x80 if line == 0 else 0xC0) + 8 - len(msg) // 2, 0)
            for ch in msg:
                if send(ord(ch), 1):
                    return out
        send(0x01, 0)
    return out


def test_lcd_shows_the_first_screen(check):
    part = run(check, "hd44780_pcf8574", "i2c", lcd_script(screens=0) + lcd_script(screens=1)[:-6])
    assert judge("lcd_1602_i2c", part) == []
    assert part.read_region("ddram", 0, 16) == b"    RP2040 by   "


def test_lcd_recovers_from_a_restart_between_two_nibbles(check):
    cut = lcd_script(screens=3, cut_after=len(lcd_script(screens=2)) + 3 * 9)
    assert len(cut) % 6 == 3
    part = run(check, "hd44780_pcf8574", "i2c", cut + lcd_script(screens=1)[:-6])
    assert judge("lcd_1602_i2c", part) == []


def test_lcd_check_rejects_a_screen_half_written(check):
    part = run(check, "hd44780_pcf8574", "i2c", lcd_script(screens=1, cut_after=len(lcd_script(screens=0)) + 60))
    assert judge("lcd_1602_i2c", part) == ["a pair of the program's messages, centred, the rest of the DDRAM spaces"]


# spi/max7219_*: write_register_all() is one chip-select window of the
# register and data bytes once per module (both files, lines 66-75).
def max7219_init(modules, decode):
    return [f"s 0 {hexes((reg, val) * modules)}"
            for reg, val in ((0x0C, 0), (0x0F, 0), (0x0B, 7), (0x09, decode), (0x0C, 1), (0x0A, 8))]


def test_max7219_32x8_ramps_all_four_modules_alike(check):
    script = max7219_init(4, 0)
    for i in range(51):
        script += [f"s 0 {hexes((1 + j, i) * 4)}" for j in range(8)]
    script += [f"s 0 {hexes((1 + j, 51) * 4)}" for j in range(3)]
    part = run(check, "max7219", "spi", script, "chain=4")
    assert judge("max7219_32x8_spi", part) == []
    assert part.read_region("digits", 0, 8) == bytes((51, 51, 51, 50, 50, 50, 50, 50))


# The no-op register (p. 10): with four parts cascaded, the first word sent
# reaches the fourth. A shorter window leaves the words it did not push out
# where they were, and LOAD latches them again.
def test_max7219_cascade_latches_each_module_its_own_word(check):
    part = run(check, "max7219", "spi", ["s 0 0a 01 0a 02 0a 03 0a 04"], "chain=4")
    assert part.intensity[:4] == (4, 3, 2, 1)
    part = run(check, "max7219", "spi", ["s 0 0a 01 0a 02 0a 03 0a 04", "s 0 01 0b 00 00"], "chain=4")
    assert part.intensity[:4] == (4, 3, 4, 3)
    assert part.read_region("digits", 0, 32)[::8] == bytes((0, 0x0B, 0, 0))


def test_max7219_8x7seg_counts(check):
    script = max7219_init(1, 0xFF) + [f"s 0 {hexes((1 + j, 0))}" for j in range(8)]
    for j in range(1000):
        digit, num = 0, j
        while num and digit < 8:
            script.append(f"s 0 {hexes((1 + digit, num % 10))}")
            num //= 10
            digit += 1
    part = run(check, "max7219", "spi", script)
    assert judge("max7219_8x7seg_spi", part) == []
    assert part.read_region("digits", 0, 8) == bytes((9, 9, 9, 0, 0, 0, 0, 0))


# pio/st7789_lcd/st7789_lcd.c:39-50, the init sequence, then RAMWR and
# pixels, MSB first; D/C low for each command byte.
def test_st7789v_takes_the_programs_init_and_a_frame(check):
    init = [(0x01,), (0x11,), (0x3A, 0x55), (0x36, 0x00), (0x2A, 0, 0, 0, 240), (0x2B, 0, 0, 0, 240),
            (0x21,), (0x13,), (0x29,)]
    script = []
    for cmd, *params in init:
        script.append(f"s 0 {cmd:02x}")
        if params:
            script.append(f"s 1 {hexes(params)}")
    script.append("s 0 2a")
    script.append("s 1 00 00 00 ef")
    script.append("s 0 2b")
    script.append("s 1 00 00 01 3f")
    script.append("s 0 2c")
    pixels = bytearray()
    for y in range(320):
        for x in range(240):
            pixels += ((x * 7 + y) & 0xFFFF).to_bytes(2, "big")
    script += [f"s 1 {hexes(pixels[at:at + 4096])}" for at in range(0, len(pixels), 4096)]
    part = run(check, "st7789v", "spi", script)
    assert (part.sleeping, part.display_on, part.inversion, part.colmod, part.madctl) == (False, True, True, 0x55, 0)
    assert (part.column_range, part.row_range, part.memory_writes, part.pixels) == ((0, 239), (0, 319), 1, 76800)
    thumb = part.read_region("thumbnail", 0, 2400)
    for ty in (0, 13, 39):
        for tx in (0, 17, 29):
            want = (8 * tx * 7 + 8 * ty) & 0xFFFF
            assert int.from_bytes(thumb[2 * (30 * ty + tx):2 * (30 * ty + tx) + 2], "big") == want
