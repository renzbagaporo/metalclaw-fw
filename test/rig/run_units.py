#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds and runs rig units (test/rig/select.py) on a platform: each unit's
# DUT images and upstream samples, built for each of the platform's DUT
# boards, each run as its own rig session.
#
#   test/rig/run_units.py --platform sim --out build/units UNITS_JSON
#
# UNITS_JSON is select.py's output, or a file holding it. Per run, OUT gets
# <board>/<image or sample>/{pytest.log,junit.xml}, and OUT/summary.txt one
# line. A run that fails is run once more, and one that passes then is
# listed as flaky. Exits 1 when any build failed or any run failed twice.
# Run through `just run-units`, which sets up the tool paths.

import argparse
import json
import os
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from test.zephyr_samples.builds import BOARDS  # noqa: E402

# A run that outlives this is stopped: a hung emulation would hold the job
# until the workflow's own timeout.
RUN_TIMEOUT_S = 1800


def safe(name: str) -> str:
    return name.replace("/", "_")


def run(command: list[str], log: Path, env: dict | None = None, timeout: float | None = None) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as f:
        f.write(" ".join(command) + "\n")
        f.flush()
        try:
            return subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT, env=env,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            f.write(f"\nrun_units: stopped after {timeout:.0f} s\n")
            return 124


def last_line(log: Path) -> str:
    lines = [line for line in log.read_text(errors="replace").splitlines() if line.strip()]
    return lines[-1][:160] if lines else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("units")
    args = parser.parse_args()
    text = Path(args.units).read_text() if Path(args.units).is_file() else args.units
    units = json.loads(text)
    platform = tomllib.loads((ROOT / "test/rig/platforms.toml").read_text())["platforms"][args.platform]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    python = sys.executable
    failed = []
    flaky = []

    def note(line: str) -> None:
        print(line, flush=True)
        with (out / "summary.txt").open("a") as summary:
            summary.write(line + "\n")

    # A session that fails runs once more: one that passes then is flaky,
    # listed but not failed, and its first run's logs are kept beside.
    def session(board: str, name: str, env: dict) -> None:
        logs = out / board / safe(name)
        for attempt in (1, 2):
            start = time.monotonic()
            rc = run([python, "test/rig/run.py", "--platform", args.platform, "-v", "-p", "no:cacheprovider", "-rfE",
                      f"--junitxml={logs / 'junit.xml'}"], logs / "pytest.log", {**os.environ, **env}, RUN_TIMEOUT_S)
            note(f"{board} {name} rc={rc} {time.monotonic() - start:.0f}s {last_line(logs / 'pytest.log')}"
                 + (" (again)" if attempt == 2 else ""))
            if rc == 0:
                if attempt == 2:
                    flaky.append(f"{board} {name}")
                return
            if attempt == 1:
                for kept in ("pytest.log", "junit.xml"):
                    if (logs / kept).exists():
                        (logs / kept).rename(logs / f"first_{kept}")
        failed.append(f"{board} {name}")

    for board in platform["boards"]:
        zephyr_board = BOARDS[board][0]
        images = [i for u in units for i in u["images"]]
        samples = [s for u in units for s in u["samples"]]
        builds = out / board / "build"
        for image in images:
            env = {**os.environ, "METALCLAW_BUILD_DIR": str(builds), "METALCLAW_DUT_BOARD": zephyr_board}
            log = out / board / safe(image) / "build.log"
            if run(["just", "build-dut", "" if image == "smoke" else image], log, env) != 0:
                failed.append(f"{board} {image} build")
                note(f"{board} {image} build failed: {log}")
                continue
            session(board, image, {"METALCLAW_DUT_IMAGE": str(builds / "dut" / image / "zephyr/zephyr.elf"),
                                   "METALCLAW_DUT_DEVICE": image,
                                   "METALCLAW_SIM_WORK": str(out / board / safe(image) / "work")})
        if not samples:
            continue
        staged = out / board / "samples"
        log = out / board / "samples_build.log"
        run(["just", "build-zephyr-samples", board, str(staged), *samples], log)
        for target in samples:
            elf = staged / "images" / board / target / "zephyr.elf"
            if not elf.is_file():
                if (staged / "images" / board / target).exists() or f"{target} skip" not in log.read_text():
                    failed.append(f"{board} {target} build")
                    note(f"{board} {target} build failed: {staged / 'logs' / board / (target + '.log')}")
                else:
                    note(f"{board} {target} not built for this board")
                continue
            session(board, target, {"METALCLAW_DUT_IMAGE": str(elf),
                                    "METALCLAW_SIM_WORK": str(out / board / safe(target) / "work")})
    note(f"flaky: {len(flaky)}" + (f" ({', '.join(flaky)})" if flaky else ""))
    note(f"failed: {len(failed)}" + (f" ({', '.join(failed)})" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
