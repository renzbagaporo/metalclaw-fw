# SPDX-License-Identifier: Apache-2.0 OR MIT
# The neo_m8n, pa1010d, lcd_backpack, mpu9250 and w25q16jv models built for
# the host with the firmware's warnings, behind stand-ins for the UART, I2C
# and SPI engines, each driven by a transcription of the pico-examples
# program that uses it (program_parts_check.c). Given the properties
# parts.py sets, what the program prints must satisfy part_targets.py, and
# the part's state afterwards must pass the checks parts.py gives the rig.

import re
from pathlib import Path
from types import SimpleNamespace

import types

import pytest

from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import PARTS
from test.unit.test_part_models import run, strict_options

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
PACKAGES = {"neo_m8n": "neo_m8n", "pa1010d": "pa1010d", "lcd_backpack": "lcd_backpack", "mpu9250": "mpu9250",
            "w25q": "w25q"}
TARGETS = ("gps_uart", "pa1010d_i2c", "lcd_uart", "mpu9250_spi", "spi_flash", "pio_spi_flash")


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    import subprocess

    exe = tmp_path_factory.mktemp("program_parts") / "program_parts_check"
    sources = [HERE / "program_parts_check.c", ROOT / "core" / "src" / "regmap.c", ROOT / "core" / "src" / "command.c",
               *(ROOT / "devices" / d / f"{f}.c" for d, f in PACKAGES.items())]
    includes = [arg for d in ("core/include", *(f"devices/{d}" for d in PACKAGES)) for arg in ("-I", ROOT / d)]
    build = subprocess.run(["cc", "-O1", "-ffp-contract=off", *strict_options(), *includes, *sources, "-lm",
                            "-o", exe], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return exe


def run_program(check, target):
    (part,) = PARTS[target]
    values = [f"{k}={','.join(map(str, v)) if isinstance(v, tuple) else v}" for k, v in part.properties.items()]
    out = run(check, target, part.model, *values)
    console = "".join(line + "\n" for line in out.split("\n")
                      if not line.startswith(("part.", "region.", "stats.")))
    return console, part, the_part(out)


# The part as the harness REPL shows it: properties, regions through
# read_region(name, offset, length), and the counters a program printed
# through stats(binding).
def the_part(out: str):
    props = dict(re.findall(r"^part\.(\w+) = (.*)$", out, re.M))
    regions = {k: bytes.fromhex(v) for k, v in re.findall(r"^region\.(\w+) = ([0-9a-f]*)$", out, re.M)}
    stats = {}
    for binding, name, value in re.findall(r"^stats\.(\w+)\.(\w+) = (\d+)$", out, re.M):
        stats.setdefault(binding, {})[name] = int(value)
    part = SimpleNamespace(**{k: eval(v) for k, v in props.items()})
    part.read_region = lambda name, offset, length: regions[name][offset:offset + length]
    part.stats = lambda binding: stats[binding]
    return part


@pytest.mark.parametrize("target", TARGETS)
def test_the_program_prints_what_its_spec_expects(check, target):
    console, _, _ = run_program(check, target)
    assert PART_SPECS[target].match(console) == ([], []), console


@pytest.mark.parametrize("target", TARGETS)
def test_the_part_passes_the_rigs_checks(check, target):
    _, attach, part = run_program(check, target)
    # The rig runs a check's setup on the part just after the restart; here
    # that is the part before the program's one run, whose counters were 0.
    at_restart = types.SimpleNamespace(stats=lambda binding: {k: 0 for k in part.stats(binding)})
    for c in attach.checks:
        scope = {"part": at_restart}
        if c.setup:
            exec(c.setup, {}, scope)
        scope["part"] = part
        value = eval(c.expression, {}, scope)
        assert c.test(value), (c.description, value)


def test_the_pa1010d_program_reads_no_rmc_before_its_command_lands(check):
    console, _, _ = run_program(check, "pa1010d_i2c")
    assert console.index("(no GNRMC in the packet)") < console.index("Protocol:GNRMC")


def test_behaviours_no_program_shows(check):
    assert run(check, "behaviours").strip() == "ok"


def test_every_target_here_has_its_parts_and_criteria():
    models = {"gps_uart": "neo_m8n", "pa1010d_i2c": "pa1010d", "lcd_uart": "lcd_backpack",
              "mpu9250_spi": "mpu9250", "spi_flash": "w25q16jv", "pio_spi_flash": "w25q16jv"}
    for target in TARGETS:
        assert [a.model for a in PARTS[target]] == [models[target]]
        assert PART_SPECS[target].source


# pio/spi/spi_flash.c:121-129 runs spi_cpha0 (pio/spi/spi.pio:31-32, four
# PIO cycles a bit) at a divisor of 31.25: 1 MHz at the RP2040's 125 MHz,
# 1.2 MHz at the RP2350's 150 MHz. spi/spi_flash.c:117 asks the PL022 for
# 1 MHz. The command image's memory read must be served at both.
def test_the_flash_programs_clock_under_the_memory_read_ceiling():
    source = (ROOT / "core" / "ports" / "rp2" / "hal" / "spi_target.c").read_text()
    ceiling = int(re.search(r"#define SPI_COMMAND_MEMORY_CLOCK_HZ (\d+)u", source).group(1))
    pio_hz = max(150e6, 125e6) / (31.25 * 4)
    assert pio_hz == 1.2e6
    assert max(pio_hz, 1e6) < ceiling
