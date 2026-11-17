# SPDX-License-Identifier: Apache-2.0 OR MIT
# Build upstream Zephyr samples (test/zephyr_samples/builds.py) for one bench
# board and stage each as the rig runs a pico-examples program:
# OUT/images/<board>/<target>/{zephyr.elf,example.json}, with the build log
# in OUT/logs/<board>/<target>.log and one line per target in
# OUT/summary_<board>.txt. example.json carries the yaml test's harness and
# harness_config, which test/zephyr_samples/targets.py judges the image by.
#
# The sample is built as twister builds its test: the test's extra_args as
# CMake cache entries, its extra_configs as a Kconfig fragment, its
# extra_conf_files / extra_overlay_confs / extra_dtc_overlay_files as
# CONF_FILE / EXTRA_CONF_FILE / EXTRA_DTC_OVERLAY_FILE.
#
# Usage (ZEPHYR_BASE, ZEPHYR_SDK_INSTALL_DIR set; west on PATH):
#   python -m test.zephyr_samples.build --board pico --out DIR [TARGET... | all]

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from test.zephyr_samples.builds import BOARDS, BUILDS, Build


def yaml_tests(sample_dir: Path) -> dict[str, dict]:
    # The sample's tests, each with the yaml's common section folded in.
    # PyYAML comes with Zephyr's build requirements, not the rig's.
    import yaml
    for name in ("sample.yaml", "testcase.yaml"):
        path = sample_dir / name
        if path.is_file():
            doc = yaml.safe_load(path.read_text())
            # A testcase.yaml test without a harness runs on twister's ztest
            # harness.
            common = {**({"harness": "ztest"} if name == "testcase.yaml" else {}), **(doc.get("common") or {})}
            return {test: {**common, **(fields or {})} for test, fields in (doc.get("tests") or {}).items()}
    raise FileNotFoundError(f"no sample.yaml or testcase.yaml in {sample_dir}")


# Every bench board's Zephyr arch (the RP2350's Cortex-M33 build).
ARCH = "arm"


def names_board(entry: str, zephyr_board: str) -> bool:
    # Whether a platform entry names the board, alone or with qualifiers.
    return zephyr_board == entry or zephyr_board.startswith(entry + "/") or entry.startswith(zephyr_board + "/")


def runs_on(test: dict, zephyr_board: str) -> bool:
    # Whether the test's platform_allow / platform_exclude let it run on the board.
    allow, exclude = _list(test.get("platform_allow")), _list(test.get("platform_exclude"))
    return (not allow or any(names_board(e, zephyr_board) for e in allow)) and \
        not any(names_board(e, zephyr_board) for e in exclude)


def board_configs(configs: list[str], zephyr_board: str) -> list[str]:
    # extra_configs as twister applies them: an "arch:<arch>:" or
    # "platform:<board>:" line only on that arch or board.
    kept = []
    for config in configs:
        parts = config.split(":")
        if len(parts) == 3 and parts[0] == "arch":
            if parts[1] == ARCH:
                kept.append(parts[2])
        elif len(parts) == 3 and parts[0] == "platform":
            if names_board(parts[1], zephyr_board):
                kept.append(parts[2])
        else:
            kept.append(config)
    return kept


def choose_test(build: Build, tests: dict[str, dict], zephyr_board: str) -> tuple[str, dict]:
    if build.test is not None:
        return build.test, tests[build.test]
    for name, test in tests.items():
        if (not test.get("build_only") and test.get("harness", "") in ("console", "ztest", "")
                and runs_on(test, zephyr_board)):
            return name, test
    raise LookupError(f"samples/{build.sample} has no test that runs on a console on {zephyr_board}")


def _list(value) -> list[str]:
    if value is None:
        return []
    return shlex.split(value) if isinstance(value, str) else list(value)


# The list-valued cache entries several sources add to: one -D each, joined.
LISTS = ("EXTRA_CONF_FILE", "EXTRA_DTC_OVERLAY_FILE")


def cmake_args(build: Build, test: dict, work: Path, zephyr_board: str) -> list[str]:
    args, lists = [], {name: [] for name in LISTS}
    for arg in _list(test.get("extra_args")):
        name, _, value = arg.partition("=")
        if name in lists:
            lists[name] += value.split(";")
        else:
            args.append(f"-D{arg}")
    if test.get("extra_configs"):
        fragment = work / "testsuite_extra.conf"
        fragment.write_text("".join(f"{line}\n" for line in board_configs(test["extra_configs"], zephyr_board)))
        lists["EXTRA_CONF_FILE"].append(str(fragment))
    if test.get("extra_conf_files"):
        args.append("-DCONF_FILE=" + ";".join(_list(test["extra_conf_files"])))
    lists["EXTRA_CONF_FILE"] += _list(test.get("extra_overlay_confs"))
    lists["EXTRA_DTC_OVERLAY_FILE"] += _list(test.get("extra_dtc_overlay_files"))
    args += [f"-D{name}=" + ";".join(values) for name, values in lists.items() if values]
    return args + [f"-D{a}" for a in build.args]


def zephyr_commit(base: Path) -> str:
    return subprocess.check_output(["git", "-C", str(base), "rev-parse", "HEAD"], text=True).strip()


def build_one(target: str, board: str, base: Path, out: Path) -> tuple[bool, str]:
    build = BUILDS[target]
    if board not in build.boards:
        return False, f"skipped: {build.other_boards or 'not built for this board'}"
    zephyr_board, chip = BOARDS[board]
    sample_dir = base / "samples" / build.sample
    test_name, test = choose_test(build, yaml_tests(sample_dir), zephyr_board)
    work = out / "build" / board / target
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    log = out / "logs" / board / f"{target}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    command = ["west", "build", "-p", "always", "-b", zephyr_board, "-d", str(work / "build"), str(sample_dir), "--",
               *cmake_args(build, test, work, zephyr_board)]
    with log.open("w") as f:
        f.write(shlex.join(command) + "\n")
        f.flush()
        rc = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT).returncode
    elf = work / "build" / "zephyr" / "zephyr.elf"
    if rc != 0 or not elf.is_file():
        return False, f"build failed (rc {rc}); {log}"
    image = out / "images" / board / target
    image.mkdir(parents=True, exist_ok=True)
    shutil.copy2(elf, image / "zephyr.elf")
    metadata = {
        "target": target,
        "kind": "zephyr",
        "board": board,
        "zephyr_board": zephyr_board,
        "chip": chip,
        "sample": build.sample,
        "test": test_name,
        "harness": test.get("harness", ""),
        "harness_config": test.get("harness_config", {}),
        "cmake_args": command[command.index("--") + 1:],
        "zephyr": zephyr_commit(base),
        # Whether the image prints the boot banner (a sample may turn it off).
        "boot_banner": "CONFIG_BOOT_BANNER=y" in (work / "build" / "zephyr" / ".config").read_text().splitlines(),
    }
    (image / "example.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return True, f"built {test_name}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True, choices=sorted(BOARDS))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("targets", nargs="+")
    args = parser.parse_args()
    base = Path(os.environ["ZEPHYR_BASE"])
    targets = sorted(BUILDS) if args.targets == ["all"] else args.targets
    unknown = [t for t in targets if t not in BUILDS]
    if unknown:
        parser.error(f"not in test/zephyr_samples/builds.py: {unknown}")
    args.out.mkdir(parents=True, exist_ok=True)
    failed = 0
    with (args.out / f"summary_{args.board}.txt").open("a") as summary:
        for target in targets:
            start = time.monotonic()
            try:
                ok, note = build_one(target, args.board, base, args.out)
            except (LookupError, FileNotFoundError) as e:
                ok, note = False, str(e)
            failed += not ok and not note.startswith("skipped")
            line = f"{target} {'ok' if ok else 'FAIL' if not note.startswith('skipped') else 'skip'} " \
                   f"{time.monotonic() - start:.0f}s {note}"
            print(line, flush=True)
            summary.write(line + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
