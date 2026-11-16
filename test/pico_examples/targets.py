# SPDX-License-Identifier: Apache-2.0 OR MIT
# What "completed" means for each pico-examples program, from its own source:
# the lines it prints on the stdio UART, in order, and what it must never
# print. A program runs unmodified, built for the DUT's board from the pinned
# pico-examples tree (example.json beside the ELF names it).

import re
from dataclasses import dataclass, field, replace

# The SDK's panic() and assert() paths, and a fault the example would print.
FORBIDDEN = (r"\*\*\* PANIC \*\*\*", r"assertion \".*\" failed", r"HardFault")


@dataclass(frozen=True)
class Spec:
    # Regular expressions, each matched after the previous one's match.
    console: tuple[str, ...]
    # Bench seconds the program gets to print them all.
    seconds: float = 5.0
    forbidden: tuple[str, ...] = FORBIDDEN
    # The source the criteria were read from, relative to pico-examples.
    source: str = ""
    notes: str = ""
    extra: dict = field(default_factory=dict)
    # The boards (example.json "board") whose build this Spec can judge;
    # empty for every board it builds for.
    boards: tuple[str, ...] = ()
    # Fields that differ in one board's build, by board: {"pico": {...}}.
    per_board: dict = field(default_factory=dict)
    # Fields that differ on a bench whose lines have pull-ups (the carrier's
    # 5.1 k on every Pico GP0-22), where an undriven line reads high.
    with_line_pullups: dict = field(default_factory=dict)
    # What the case does as the program runs, in order, each once `pattern`
    # has matched after where the previous one did: (pattern, bytes) types
    # on the console, (pattern, str) runs code on the harness, for a part
    # that is to act then.
    input: tuple[tuple[str, bytes | str], ...] = ()
    # Patterns as `console`, matched on the DUT's own USB console instead:
    # the first CDC ACM function of the device it presents
    # (test/pico_examples/usb_devices.py), which reaches the rig host only
    # in the emulator, and the same `seconds` and `forbidden`. A Spec with
    # these and no `console` prints nothing on its UART.
    usb_console: tuple[str, ...] = ()
    # As `input`, for the USB console: (pattern, interface string, bytes)
    # writes the bytes to that CDC function of the DUT's device.
    usb_input: tuple[tuple[str, str, bytes], ...] = ()
    # (interface string, pattern): what a CDC function of the DUT's device
    # answers on its own tty, read once the USB console's patterns matched,
    # within the same `seconds`.
    usb_replies: tuple[tuple[str, str], ...] = ()
    # The DUT pins these criteria take an analog level on, and the fields
    # that differ on a bench whose harness can only drive them high or low
    # (fixture "analog_pins").
    analog_pins: tuple[int, ...] = ()
    without_analog: dict = field(default_factory=dict)

    def for_board(self, board: str | None) -> "Spec | None":
        # The criteria for this board's build, or None where they cannot judge it.
        if self.boards and board not in self.boards:
            return None
        return replace(self, boards=(), per_board={}, **self.per_board.get(board, {}))

    def for_bench(self, line_pullups: bool, analog_pins=()) -> "Spec":
        spec = self
        if line_pullups and spec.with_line_pullups:
            spec = replace(spec, with_line_pullups={}, **spec.with_line_pullups)
        if not set(spec.analog_pins) <= set(analog_pins) and spec.without_analog:
            spec = replace(spec, without_analog={}, **spec.without_analog)
        return spec

    def match(self, text: str, patterns: tuple[str, ...] | None = None) -> tuple[list[str], list[str]]:
        # The expected patterns (`console` unless given) not found in order,
        # and the forbidden ones found.
        missing, at = [], 0
        for pattern in self.console if patterns is None else patterns:
            m = re.compile(pattern).search(text, at)
            if m is None:
                missing.append(pattern)
            else:
                at = m.end()
        found = [p for p in self.forbidden if re.search(p, text)]
        return missing, found


NL = r"\r?\n"
# One 16-byte row of "%02x " bytes, the last one followed by a newline.
HEX_ROW = r"(?:[0-9a-f]{2} ){15}[0-9a-f]{2}\r?\n"
# A floating-point value as %f, %g or %.Nf print it.
NUM = r"-?[0-9.]+(?:e[-+]?\d+)?"


def forbid(*patterns: str) -> tuple[str, ...]:
    return FORBIDDEN + patterns


# FreeRTOS tasks must not return, but hello_freertos's main_task does once it
# prints its exit line; the ports then panic or spin (see its entries). A
# panic is a failure only before that line.
_PANIC_BEFORE_FREERTOS_EXIT = r"\A(?:(?!main_task is exiting now)[\s\S])*\*\*\* PANIC \*\*\*"


def _freertos(banner: str) -> "Spec":
    main = [rf"Hello from main task count={n}\r?\n" for n in range(10)]
    return Spec(
        console=(banner, *main[:4], r"Hello from worker count=1\r?\n", *main[4:7],
                 r"Hello from worker count=2\r?\n", *main[7:], r"main_task is exiting now\r?\n"),
        seconds=60.0,
        forbidden=(_PANIC_BEFORE_FREERTOS_EXIT, *FORBIDDEN[1:]),
        source="freertos/hello_freertos/hello_freertos.c",
    )


def _bus_scan(title: str) -> tuple[str, ...]:
    rows = tuple(rf"{r}0 (?:[.@]  ){{15}}[.@]\r?\n" for r in range(8))
    return (title + NL, r"   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r?\n", *rows, r"Done\." + NL)


_FREQS = ("pll_sys  ", "pll_usb  ", "rosc     ", "clk_sys  ", "clk_peri ", "clk_usb  ", "clk_adc  ")


def _hello_48mhz(with_rtc: bool) -> tuple[str, ...]:
    names = _FREQS + (("clk_rtc  ",) if with_rtc else ())
    block = tuple(rf"{n}= \d+kHz\r?\n" for n in names)
    return (r"Hello, world!" + NL, *block, *block, r"Hello, 48MHz")


def _rows(byte) -> tuple[str, ...]:
    # printbuf of a 256-byte page whose byte i is byte(i), in spi_master_slave
    # and both spi_flash programs: sixteen "%02x" a row, a space between, a
    # newline after the last.
    return tuple(" ".join(f"{byte(16 * r + c):02x}" for c in range(16)) + NL for r in range(16))


def _wake_cycles(asleep: str, runs: int) -> tuple[str, ...]:
    lines = []
    for n in range(1, runs + 1):
        lines += [rf"Wake up, test run: {n}\r?\n", r"Awake for 10000ms" + NL]
        if n < runs:
            lines.append(asleep + NL)
    return tuple(lines)


SPECS: dict[str, Spec] = {
    # adc/dma_capture/dma_capture.c:71-107: "Arming DMA", sleep_ms(1000),
    # "Starting capture", "Capture finished", then 1000 samples as
    # printf("%-3d, ") with a newline after every tenth; main returns.
    "adc_dma_capture": Spec(
        console=(r"Arming DMA\r?\n", r"Starting capture\r?\n", r"Capture finished\r?\n",
                 r"(?:(?:\d{1,3} *, ){10}\r?\n){100}"),
        seconds=4.0,
        source="adc/dma_capture/dma_capture.c",
    ),
    # adc/hello_adc/hello_adc.c:14-29: the banner, then GP26's raw value and
    # voltage every 500 ms, forever.
    "hello_adc": Spec(
        console=(r"ADC Example, measuring GPIO26\r?\n",
                 *(rf"Raw value: 0x[0-9a-f]{{3}}, voltage: {NUM} V\r?\n",) * 3),
        seconds=3.0,
        source="adc/hello_adc/hello_adc.c",
    ),
    # adc/onboard_temperature/onboard_temperature.c:49-60: "Onboard
    # temperature = %.02f C" every 10 + 990 ms, forever.
    "onboard_temperature": Spec(
        console=(rf"Onboard temperature = -?\d+\.\d{{2}} C\r?\n",) * 3,
        seconds=5.0,
        source="adc/onboard_temperature/onboard_temperature.c",
    ),
    # adc/read_vsys/read_vsys.c:36-68: the first pass always prints the power
    # source, VSYS, an optional battery percentage and the temperature; later
    # passes print only on a change.
    "read_vsys": Spec(
        console=(r"Power (?:POWERED|BATTERY), -?\d+\.\d{2}V(?: \(-?\d+%\))?, temp -?\d+\.\d DegC\r?\n",),
        seconds=3.0,
        source="adc/read_vsys/read_vsys.c",
    ),
    # binary_info/hello_anything/hello_anything.c:18-39: by default stdio on
    # uart0 (GP0/GP1, 115200) and USB, then "Hello, world!" every second.
    # With no USB host each printf also waits out stdio_usb's 500 ms stdout
    # timeout (PICO_STDIO_USB_STDOUT_TIMEOUT_US), so a line comes about
    # every 1.5 s on the benches.
    # The same lines reach its USB console, judged where that reaches the
    # rig host (hello_anything.c:27-38).
    "hello_anything": Spec(
        console=(r"Hello, world!\r?\n",) * 3,
        usb_console=(r"Hello, world!\r?\n",) * 3,
        seconds=8.0,
        source="binary_info/hello_anything/hello_anything.c",
    ),
    # clocks/detached_clk_peri/detached_clk_peri.c:66-84: two banner lines,
    # then for divisors 1..10 the divisor and clk_sys from the frequency
    # counter; main returns.
    "clocks_detached_clk_peri": Spec(
        console=(r"Peripheral clock is attached directly to system PLL\.\r?\n",
                 r"We can vary the system clock divisor while printing from the UART:\r?\n",
                 *(rf"Setting system clock divisor to {d}\r?\nMeasuring system clock with frequency "
                   rf"counter:\d+ kHz\r?\n" for d in range(1, 11))),
        seconds=3.0,
        source="clocks/detached_clk_peri/detached_clk_peri.c",
    ),
    # clocks/hello_48MHz/hello_48MHz.c:14-72: "Hello, world!", the frequency
    # counter readings, the same after moving clk_sys and clk_peri to
    # pll_usb, then "Hello, 48MHz" with no newline. clk_rtc exists only on
    # RP2040 (:22-35).
    "hello_48MHz": Spec(
        console=_hello_48mhz(with_rtc=False),
        seconds=3.0,
        source="clocks/hello_48MHz/hello_48MHz.c",
        per_board={"pico": {"console": _hello_48mhz(with_rtc=True)}},
    ),
    # clocks/hello_resus/hello_resus.c:14-48: "Hello resus", pll_sys stopped,
    # the resus callback restarts it and prints "Resus event fired".
    "hello_resus": Spec(
        console=(r"Hello resus\r?\n", r"Resus event fired\r?\n"),
        seconds=3.0,
        source="clocks/hello_resus/hello_resus.c",
        # Upstream defect on the RP2350: the callback (:14-31) sets clk_sys to
        # the RP2040's 125 MHz while clk_peri, which the UART baud is taken
        # from, is still recorded at the pico2's 150 MHz, so on real silicon
        # its line comes out at the wrong baud. The callback ran: something
        # arrives after "Hello resus".
        per_board={"pico2": {"console": (r"Hello resus\r?\n", r"[^\r\n]+")}},
    ),
    # cmake/build_variants/other.c:10-15, main.c:11-18: the shared define is
    # 123 and this binary's 456, without DO_EXTRA (CMakeLists.txt).
    "build_variant1": Spec(
        console=(r"The common thing is 123\r?\n", r"The binary local thing is 456\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"A little extra"),
        source="cmake/build_variants/main.c",
    ),
    # cmake/build_variants: build_variant2 has 789 and DO_EXTRA.
    "build_variant2": Spec(
        console=(r"The common thing is 123\r?\n", r"The binary local thing is 789\r?\n",
                 r"A little extra\r?\n"),
        seconds=3.0,
        source="cmake/build_variants/main.c",
    ),
    # dcp/hello_dcp/hello_dcp.c:27-109: (1,2,3).(4,5,6) is 32, and the
    # extended dot product is exactly 29 - 2^-40; then twenty IIR samples and
    # four butterflies.
    "hello_dcp": Spec(
        console=(r"Hello, DCP!\r?\n", r"\(1,2,3\)\.\(4,5,6\)=32\r?\n",
                 r"\(1\+pow\(2,-20\),2,3\)\.\(1-pow\(2,-20\),5,6\)=28\.999999999999091\r?\n",
                 r"IIR filter impulse response:\r?\n", rf"(?:y\[[ \d]\d\]={NUM}\r?\n){{20}}",
                 r"Radix-2 butterfly of \(2\+3j,5\+7j\)=\([^)]*\)\r?\n",
                 r"Radix-2 DIF butterfly of \(2\+3j,5\+7j\) with twiddle factor \(1\.5\+2\.5j\)=\([^)]*\)\r?\n",
                 r"Radix-2 DIT butterfly of \(2\+3j,5\+7j\) with twiddle factor \(1\.5\+2\.5j\)=\([^)]*\)\r?\n",
                 r"Radix-4 butterfly of \(2\+3j,5\+7j,11\+17j,41\+43j\)=\([^)]*\)\r?\n"),
        seconds=3.0,
        source="dcp/hello_dcp/hello_dcp.c",
    ),
    # divider/hello_divider.c:12-74: 123456 / -321 and / 321 by the divider,
    # each checked backwards, async, by operator, and saved across 123 / 7.
    "hello_divider": Spec(
        console=(r"Hello, divider!\r?\n", r"123456/-321 = -384 remainder 192\r?\n",
                 r"Working backwards! Result 123456 should equal 123456!\r?\n",
                 r"123456/321 = 384 remainder 192\r?\n",
                 r"Working backwards! Result 123456 should equal 123456!\r?\n",
                 r"Async result 123456/-321 = -384 remainder 192\r?\n",
                 r"123456 / -321 = \(by operator -384\) \(inlined -384\)\r?\n",
                 r"inner 123 / 7 = 17\r?\n", r"outer divide 123456 / -321 = -384\r?\n"),
        seconds=3.0,
        source="divider/hello_divider.c",
    ),
    # dma/control_blocks/control_blocks.c:18-113: the control channel feeds
    # six words to the UART by DMA, then "DMA finished.".
    "dma_control_blocks": Spec(
        console=(r"DMA control block example:\r?\n", r"Transferring one word at a time\.\r?\n",
                 r"DMA finished\.\r?\n"),
        seconds=3.0,
        source="dma/control_blocks/control_blocks.c",
    ),
    # dma/hello_dma/hello_dma.c:14-49: puts() of the DMA-copied string.
    "hello_dma": Spec(
        console=(r"Hello, world! \(from DMA\)\r?\n",),
        seconds=3.0,
        source="dma/hello_dma/hello_dma.c",
    ),
    # dma/sniff_crc/sniff_crc.c:25-100: "123456789" and soft_crc32_block's
    # CRC32 from CRC32_INIT 0xffffffff with no final XOR (:31-39, :48), which
    # is ~0xcbf43926 = 0x340bc6d9, appended little-endian; sniffing all 13
    # bytes then leaves 0, "CRC32 check is good".
    "sniff_crc": Spec(
        console=(r"Buffer to DMA: 0x31 0x32 0x33 0x34 0x35 0x36 0x37 0x38 0x39 0xd9 0xc6 0x0b 0x34 \r?\n",
                 r"Completed DMA sniff of 13 byte buffer, DMA sniff accumulator value: 0x0\r?\n",
                 r"CRC32 check is good\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"CRC32 check FAILED"),
        source="dma/sniff_crc/sniff_crc.c",
    ),
    # flash/cache_perfctr/flash_cache_perfctr.c:19-90: the XIP counters, the
    # 25th Fibonacci number (fib(0) = fib(1) = 1), and the hit/miss checks,
    # each of which puts() its message and exits on failure.
    "flash_cache_perfctr": Spec(
        console=(r"At boot: \d+ hits, \d+ accesses\r?\n", rf"Hit rate so far: {NUM}%\r?\n",
                 r"Calculate 25th fibonacci number: 121393\r?\n",
                 rf"New hit rate after printf and fibonacci: {NUM}%\r?\n", r"Hit/miss check passed\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"running this example from SRAM", r"should miss", r"should hit",
                         r"Should miss after invalidation"),
        source="flash/cache_perfctr/flash_cache_perfctr.c",
    ),
    # flash/partition_info/partition_info.c:144-212: the un-partitioned
    # space's permissions and families, then one line per partition; a ROM
    # error panics.
    "partition_info": Spec(
        console=(r"un-partitioned_space: S\(r?w?\) NSBOOT\(r?w?\) NS\(r?w?\) uf2 \{ [^}]*\}\r?\n",),
        seconds=3.0,
        source="flash/partition_info/partition_info.c",
    ),
    # flash/program/flash_program.c:43-82: 256 rand() bytes, the erased
    # sector (all ff), the programmed page, then the comparison.
    "flash_program": Spec(
        console=(r"Generated random data:\r?\n", *(HEX_ROW,) * 16, r"Erasing target region\.\.\.\r?\n",
                 r"Done\. Read back target region:\r?\n", *(r"(?:ff ){15}ff\r?\n",) * 16,
                 r"Programming target region\.\.\.\r?\n", r"Done\. Read back target region:\r?\n",
                 *(HEX_ROW,) * 16, r"Programming successful!\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"Programming failed!"),
        source="flash/program/flash_program.c",
    ),
    # flash/runtime_flash_permissions/runtime_flash_permissions.c:12-62 with
    # pt.json (un-partitioned space: bootloader rw only): a secure read is
    # refused, allowed inside an added runtime partition, refused past it.
    "runtime_flash_permissions": Spec(
        console=(r"Attempt to read start of flash\r?\n", r"Not permitted, as expected\r?\n",
                 r"Adding partition with all permissions\r?\n", r"Attempt to read start of flash\r?\n",
                 r"Read successful - read first 2 words [0-9a-f]{8} [0-9a-f]{8}\r?\n",
                 r"Attempt to read later in flash\r?\n", r"Not permitted, as expected\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"ERROR:"),
        source="flash/runtime_flash_permissions/runtime_flash_permissions.c",
    ),
    # flash/ssi_dma/flash_ssi_dma.c:68-96: a 16 KiB SSI DMA read of flash,
    # its speed, and the check against the uncached XIP view.
    "flash_ssi_dma": Spec(
        console=(r"Starting DMA\r?\n", r"DMA finished\r?\n", rf"Transfer speed: {NUM} MB/s\r?\n",
                 r"Data check ok\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"Mismatch at"),
        source="flash/ssi_dma/flash_ssi_dma.c",
    ),
    # flash/xip_stream/flash_xip_stream.c:23-88: the 471 words of
    # random_test_data.h streamed by DMA and printed eight per line; the
    # last line has seven, and "Data check OK" follows its trailing space.
    "flash_xip_stream": Spec(
        console=(r"Starting stream from \S+\r?\n", r"Starting DMA\r?\n", r"DMA complete\r?\n",
                 r"(?:(?:[0-9a-f]{8} ){7}[0-9a-f]{8}\r?\n){58}(?:[0-9a-f]{8} ){7}Data check OK\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"Data mismatch", r"run this example from flash"),
        source="flash/xip_stream/flash_xip_stream.c",
    ),
    # freertos/hello_freertos/hello_freertos.c:114-204: main_task counts 0..9
    # every 3000 ticks (1 kHz, FreeRTOSConfig_examples_common.h:48), the
    # worker counts every 10 s (so 1 lands between main 3 and 4, 2 between
    # 6 and 7), then main_task prints its exit line and returns, which the
    # RP2040 port answers with panic("not supported") (FreeRTOS-Kernel
    # portable/ThirdParty/GCC/RP2040/port.c:200-205).
    "hello_freertos_one_core": _freertos(r"Starting FreeRTOS on core 0:\r?\n"),
    "hello_freertos_static_allocation": _freertos(r"Starting FreeRTOS SMP on both cores:\r?\n"),
    "hello_freertos_two_cores": _freertos(r"Starting FreeRTOS SMP on both cores:\r?\n"),
    # i2c/bus_scan/bus_scan.c:33-76: the header, eight rows of '.' or '@'
    # for addresses 0x00-0x7f, then "Done.".
    "i2c_bus_scan": Spec(
        console=_bus_scan(r"I2C Bus Scan"),
        seconds=3.0,
        source="i2c/bus_scan/bus_scan.c",
    ),
    # interp/hello_interp/hello_interp.c:11-23, 275-291: the 9 times table
    # (9..90), each section header in call order, and the twelve texture
    # samples.
    "hello_interp": Spec(
        console=(r"Interpolator example\r?\n",
                 r"9 times table:\r?\n" + "".join(rf"{9 * n}\r?\n" for n in range(1, 11)),
                 r"Masking:\r?\n", r"Masking with sign extension:\r?\n", r"Lane result crossover:\r?\n",
                 r"Simple blend 1:\r?\n", r"Simple blend 2:\r?\n", r"signed:\r?\n", r"unsigned:\r?\n",
                 r"Simple blend 3:\r?\n", r"Clamp:\r?\n", r"Linear interpolation:\r?\n",
                 r"Affine Texture mapping \(with texture wrap\):\r?\n", r"(?:0x[0-9a-f]{2}\r?\n){12}"),
        seconds=3.0,
        source="interp/hello_interp/hello_interp.c",
    ),
    # low_power/low_power_dormant/low_power_dormant_timer.c:18-77: per run
    # "Wake up, test run: N", 10 s awake, "Dormant for 10000ms", 10 s
    # dormant on the AON timer. On RP2040 that timer runs only from an
    # external clock on GP20 (:25-29, 66-68), which the harness makes
    # (parts.py).
    "low_power_dormant_timer": Spec(
        console=_wake_cycles(r"Dormant for 10000ms", 3),
        seconds=80.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_dormant/low_power_dormant_timer.c",
    ),
    # low_power/low_power_sleep/low_power_sleep_timer.c:13-47: per run "Wake
    # up, test run: N", 10 s awake, "Sleeping for 10000ms", 10 s asleep.
    "low_power_sleep_timer": Spec(
        console=_wake_cycles(r"Sleeping for 10000ms", 3),
        seconds=80.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_sleep/low_power_sleep_timer.c",
    ),
    # low_power/low_power_pstate/low_power_pstate_timer.c:21-57: per boot the
    # AON time, the persistent run count (garbage after a cold boot), 10 s
    # awake, "Low power for 10000ms", and a power-down whose 10 s wake is a
    # reboot; returning from the pstate call is the failure.
    "low_power_pstate_timer": Spec(
        console=(*(r"Current time: \d+\r?\n", r"Wake up, test run: \d+\r?\n", r"Awake for 10000ms\r?\n",
                   r"Low power for 10000ms\r?\n") * 2,
                 r"Current time: \d+\r?\n", r"Wake up, test run: \d+\r?\n"),
        seconds=90.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_pstate/low_power_pstate_timer.c",
    ),
    # multicore/hello_multicore/multicore.c:13-48: the banner, then each
    # core's success message, with no newline and in either order.
    "hello_multicore": Spec(
        console=(r"Hello, multicore!\r?\n",
                 r"(?=[\s\S]*It's all gone well on core 0!)(?=[\s\S]*Its all gone well on core 1!)"),
        seconds=3.0,
        forbidden=forbid(r"that's not right"),
        source="multicore/hello_multicore/multicore.c",
    ),
    # multicore/multicore_doorbell/multicore_doorbell.c:19-79: core 1 counts
    # 100000 doorbells, exits, rings back; core 0 prints the count.
    "multicore_doorbell": Spec(
        console=(r"core1 is exiting\r?\n", r"Final doorbell irq count \d+/100000 \(\d+%\)\r?\n"),
        seconds=3.0,
        source="multicore/multicore_doorbell/multicore_doorbell.c",
    ),
    # multicore/multicore_fifo_irqs/multicore_fifo_irqs.c:14-73: core 1 pushes
    # 123 and core 0 321, each received in the other's FIFO IRQ.
    "multicore_fifo_irqs": Spec(
        console=(r"Hello, multicore_fifo_irqs!\r?\n",
                 r"Irq handlers should have rx'd some stuff - core 0 got 123, core 1 got 321!\r?\n"),
        seconds=3.0,
        source="multicore/multicore_fifo_irqs/multicore_fifo_irqs.c",
    ),
    # multicore/multicore_runner/multicore_runner.c:54-83: core 1 runs
    # factorial(10) and fibonacci(10) sent through the FIFO.
    "multicore_runner": Spec(
        console=(r"Hello, multicore_runner!\r?\n", r"Factorial 10 is 3628800\r?\n", r"Fibonacci 10 is 55\r?\n"),
        seconds=3.0,
        source="multicore/multicore_runner/multicore_runner.c",
    ),
    # multicore/multicore_runner_queue/multicore_runner_queue.c:64-97: the
    # same through queues.
    "multicore_runner_queue": Spec(
        console=(r"Hello, multicore_runner using queues!\r?\n", r"Factorial 10 is 3628800\r?\n",
                 r"Fibonacci 10 is 55\r?\n"),
        seconds=3.0,
        source="multicore/multicore_runner_queue/multicore_runner_queue.c",
    ),
    # otp/hello_otp/hello_otp.c:23-40: a run that finds OTP rows 0x400 or
    # 0x410 programmed says so and returns, before any write. The bench's
    # own boot runs it before any case listens, on a blank OTP, and its
    # writes last, so every run a case sees stops there; what the boot run
    # wrote is judged in the rows themselves (pin_specs.REGISTER_SPECS).
    # The emulator only: hazards.py.
    "hello_otp": Spec(
        console=(r"ERROR: This example requires empty OTP rows to run - change the ecc_row and raw_row variables "
                 r"to an empty row and recompile" + NL,),
        seconds=3.0,
        forbidden=forbid(r"Initial (?:ECC|Raw) Row Read failed", r"ECC Write", r"Raw Write", r"Software Lock"),
        source="otp/hello_otp/hello_otp.c",
    ),
    # pio/addition/addition.c:21-35: ten rand() % 100 pairs added by PIO.
    "pio_addition": Spec(
        console=(r"Doing some random additions:\r?\n", *(r"\d{1,2} \+ \d{1,2} = \d{1,3}\r?\n",) * 10),
        seconds=3.0,
        source="pio/addition/addition.c",
    ),
    # pio/i2c/i2c_bus_scan.c:13-42: as i2c_bus_scan, by a PIO I2C master.
    "pio_i2c_bus_scan": Spec(
        console=_bus_scan(r"PIO I2C Bus Scan"),
        seconds=3.0,
        source="pio/i2c/i2c_bus_scan.c",
    ),
    # pio/logic_analyser/logic_analyser.c:89-159: 96 samples of the PWM on
    # GP16 (25 %) and GP17 (75 %), one row of '-' and '_' each.
    "pio_logic_analyser": Spec(
        console=(r"PIO logic analyser example\r?\n", r"Arming trigger\r?\n", r"Starting PWM example\r?\n",
                 r"Capture:\r?\n", r"16: (?=[-_]*-)(?=[-_]*_)[-_]{96}\r?\n", r"17: (?=[-_]*-)(?=[-_]*_)[-_]{96}\r?\n"),
        seconds=3.0,
        source="pio/logic_analyser/logic_analyser.c",
        # The trigger waits for GP16 high (:135). A pulled-up GP16 is high
        # before the PWM starts (:139-151), so the capture fires at once and
        # holds the idle level, which the program prints all the same.
        with_line_pullups={"console": (
            r"PIO logic analyser example\r?\n", r"Arming trigger\r?\n", r"Starting PWM example\r?\n",
            r"Capture:\r?\n", r"16: [-_]{96}\r?\n", r"17: [-_]{96}\r?\n")},
    ),
    # pio/spi/spi_loopback.c:18-90: MOSI and MISO are both GP16, so each
    # CPHA/CPOL mode reads back the 20 bytes it sent; then "Example PASSED".
    "pio_spi_loopback": Spec(
        console=(r"spi_loopback start\r?\n",
                 *(rf"CPHA = {a}, CPOL = {b}\r?\nTX:((?: [0-9a-f]{{2}}){{20}})\r?\nRX:\1\r?\nOK\r?\n"
                   for a in (0, 1) for b in (0, 1)),
                 r"Example PASSED\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"Nope", r"FAILED"),
        source="pio/spi/spi_loopback.c",
    ),
    # pio/uart_tx/uart_tx.c:13-43: a PIO UART on GP0, the console line, at
    # 115200 sends "Hello, world! (from PIO!)\r\n" every second.
    "pio_uart_tx": Spec(
        console=(r"Hello, world! \(from PIO!\)\r?\n",) * 3,
        seconds=4.0,
        source="pio/uart_tx/uart_tx.c",
    ),
    # reset/hello_reset/hello_reset.c:12-30.
    "hello_reset": Spec(
        console=(r"Hello, reset!\r?\n",),
        seconds=3.0,
        source="reset/hello_reset/hello_reset.c",
    ),
    # rtc/hello_rtc/hello_rtc.c:13-46: the RTC set to Friday 5 June 2020
    # 15:45:00, printed after '\r' every 100 ms (pico-sdk
    # src/common/pico_util/datetime.c:47-56 for the format). The carrier's
    # RP2040 counts its first second as the RTC is enabled: loaded with
    # 15:45:00 as rtc_set_datetime() does, from the debugger with the core
    # halted, it reads 15:45:01 at once and 15:45:02 a second later, so the
    # program never prints 15:45:00.
    "hello_rtc": Spec(
        console=(r"Hello RTC!\r?\n", *(rf"\rFriday 5 June 15:45:0{s} 2020" for s in range(1, 4))),
        seconds=6.0,
        source="rtc/hello_rtc/hello_rtc.c",
    ),
    # rtc/rtc_alarm/rtc_alarm.c:14-60: set to 11:20:00, alarm at 11:20:05.
    "rtc_alarm": Spec(
        console=(r"RTC Alarm!\r?\n", r"Alarm Fired At Wednesday 13 January 11:20:05 2020\r?\n"),
        seconds=12.0,
        source="rtc/rtc_alarm/rtc_alarm.c",
    ),
    # rtc/rtc_alarm_repeat/rtc_alarm_repeat.c:14-59: from 11:20:00 an alarm
    # on seconds == 0 only, so once a minute, forever.
    "rtc_alarm_repeat": Spec(
        console=(r"RTC Alarm Repeat!\r?\n",
                 *(rf"Alarm Fired At Wednesday 13 January 11:2{m}:00 2020\r?\n" for m in (1, 2, 3))),
        seconds=360.0,
        source="rtc/rtc_alarm_repeat/rtc_alarm_repeat.c",
    ),
    # sha/mbedtls_sha256/mbedtls_sha256.c:27-58: 1 MB of 'a' hashed by
    # mbedtls, its time, hard_asserts on the NIST digest and < 50 ms.
    "mbedtls_sha256": Spec(
        console=(r"Mbedtls time for sha256 of 1M bytes \d+ms\r?\n", r"Test passed\r?\n"),
        seconds=3.0,
        source="sha/mbedtls_sha256/mbedtls_sha256.c",
    ),
    # sha/sha256/hello_sha256.c:21-92: the sample text, its digest (fixed by
    # sha_expected, :47-50), the 1 MB NIST test without and with DMA.
    "hello_sha256": Spec(
        console=(r"Text: \d+ bytes\r?\n", r"Result:\r?\n",
                 r"2d 8c 2f 6d 97 8c a2 17 12 b5 f6 de 36 c9 d3 1f \r?\n",
                 r"a8 e9 6a 4f a5 d8 ff 8b 01 88 df b9 e7 c1 71 bb \r?\n",
                 r"Time for sha256 of 1M bytes without DMA \d+ms\r?\n",
                 r"Time for sha256 of 1M bytes with DMA \d+ms\r?\n", r"Success\r?\n"),
        seconds=3.0,
        source="sha/sha256/hello_sha256.c",
    ),
    # spi/spi_dma/spi_dma.c:18-106: 1024 bytes through SPI0 in its internal
    # loopback by DMA; a mismatch panics.
    "spi_dma": Spec(
        console=(r"SPI DMA example\r?\n", r"Configure TX DMA\r?\n", r"Configure RX DMA\r?\n",
                 r"Starting DMAs\.\.\.\r?\n", r"Wait for RX complete\.\.\.\r?\n",
                 r"Done\. Checking\.\.\.All good\r?\n"),
        seconds=3.0,
        source="spi/spi_dma/spi_dma.c",
    ),
    # spi/spi_master_slave/spi_master/spi_master.c:48-89: the 00..ff buffer,
    # then every 10 s one 256-byte exchange and the MISO page it read, which
    # the rig's spi_page_target answers with spi_slave.c's ~i (parts.py).
    "spi_master": Spec(
        console=(r"SPI master example\r?\n",
                 r"SPI master says: The following buffer will be written to MOSI endlessly:\r?\n",
                 *(_rows(lambda i: i)),
                 *(rf"SPI master says: read page {p} from the MISO line:\r?\n" + "".join(_rows(lambda i: 255 - i))
                   for p in range(3))),
        seconds=40.0,
        source="spi/spi_master_slave/spi_master/spi_master.c",
    ),
    # system/boot_info/boot_info.c:146-160: each rom_get_boot_info field and
    # its decoded name, then the watchdog reboot reason.
    "boot_info": Spec(
        console=(r"boot_type: \d+\r?\n\t(?:normal|bootsel|ram image|flash update|pc_sp|unknown)\r?\n",
                 r"reboot_params: 0x[0-9a-f]{8}:0x[0-9a-f]{8}\r?\n",
                 r"diagnostic_partition_index: -?\d+\r?\n\t\w+\r?\n",
                 r"recent_boot_partition: -?\d+\r?\n\t\w+\r?\n",
                 r"recent_boot_tbyb_and_update_info: \d+\r?\n\t\w+",
                 r"boot_diagnostics 0x[0-9a-f]{8}:\r?\n",
                 r"watchdog reboot [01] \(0x[0-9a-f]+\)\r?\n"),
        seconds=3.0,
        source="system/boot_info/boot_info.c",
    ),
    # system/narrow_io_write/narrow_io_write.c:19-62: WATCHDOG_SCRATCH0
    # written by 32, 8 and 16 bits; narrow writes replicate across the word.
    "narrow_io_write": Spec(
        console=(r"Should be 0xdeadbeef: 0xdeadbeef\r?\n", r"Should be ef be ad de: ef be ad de\r?\n",
                 r"Should be 0xa5a5a5a5: 0xa5a5a5a5\r?\n", r"Should be 0x3c3c3c3c: 0x3c3c3c3c\r?\n",
                 r"Should be 0xf00df00d: 0xf00df00d\r?\n"),
        seconds=3.0,
        source="system/narrow_io_write/narrow_io_write.c",
    ),
    # system/unique_board_id/unique_board_id.c:21-32: the 8 id bytes.
    "unique_board_id": Spec(
        console=(r"Unique identifier:(?: [0-9a-f]{2}){8}\r?\n",),
        seconds=3.0,
        source="system/unique_board_id/unique_board_id.c",
    ),
    # timer/hello_timer/hello_timer.c:13-57: the 2 s alarm, a 500 ms
    # repeating timer for 3 s (five or six repeats), cancelled, 2 s, the same
    # with -500 ms, cancelled, 2 s, "Done".
    "hello_timer": Spec(
        console=(r"Hello Timer!\r?\n", r"Timer -?\d+ fired!\r?\n", *(r"Repeat at \d+\r?\n",) * 5,
                 r"cancelled\.\.\. 1\r?\n", *(r"Repeat at \d+\r?\n",) * 5, r"cancelled\.\.\. 1\r?\n",
                 r"Done\r?\n"),
        seconds=25.0,
        source="timer/hello_timer/hello_timer.c",
    ),
    # timer/periodic_sampler/periodic_sampler.c:22-83: a 25 Hz timer queues
    # 100, 200, ...; the first ten are printed in order, then polled batches
    # and the leftovers, then "Done".
    "periodic_sampler": Spec(
        console=(*(rf"Got {i}: {100 * (i + 1)}\r?\n" for i in range(10)), r"  got \d+\r?\n", r"Done\r?\n"),
        seconds=6.0,
        forbidden=forbid(r"FIFO was full", r"Failed to add timer"),
        source="timer/periodic_sampler/periodic_sampler.c",
    ),
    # timer/timer_lowlevel/timer_lowlevel.c:34-73: the hardware alarm IRQ
    # every 2 s, forever.
    "timer_lowlevel": Spec(
        console=(r"Timer lowlevel!\r?\n", *(r"Alarm IRQ fired\r?\n",) * 3),
        seconds=12.0,
        source="timer/timer_lowlevel/timer_lowlevel.c",
    ),
    # uart/hello_uart/hello_uart.c:22-43: uart0 on GP0/GP1 written directly.
    "hello_uart": Spec(
        console=(r"AB Hello, UART!\r?\n",),
        seconds=3.0,
        source="uart/hello_uart/hello_uart.c",
    ),
    # universal/hello_universal/hello_universal.c:12-50: one image for every
    # chip. RP2040 prints its two lines every second forever; RP2350 ten
    # times, then reboots into the other architecture's image.
    "hello_universal": Spec(
        console=(r"Hello, world!\r?\nI'm an RP2[03][45]0",) * 3,
        seconds=5.0,
        source="universal/hello_universal/hello_universal.c",
        per_board={
            "pico": {"console": (r"Hello, world!\r?\nI'm an RP2040\r?\n",) * 3},
            "pico2": {"console": (*(r"Hello, world!\r?\nI'm an RP2350 running ARM\r?\n",) * 10,
                                  r"Rebooting to other architecture\r?\n",
                                  r"Hello, world!\r?\nI'm an RP2350 running RISC-V\r?\n"),
                      "seconds": 24.0},
        },
    ),
    # watchdog/hello_watchdog/hello_watchdog.c:11-33: a clean boot updates
    # the 100 ms watchdog five times and stops; the watchdog reboot prints
    # "Rebooted by Watchdog!" and returns.
    "hello_watchdog": Spec(
        console=(r"Clean boot\r?\n", *(rf"Updating watchdog {i}\r?\n" for i in range(5)),
                 r"Waiting to be rebooted by watchdog\r?\n", r"Rebooted by Watchdog!\r?\n"),
        seconds=3.0,
        source="watchdog/hello_watchdog/hello_watchdog.c",
    ),
    # adc/adc_console/adc_console.c:40-100: the banner and the help, then
    # one command per character from the console, each echoed: "c4"
    # selects the temperature sensor and says so, "s" samples it once,
    # "x" is unknown and brings the help back. "w" is never sent: it drives
    # GP2-GP29 as outputs against whatever the bench holds on them.
    "adc_console": Spec(
        console=(r"={27}\r?\nRP2040 ADC and Test Console\r?\n={27}\r?\n", r"Commands:\r?\n",
                 r"w\t: Wiggle pins\r?\n", r"c4\r?\nSwitched to channel 4\r?\n",
                 r"s\r?\n0x[0-9a-f]{3} -> \d\.\d{6} V\r?\n", r"x\r?\nUnrecognised command: x\r?\n",
                 r"Commands:\r?\n"),
        seconds=3.0,
        forbidden=forbid(r"Unknown input channel"),
        source="adc/adc_console/adc_console.c",
        input=((r"w\t: Wiggle pins\r?\n", b"c4"), (r"Switched to channel 4\r?\n", b"s"), (r" V\r?\n", b"x")),
    ),
    # uart/uart_advanced/uart_advanced.c:29-84: uart0 with its FIFOs off
    # greets, then the RX interrupt sends back every character it gets,
    # plus one.
    "uart_advanced": Spec(
        console=(r"Hello, uart interrupts\r?\n", r"IBM"),
        seconds=3.0,
        source="uart/uart_advanced/uart_advanced.c",
        input=((r"Hello, uart interrupts\r?\n", b"HAL"),),
    ),
    # pio/hello_pio/hello.c:42: the pin it blinks; that a key stops the
    # blink is judged by its pins (pin_specs.py).
    "hello_pio": Spec(
        console=(r"Using gpio 25\r?\n",),
        seconds=3.0,
        source="pio/hello_pio/hello.c",
    ),
    # hello_world/serial/hello_serial.c: stdio_init_all(), then
    # printf("Hello, world!\n") every second, forever.
    "hello_serial": Spec(
        console=(r"Hello, world!\r?\n",) * 3,
        seconds=4.0,
        source="hello_world/serial/hello_serial.c",
    ),
    # hello_world/usb/hello_usb.c:10-16 and CMakeLists.txt:9-11: the same
    # loop on stdio_usb alone; the UART stays silent.
    "hello_usb": Spec(
        console=(),
        usb_console=(r"Hello, world!\r?\n",) * 3,
        seconds=4.0,
        source="hello_world/usb/hello_usb.c",
    ),
    # usb/device/dev_multi_cdc/main.c:43-85: CDC 0 is stdio; once it is
    # open it says so every 5 s (:53-57), and what comes in on CDC 1 is
    # answered "OK" on CDC 1 (:81-83). Its callback also names the input on
    # CDC 0 (:66, :79), but that line is dropped whenever stdio_usb's
    # low-priority IRQ ran the tud_task that took it: the IRQ holds
    # stdio_usb_mutex (pico-sdk 2.3.0 stdio_usb.c:62-64), and the printf's
    # mutex_try_enter_block_until gives up on a mutex its own caller owns
    # (pico_sync/mutex.c:121). Which tud_task takes it is timing, on either
    # chip, so only the answer is judged. Nothing prints until a host opens
    # CDC 0.
    "dev_multi_cdc": Spec(
        console=(),
        usb_console=(r"Connected to CDC 0" + NL,),
        usb_input=((r"Connected to CDC 0" + NL, "Custom CDC", b"metalclaw"),),
        usb_replies=(("Custom CDC", r"OK\r\n"),),
        seconds=4.0,
        source="usb/device/dev_multi_cdc/main.c",
    ),
    # usb/device/dev_lowlevel/dev_lowlevel.c:565-568, 149-150, 170-178, 54-89:
    # the banner, then each of EP0 OUT, EP0 IN, EP1 OUT and EP2 IN set up
    # (%p prints its own 0x), before the pull-up presents the device.
    "dev_lowlevel": Spec(
        console=(r"USB Device Low-Level hardware example" + NL,
                 *(rf"Set up endpoint 0x{ep:x} with buffer address 0x\S+" + NL for ep in (0x00, 0x80, 0x01, 0x82))),
        seconds=3.0,
        source="usb/device/dev_lowlevel/dev_lowlevel.c",
    ),
    # usb/host/host_cdc_msc_hid/main.c:47-50: TinyUSB's board_init puts
    # stdio on the default UART and the banner follows. The rest of the
    # program answers devices plugged into the DUT's USB host port, which
    # nothing reaches on either bench or in the emulator, where the DUT's
    # USB is a device port.
    "host_cdc_msc_hid": Spec(
        console=(r"TinyUSB Host CDC MSC HID Example" + NL,),
        seconds=3.0,
        source="usb/host/host_cdc_msc_hid/main.c",
    ),
}
