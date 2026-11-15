# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples LED programs show, from their sources (pico-examples
# 0d62f75), and the judgments parts.py's checks make of what an LED or panel
# model holds once a program has run. The programs draw from newlib's rand(),
# which the pico builds link (the LCG below, rand.c), seeded with 1 as at
# reset, so every frame they send is known.

import math
import struct

_LCG_MUL = 0x5851F42D4C957F2D


def newlib_rand():
    state = 1
    while True:
        state = (state * _LCG_MUL + 1) & 0xFFFFFFFFFFFFFFFF
        yield (state >> 32) & 0x7FFFFFFF


# pio/ws2812/ws2812.c:100-108, 128-139 and ws2812_parallel.c:115-125,
# 303-308: the first pattern is pattern_table[rand() % 4], then the
# direction from rand() >> 30, and (parallel only) rand() & 1 for none.
PATTERNS = ("Snakes!", "Random data", "Sparkles", "Greys")


def first_pattern(parallel: bool) -> tuple[str, str]:
    r = newlib_rand()
    pattern = PATTERNS[next(r) % 4]
    direction = 1 if (next(r) >> 30) & 1 else -1
    if parallel and next(r) & 1:
        direction = 0
    return pattern, {1: "(forward)", -1: "(backward)", 0: "(still)"}[direction]


def _grb(pixel: int) -> bytes:
    return bytes(((pixel >> 16) & 0xFF, (pixel >> 8) & 0xFF, pixel & 0xFF))


# ws2812.c:76-81, 134-138: "Random data" sends 150 pixels of rand() every
# eighth step (put_pixel shifts each left by 8, so its low 24 bits go out
# as G, R, B), and nothing in between: frame k is rand() calls 3 + 150k on.
def ws2812_random_frames(count: int, pixels: int = 150):
    r = newlib_rand()
    next(r), next(r)
    for _ in range(count):
        yield b"".join(_grb(next(r)) for _ in range(pixels))


def ws2812_shows_a_random_frame(leds: bytes, within: int = 130) -> bool:
    return any(leds == frame for frame in ws2812_random_frames(within))


# ws2812_parallel.c: both strips' data from the pattern (strip 0 RGB,
# strip 1 RGBW with W 0), scaled by each strip's frac_brightness and the
# brightness ramp (transform_strips, 159-175), dithered against the last
# state's fractional bits (add_error, dither_values, 135-181), and sent as
# the top eight of twelve bit planes, one bit per strip in each word
# (output_strips_dma, 274-280). A frame is 256 values for each strip.
VALUES = 64 * 4
FRAC_BITS = 4


def _pattern_random_data(r, t, data, four):
    if t % 8:
        return
    at = 0
    for _ in range(64):
        pixel = next(r)
        for byte in _grb(pixel) + (b"\x00" if four else b""):
            data[at] = byte
            at += 1


def ws2812_parallel_frames(count: int):
    # Yields (strip 0 bytes, strip 1 bytes) as each frame goes out.
    r = newlib_rand()
    pattern = next(r) % 4
    direction = 1 if (next(r) >> 30) & 1 else -1
    if next(r) & 1:
        direction = 0
    assert PATTERNS[pattern] == "Random data"
    strips = ((bytearray(192), 0x40, False), (bytearray(256), 0x100, True))
    states = [[(0, 0)] * VALUES, [(0, 0)] * VALUES]
    current, t, brightness = 0, 0, 0
    for _ in range(count):
        for data, _, four in strips:
            _pattern_random_data(r, t, data, four)
        old = states[current ^ 1]
        state = []
        for v in range(VALUES):
            pair = []
            for s, (data, frac, _) in enumerate(strips):
                value = ((data[v] * frac) >> 8) * brightness >> 8 if v < len(data) else 0
                pair.append((value + (old[v][s] & 0xF)) & 0xFFF)
            state.append(tuple(pair))
        states[current] = state
        yield tuple(bytes((state[v][s] >> FRAC_BITS) & 0xFF for v in range(VALUES)) for s in range(2))
        current ^= 1
        t += direction
        brightness += 1
        if brightness == 0x20 << FRAC_BITS:
            brightness = 0


def ws2812_parallel_shows_a_frame(leds: bytes, strip: int, within: int = 3000) -> bool:
    return any(leds == frame[strip][:len(leds)] for frame in ws2812_parallel_frames(within))


def _f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


# pio/apa102/apa102.c:56-57: wave_table[i] = (uint8_t)(powf(sinf(i * M_PI /
# 256), 5.f) * 255), in single precision; the host's maths may land an
# entry one count off the DUT's, which the checks allow.
WAVE = tuple(int(_f32(_f32(_f32(math.sin(_f32(i * math.pi / 256))) ** 5) * 255)) for i in range(256))


# apa102.c:59-72: frame t is LED i at global brightness 16 (0xE0 | 16) with
# red wave[(i + t) % 256], green wave[(2i + 3t) % 256], blue
# wave[(3i + 4t) % 256]; the datasheet's LED frame is the brightness byte,
# then blue, green, red.
def apa102_frame(t: int, leds: int = 150) -> bytes:
    out = bytearray()
    for i in range(leds):
        out += bytes((0xF0, WAVE[(3 * i + 4 * t) % 256], WAVE[(2 * i + 3 * t) % 256], WAVE[(i + t) % 256]))
    return bytes(out)


def apa102_shows_a_frame(leds: bytes) -> bool:
    for t in range(256):
        want = apa102_frame(t, len(leds) // 4)
        if all(abs(a - b) <= 1 for a, b in zip(leds, want)) and leds[0::4] == want[0::4]:
            return True
    return False


# pio/hub75/mountains_128x64_rgb565.h: pixel (8c, y) for c < 16 and every
# row y, little-endian RGB 5-6-5; each is the first two bytes of the
# header's line 2 + 16y + c.
HUB75_THUMB_RGB565 = (
    "ffffe7ff04bf04df04df04bf04bf04dfffffffff04bf7effd7ff051f04ff04ff"
    "ffff051f051f051f051f04ff04ff051fffffffff04ff9f7fffff05df051f053f"
    "ffff04df053f053f051f051f04ffffffffffffff04dfffffefff053f051f053f"
    "ffff051f055f057f055f053f053fffffffffffff04ffffff4e3f055f055f055f"
    "d6dd051f057f057f057f055f053fffffffffffff051fffffefff057f055f055f"
    "53516ebf0ddf05bf059f057f055ffffff7ffffff053fc7ffffff059f057f057f"
    "ffff7f1f05ff05bf059f057f057fffffc7bfffff055fffff4e5f059f057f057f"
    "9cf61ddf05ff05ff05df05bf05dfd7ff5e5fb77f059f4e9f05df05df059f059f"
    "be3aef9f05df061f05ff05bf059f0dff055fc7df05bf05bf05bf05bf059f059f"
    "ffffffff255f063f061f05ff05df369f055ff7ff05df05df05df05ff05df05df"
    "ffffffff21aa063f061f05ff05df05bf059fb7ff05ff05df05df05df05ff05df"
    "ffffffff322c167f0e5f061f05ff05ff05ff05df063f46bf061f061f061f061f"
    "e6fbffff324c369f065f061f061f05ff05ff05ff061fafdf061f05ff063f05ff"
    "c5f6ffff2a0b6c360e9f065f065f065f067f5edfc7ffffff065f061fdfff061f"
    "c617a5134b3052cb069f065f065f065fefffffffffffffff065f065fffff061f"
    "a514bdb5ffff426c1ebf069f069f069fffffffffa59affff56dfefffffff063f"
    "d678ad33ffff3a6be7ff069f471fc7ffe7ffffff9d375c15ffff067fffff5edf"
    "9470c5f6945129aaffff4edf3f1fffffc73ff7ff7cd784d7ffff5effe7dfffff"
    "deb9bdd6e71ba4d3ffff877f2edfd6de64786cbadf3d4b32fffffffffffff7ff"
    "8c4e9470ad338c51b596ffffffffc65d4bd66cba95387c75ef7fffffffffffff"
    "320594b0a5324a8973aeffff7d3a74b953f7963f8497539384f7ffffdfffd7ff"
    "5ac9a4af83ee3a28738ed71f7d5c753bd75f6437d6bd2250d6bdffff97bfffff"
    "29a562e87bcd52eaa533ffffffff4bd5ffff32d0a578df3fce7afffff7ffffff"
    "214339e7426783efacf3ffffffff7cfac79f2ab184b6c618f75fc6dfffffffff"
    "636a2164198373cd7bf0ffbeffff5c167d3c226fcebc8cb474347496ffffad96"
    "31c54a8821a46b6c83ef4acc8d1774b98ddf8cd6f7bf63b2a536adb9ffffad97"
    "29a51963192273ac73cd4b0dffff020fc6de94f6ce9b328d7c537c33be7c4a8a"
    "4a683a07218510c27bce5b0effff1a51ffffa5789d163a6e9d379cf6ce5a31e7"
    "192219031963110329a78c93ffff01ce63b24b0f42cf328d94d484338c9429e0"
    "18e431a54a692164632dbdd9d67b09ce3ab04b51220c2a4b42ad847429662161"
    "088210e310c429c54a8a73d0743301ad63915b912a4c21eb5b509d5708411102"
    "18c318c3528a214439e821676bd201ad7c53430f3aad19aa3a6d5b7031e229e1"
    "006221045ac910a310e329861988018b53300149222b11aa0988535021a01920"
    "18e421044a6710c321663a0a194701cd63d2012a19ea1169118a4acd21611080"
    "08412965298510a211041925320909ec42ee010911eb0948114929e729c20880"
    "18e310c318c3294510c429654a8b1a4d63ee118a19ab116921e829c121422142"
    "08820881110210a208c242485b2c32cd3aed224c21ea094821e921e119011961"
    "1082190310c318c3110429865b2c7470328b222b42ce11482a062a2019412142"
    "10c3190208c210a210c310e34229c67a747063cf3aab094829c421c121a21922"
    "10a34a2710c3210419041905634dad55df1c8cb36390116921a4216121821902"
    "08a218e310c210e310c3190431e63a68ffdece594acc094708a12181086010c0"
    "4a8710c3294418c318e31904426a6caa8edbe71a9cf311480881190210612162"
    "214310a2212410c310e3298531e85c490dd7aef9deda4b4b1942088110c129a2"
    "190310a218c318c321046b8d21045348061915f6e6d9736b08a010c110e11901"
    "10c210a2088118e310c384e95b8ade98061915f7ded9006108411901218218c2"
    "10e218a218e318c218e47ca85ba7ff9c0e5b1658555408410841190218e318c3"
    "10a2108139e6108208823a65846bf77b0e39163805330040086110c210c11082"
    "108210a3212518c310e33224f73bdeba26170533136c18c10841212219020822"
    "10c2086110c210e3194342c66b8e39c74a8a25d611a531a50841190118e210c1"
    "31c6108218e318c3218521a4088439e731866baf19241080106110a229642124"
    "10821082088210a2212431e54a493186318629a73a03194218c2214308416bc5"
    "18a318e3192318a3210421046bcc18c4318718c431641082108219027b2b6b27"
    "210408813285108210c3190419430842210510c242632122190208603a057349"
    "10c221242a4419041904298418e4106310a410c231a339e339e441e6738862e5"
    "08a118e33a85110331a529a410c33a850861088119023181298152a463094226"
    "19221081196318c339e5214431e742c710c21061214239e34a665b047beb62eb"
    "19611082218308613a4432041103326508a12162110119216b4b946e6bc52964"
    "21823b043a85296429c529e23a67192419034a0419025ac883a662c84ac339a5"
    "114029e131e432241142196221830881108252a6214283eb638284073a4362c8"
    "212229a1320429a5190321a329642103210231c3630a94e873e17c05842383e8"
    "2141218132602162088210c118e3108142e14a46844d74248403e6cd73455ac5"
    "2963216229a34ac510c31082210318c342a094715aaaa50f7bc2cdcc6b476b24"
    "10a131e5196142a310a21081190208c15ae783ce6b496b496b608c03d5ed7363"
    "10823164216232211923108118c11942a4ef94315aa67b4b7c23e609cda993e7"
)


def hub75_expected_thumbnail() -> bytes:
    # hub75.c:27-35, gamma_correct_565_888(); the panel's thumbnail is red,
    # green, blue per pixel.
    raw = "".join(HUB75_THUMB_RGB565)
    out = bytearray()
    for i in range(0, len(raw), 4):
        pix = int(raw[i:i + 4], 16)
        r = ((pix & 0xF800) ** 2) >> 24
        g = ((pix & 0x07E0) ** 2) >> 14 & 0xFF
        b = ((pix & 0x001F) ** 2) >> 2 & 0xFF
        out += bytes((r, g, b))
    return bytes(out)


def hub75_shows_the_mountains(thumbnail: bytes) -> bool:
    want = hub75_expected_thumbnail()
    return len(thumbnail) == len(want) and all(abs(a - b) <= 2 for a, b in zip(thumbnail, want))


def ds18b20_rom(serial: int) -> int:
    # DS18B20 figure 6: family 28h, the 48-bit serial, the CRC of both; the
    # program prints the 64 bits as one number, family code lowest.
    rom = bytes((0x28,)) + serial.to_bytes(6, "little")
    crc = 0
    for byte in rom:
        for _ in range(8):
            mix = (crc ^ byte) & 1
            crc >>= 1
            if mix:
                crc ^= 0x8C
            byte >>= 1
    return int.from_bytes(rom + bytes((crc,)), "little")
