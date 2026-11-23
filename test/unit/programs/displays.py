# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples display programs draw, from their sources
# (pico-examples 0d62f75), and the judgments parts.py's checks make of what
# a display model holds once a program has run. The bitmaps are the
# programs' own, copied byte for byte from the files and lines cited.

SSD1309_FONT = {
    ' ': bytes.fromhex('0000000000000000'),  # spi/ssd1309_spi/ssd1309_font.h:18
    'A': bytes.fromhex('7814121112147800'),  # spi/ssd1309_spi/ssd1309_font.h:19
    'B': bytes.fromhex('7f49494949497e00'),  # spi/ssd1309_spi/ssd1309_font.h:20
    'C': bytes.fromhex('7e41414141414100'),  # spi/ssd1309_spi/ssd1309_font.h:21
    'D': bytes.fromhex('7f41414141417e00'),  # spi/ssd1309_spi/ssd1309_font.h:22
    'E': bytes.fromhex('7f49494949494900'),  # spi/ssd1309_spi/ssd1309_font.h:23
    'F': bytes.fromhex('7f09090909010100'),  # spi/ssd1309_spi/ssd1309_font.h:24
    'G': bytes.fromhex('7e41414151517200'),  # spi/ssd1309_spi/ssd1309_font.h:25
    'H': bytes.fromhex('7f08080808087f00'),  # spi/ssd1309_spi/ssd1309_font.h:26
    'I': bytes.fromhex('0000007f00000000'),  # spi/ssd1309_spi/ssd1309_font.h:27
    'J': bytes.fromhex('002141413f010100'),  # spi/ssd1309_spi/ssd1309_font.h:28
    'K': bytes.fromhex('007f080814224100'),  # spi/ssd1309_spi/ssd1309_font.h:29
    'L': bytes.fromhex('007f404040404000'),  # spi/ssd1309_spi/ssd1309_font.h:30
    'M': bytes.fromhex('7f02040804027f00'),  # spi/ssd1309_spi/ssd1309_font.h:31
    'N': bytes.fromhex('7f02040810207f00'),  # spi/ssd1309_spi/ssd1309_font.h:32
    'O': bytes.fromhex('3e41414141413e00'),  # spi/ssd1309_spi/ssd1309_font.h:33
    'P': bytes.fromhex('7f11111111110e00'),  # spi/ssd1309_spi/ssd1309_font.h:34
    'Q': bytes.fromhex('3e41414951617e00'),  # spi/ssd1309_spi/ssd1309_font.h:35
    'R': bytes.fromhex('7f11111131510e00'),  # spi/ssd1309_spi/ssd1309_font.h:36
    'S': bytes.fromhex('4649494949300000'),  # spi/ssd1309_spi/ssd1309_font.h:37
    'T': bytes.fromhex('0101017f01010100'),  # spi/ssd1309_spi/ssd1309_font.h:38
    'U': bytes.fromhex('3f40404040403f00'),  # spi/ssd1309_spi/ssd1309_font.h:39
    'V': bytes.fromhex('0f10204020100f00'),  # spi/ssd1309_spi/ssd1309_font.h:40
    'W': bytes.fromhex('7f20100810207f00'),  # spi/ssd1309_spi/ssd1309_font.h:41
    'X': bytes.fromhex('4122140814224100'),  # spi/ssd1309_spi/ssd1309_font.h:42
    'Y': bytes.fromhex('0102047804020100'),  # spi/ssd1309_spi/ssd1309_font.h:43
    'Z': bytes.fromhex('4161594543410000'),  # spi/ssd1309_spi/ssd1309_font.h:44
    '0': bytes.fromhex('003e4149413e0000'),  # spi/ssd1309_spi/ssd1309_font.h:45
    '1': bytes.fromhex('0000427f40000000'),  # spi/ssd1309_spi/ssd1309_font.h:46
    '2': bytes.fromhex('0062514949460000'),  # spi/ssd1309_spi/ssd1309_font.h:47
    '3': bytes.fromhex('0049494949360000'),  # spi/ssd1309_spi/ssd1309_font.h:48
    '4': bytes.fromhex('003f207820200000'),  # spi/ssd1309_spi/ssd1309_font.h:49
    '5': bytes.fromhex('4f49494949300000'),  # spi/ssd1309_spi/ssd1309_font.h:50
    '6': bytes.fromhex('3e49494949300000'),  # spi/ssd1309_spi/ssd1309_font.h:51
    '7': bytes.fromhex('010161110d030000'),  # spi/ssd1309_spi/ssd1309_font.h:52
    '8': bytes.fromhex('0036494949493600'),  # spi/ssd1309_spi/ssd1309_font.h:53
    '9': bytes.fromhex('00060909097e0000'),  # spi/ssd1309_spi/ssd1309_font.h:54
}

STDOUT_FONT = {
    ' ': bytes.fromhex('0000000000000000'),  # spi/ssd1309_stdout_spi/font.h:23
    ',': bytes.fromhex('0000806000000000'),  # spi/ssd1309_stdout_spi/font.h:35
    'H': bytes.fromhex('00fe10101010fe00'),  # spi/ssd1309_stdout_spi/font.h:63
    'W': bytes.fromhex('007e80807080807e'),  # spi/ssd1309_stdout_spi/font.h:78
    'd': bytes.fromhex('006090909062fe80'),  # spi/ssd1309_stdout_spi/font.h:91
    'e': bytes.fromhex('0070a8a8a8a83000'),  # spi/ssd1309_stdout_spi/font.h:92
    'l': bytes.fromhex('000082fe80000000'),  # spi/ssd1309_stdout_spi/font.h:99
    'o': bytes.fromhex('0070888888887000'),  # spi/ssd1309_stdout_spi/font.h:102
    'r': bytes.fromhex('0088f89008083000'),  # spi/ssd1309_stdout_spi/font.h:105
}

# i2c/ssd1306_i2c/raspberry26x32.h:4, 104 bytes: IMG_WIDTH 26 columns of IMG_HEIGHT 32 rows,
# a page of 26 columns after another.
RASPBERRY = bytes.fromhex('00000e7efefffffffffffefefcf8fcfefefffffffffffe7e1e00000080e0f8fdfffffffffffffffffffffffffffffffdf8e08000001e7fffffffffffffffffffffffffffffffffffffffffff7f1e00000003070f1f1f3f3f7fffffffff7f7f3f3f1f1f0f07030000')

# i2c/ht16k33_i2c/ht16k33_i2c.c:63-71, char_to_pattern(): 14-segment
# patterns for A..Z and 0..9; any other character is 0.
HT16K33_ALPHA = (0xF7, 0x128F, 0x39, 0x120F, 0xF9, 0xF1, 0xBD, 0xF6, 0x1209, 0x1E, 0x2470, 0x38, 0x536, 0x2136,
                 0x3F, 0xF3, 0x203F, 0x20F3, 0x18D, 0x1201, 0x3E, 0xC30, 0x2836, 0x2D00, 0x1500, 0xC09)
HT16K33_NUM = (0xC3F, 0x406, 0xDB, 0x8F, 0xE6, 0xED, 0xFD, 0x1401, 0xFF, 0xE7)
# ht16k33_i2c.c:175.
HT16K33_SCROLL = "Welcome to the Raspberry Pi Pico"

# i2c/lcd_1602_i2c/lcd_1602_i2c.c:147-154, shown two lines at a time.
LCD_MESSAGES = ("RP2040 by", "Raspberry Pi", "A brand new", "microcontroller", "Twin core M0", "Full C SDK",
                "More power in", "your product", "More beans", "than Heinz!")

PAGE = 128


def page(frame: bytes, p: int) -> bytes:
    return frame[PAGE * p:PAGE * (p + 1)]


def text_cells(font: dict, text: str) -> bytes:
    return b"".join(font[c] for c in text)


# i2c/ssd1306_i2c/ssd1306_i2c.c:345-376: the zeroed render of the 128 x 32
# frame (pages 0-3), then render(raspberry26x32, area) three times from
# column 0, 5 + IMG_WIDTH apart, before SSD1306_scroll(true) and its 5 s.
# Pages 4-7 are never written.
def ssd1306_raspberries() -> bytes:
    frame = bytearray(PAGE * 8)
    for start in (0, 31, 62):
        for p in range(4):
            frame[PAGE * p + start:PAGE * p + start + 26] = RASPBERRY[26 * p:26 * (p + 1)]
    return bytes(frame)


def _digits(cells: bytes, font: dict) -> str | None:
    glyphs = {font[c]: c for c in "0123456789"}
    text = ""
    for at in range(0, len(cells), 8):
        cell = cells[at:at + 8]
        if cell == bytes(8):
            return text if cells[at:] == bytes(len(cells) - at) else None
        if cell not in glyphs:
            return None
        text += glyphs[cell]
    return text


# spi/ssd1309_spi/ssd1309_spi.c:129-151 in page addressing mode after
# display_clear(): row 0 column 0 "abcdefghijklmnop", row 1 column 3
# "qrstuvwxyz", both through toupper(); then the counter, "%u" at row 3
# column 7, every 250 ms. Columns are 8 wide.
def ssd1309_spi_frame(frame: bytes, least: int) -> bool:
    if len(frame) != PAGE * 8 or page(frame, 0) != text_cells(SSD1309_FONT, "ABCDEFGHIJKLMNOP"):
        return False
    if page(frame, 1) != bytes(24) + text_cells(SSD1309_FONT, "QRSTUVWXYZ") + bytes(24):
        return False
    if page(frame, 2) != bytes(PAGE) or frame[PAGE * 4:] != bytes(PAGE * 4):
        return False
    row3 = page(frame, 3)
    count = _digits(row3[56:], SSD1309_FONT)
    return row3[:56] == bytes(56) and bool(count) and (count == "0" or count[0] != "0") and int(count) >= least


# spi/ssd1309_stdout_spi/ssd1309_stdout_spi.c:208-209: set_cursor_pos(0, 2),
# printf("Hello, World\n") into the frame buffer from byte 16; the snake
# runs below row 8 (lines 214-233), its tail 54 steps behind its head on
# the same path, so at most 54 pixels are lit there.
def ssd1309_stdout_frame(frame: bytes) -> bool:
    if len(frame) != PAGE * 8 or page(frame, 0) != bytes(16) + text_cells(STDOUT_FONT, "Hello, World") + bytes(16):
        return False
    lit = sum(bin(b).count("1") for b in frame[PAGE:])
    return 1 <= lit <= 60


def ht16k33_pattern(ch: str) -> int:
    if ch.isalpha():
        return HT16K33_ALPHA[ord(ch.upper()) - ord("A")]
    if ch.isdigit():
        return HT16K33_NUM[ord(ch) - ord("0")]
    return 0


# ht16k33_i2c.c:111-131: ht16k33_scroll_string() shows str[i:] from digit 0
# for i = 0 .. 28, 150 ms apart, digit d at RAM address 2d, low byte first.
# Between two steps the first k digits may already show step i + 1.
def ht16k33_scrolling(display: bytes) -> bool:
    if len(display) != 16:
        return False
    shown = [display[2 * d] | display[2 * d + 1] << 8 for d in range(4)]
    s = HT16K33_SCROLL
    for i in range(len(s) - 3):
        for k in range(5):
            if k and i + 4 >= len(s):
                break
            want = [ht16k33_pattern(s[i + 1 + d]) if d < k else ht16k33_pattern(s[i + d]) for d in range(4)]
            if shown == want:
                return True
    return False


# lcd_1602_i2c.c:156-165: each pair of messages centred on lines 0 and 1
# at (MAX_CHARS / 2) - strlen / 2, shown for 2 s, then lcd_clear(). The
# DDRAM is the 2-line map, line 0 at 00h-27h and line 1 at 40h-67h.
def lcd_screens() -> list[bytes]:
    screens = []
    for m in range(0, len(LCD_MESSAGES), 2):
        ddram = bytearray(b" " * 128)
        for line, base in ((0, 0x00), (1, 0x40)):
            text = LCD_MESSAGES[m + line].encode()
            at = base + 16 // 2 - len(text) // 2
            ddram[at:at + len(text)] = text
        screens.append(bytes(ddram))
    return screens


def lcd_showing_a_screen(ddram: bytes) -> bool:
    return ddram in lcd_screens()


# spi/max7219_8x7seg_spi/max7219_8x7seg_spi.c:129-146: decode mode on every
# digit, clear(), then display_num(j++) every millisecond or so, digit 0
# the least significant; digits above the number's length stay 0.
def max7219_counting(digits: bytes, least: int) -> bool:
    mine = digits[:8]
    if len(mine) != 8 or any(d > 9 for d in mine):
        return False
    return sum(d * 10 ** k for k, d in enumerate(mine)) >= least


# spi/max7219_32x8_spi/max7219_32x8_spi.c:116-121: for i in 0..255 every
# digit of all four modules is written i, then 20 ms; write_register_all()
# loads the four alike. Between two passes the first k digits may show
# i + 1 already.
def max7219_ramp(digits: bytes, least: int) -> bool:
    chips = [digits[8 * k:8 * (k + 1)] for k in range(4)]
    if len(digits) < 32 or any(c != chips[0] for c in chips):
        return False
    d = chips[0]
    i = d[7]
    k = next((n for n in range(8) if d[n] != (i + 1) % 256), 8)
    return all(v == (i + 1) % 256 for v in d[:k]) and all(v == i for v in d[k:]) and i >= least
