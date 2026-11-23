# SPDX-License-Identifier: Apache-2.0 OR MIT
# Part models built for the host with the firmware's warnings. The ina2xx,
# mpu6050, lis3dh, mma8451q and mpl3115a2 models run behind a stand-in for
# the I2C target engine, each driven by a transcription of the pico-examples
# program that uses it (part_models_check.c); what comes out, given the
# properties parts.py sets, must satisfy part_targets.py; the mcp9808 model
# runs there too, for the registers no program shows. The arithmetic of
# the bmx280, mcp9808 and pcf8523 models, built with a signed-overflow trap
# as well, is checked against the drivers' side of the bus, and those parts'
# console criteria against what that arithmetic makes the programs print
# for the properties the rig sets.

import re
import subprocess
from pathlib import Path

import pytest

from test.unit.programs.part_targets import PART_SPECS
from test.unit.programs.parts import PARTS

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
CHECK = HERE / "part_models_check.c"
DEVICES = ("ina2xx", "mpu6050", "lis3dh", "mma8451q", "mpl3115a2")
TARGETS = ("ina219_i2c", "ina228_i2c", "ina237_i2c", "ina260_i2c", "mpu6050_i2c", "lis3dh_i2c",
           "mma8451_i2c", "mpl3115a2_i2c")
SANITIZE = ["-fsanitize=signed-integer-overflow", "-fno-sanitize-recover=all"]


def strict_options():
    return re.findall(r"^\s+(-W\S+)$", (ROOT / "core" / "cmake" / "strict.cmake").read_text(), re.M)


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    exe = tmp_path_factory.mktemp("part_models") / "part_models_check"
    devices = (*DEVICES, "mcp9808")
    sources = [CHECK, ROOT / "core" / "src" / "regmap.c", *(ROOT / "devices" / d / f"{d}.c" for d in devices),
               ROOT / "devices" / "mcp9808" / "mcp9808_temp.c"]
    includes = [arg for d in ("core/include", *(f"devices/{d}" for d in devices)) for arg in ("-I", ROOT / d)]
    build = subprocess.run(["cc", "-O1", "-ffp-contract=off", *strict_options(), *includes, *sources, "-lm",
                            "-o", exe], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return exe


def run_program(check, target):
    (part,) = PARTS[target]
    values = [f"{k}={','.join(map(str, v)) if isinstance(v, tuple) else v}" for k, v in part.properties.items()]
    got = subprocess.run([check, target, part.model, hex(part.bindings["i2c"][2]), *values],
                         capture_output=True, timeout=60)
    assert got.returncode == 0, got.stdout
    return got.stdout.decode(errors="replace")


@pytest.mark.parametrize("target", TARGETS)
def test_the_program_prints_what_its_spec_expects(check, target):
    text = run_program(check, target)
    assert PART_SPECS[target].match(text) == ([], []), text


def test_the_mpl3115a2_fifo_overflows_within_the_specs_time(check):
    text = run_program(check, "mpl3115a2_i2c")
    first = float(re.search(r"\(at ([0-9.]+) s, \d+ callbacks\)", text).group(1))
    assert 31.0 < first < PART_SPECS["mpl3115a2_i2c"].seconds - 5


def test_every_engine_checked_program_has_a_spec_and_its_own_model():
    for target in TARGETS:
        assert target in PART_SPECS and PART_SPECS[target].source
        assert [a.model for a in PARTS[target]] == [{"mma8451_i2c": "mma8451q"}.get(target, target[:-4])]


def test_registers_no_program_output_shows(check):
    got = subprocess.run([check, "behaviours"], capture_output=True, text=True, timeout=60)
    assert (got.returncode, got.stdout.strip()) == (0, "ok"), got.stdout


@pytest.fixture(scope="module")
def build(tmp_path_factory):
    out = tmp_path_factory.mktemp("part_arithmetic")
    built = {}

    def make(check, package, *sources):
        if check not in built:
            exe = out / check
            src = [ROOT / "devices" / package / s for s in sources]
            cmd = ["cc", "-O1", *strict_options(), *SANITIZE, "-I", ROOT / "core" / "include",
                   "-I", ROOT / "devices" / package, HERE / f"{check}.c", *src, "-o", exe]
            result = subprocess.run(cmd, capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            built[check] = exe
        return built[check]

    return make


def run(exe, *args):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def bmx280(build):
    return build("bmx280_check", "bmx280", "bmx280_comp.c")


def mcp9808(build):
    return build("mcp9808_check", "mcp9808", "mcp9808_temp.c")


def pcf8523(build):
    return build("pcf8523_check", "pcf8523", "pcf8523_clock.c")


def test_bmx280_inversion_gives_the_drivers_back_every_value(build):
    assert run(bmx280(build)).strip() == "ok"


def test_mcp9808_temperature_words(build):
    assert run(mcp9808(build)).strip() == "ok"


def test_pcf8523_counters_and_alarm(build):
    assert run(pcf8523(build)).strip() == "ok"


def properties(target, model):
    (part,) = [p for p in PARTS[target] if p.model == model]
    return part.properties


def reading_patterns(target, words):
    return [p for p in PART_SPECS[target].console if any(w in p for w in words)]


def test_bmp280_i2c_criteria_match_its_reading(build):
    props = properties("bmp280_i2c", "bmp280")
    out = run(bmx280(build), "bmp280", props["pressure"], props["temperature"])
    patterns = reading_patterns("bmp280_i2c", ("Pressure", "Temp"))
    assert patterns
    for pattern in patterns:
        assert re.search(pattern, out.replace("\n", "\r\n")), (pattern, out)


def test_bme280_spi_criteria_match_its_reading(build):
    props = properties("bme280_spi", "bme280")
    out = run(bmx280(build), "bme280", props["pressure"], props["temperature"], props["humidity"])
    patterns = reading_patterns("bme280_spi", ("Humidity", "Pressure", "Temp"))
    assert patterns
    for pattern in patterns:
        assert re.search(pattern, out.replace("\n", "\r\n")), (pattern, out)


def test_mcp9808_i2c_criteria_match_its_reading(build):
    out = run(mcp9808(build), properties("mcp9808_i2c", "mcp9808")["temperature"])
    patterns = reading_patterns("mcp9808_i2c", ("Ambient", "limit"))
    assert patterns
    for pattern in patterns:
        assert re.search(pattern, out.replace("\n", "\r\n")), (pattern, out)
    for pattern in PART_SPECS["mcp9808_i2c"].forbidden:
        assert not re.search(pattern, out), (pattern, out)


# Which of the spec's patterns is the reading the program prints that many
# seconds after it set the clock.
@pytest.mark.parametrize("seconds, index", [(0, 2), (1, 2), (5, 3), (59, 4), (60, 5)])
def test_pcf8523_i2c_criteria_follow_its_clock(build, seconds, index):
    out = run(pcf8523(build), seconds).replace("\n", "\r\n")
    pattern = PART_SPECS["pcf8523_i2c"].console[index]
    assert re.search(pattern, out), (pattern, out)
