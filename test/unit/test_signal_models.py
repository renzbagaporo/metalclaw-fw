# SPDX-License-Identifier: Apache-2.0 OR MIT
# square_wave, jumper, jumper_open_drain, spi_page_target and
# spi_page_controller built for the host with the firmware's warnings and
# run behind stand-ins for the gpio and SPI peer HAL (signal_models_check.c).

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
DEVICES = {"square_wave": "square_wave.c", "jumper": "jumper.c", "spi_page": "spi_page.c"}


def strict_options():
    return re.findall(r"^\s+(-W\S+)$", (ROOT / "core" / "cmake" / "strict.cmake").read_text(), re.M)


def test_the_models_ask_the_hal_for_what_the_programs_need(tmp_path):
    exe = tmp_path / "signal_models_check"
    sources = [HERE / "signal_models_check.c", *(ROOT / "devices" / d / f for d, f in DEVICES.items())]
    includes = [arg for d in ("core/include", *(f"devices/{d}" for d in DEVICES)) for arg in ("-I", ROOT / d)]
    build = subprocess.run(["cc", "-O1", *strict_options(), "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            *includes, *sources, "-o", exe], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    got = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert (got.returncode, got.stdout.strip()) == (0, "ok"), got.stdout + got.stderr
