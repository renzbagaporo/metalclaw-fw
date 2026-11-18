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

from test.zephyr_samples.builds import BOARDS, BUILDS, STORAGE, Build


def yaml_tests(sample_dir: Path) -> dict[str, dict]:
    if not any((sample_dir / n).is_file() for n in ("sample.yaml", "testcase.yaml")):
        return {}
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
    # extra_configs or extra_args as twister applies them: an "arch:<arch>:"
    # or "platform:<board>:" entry only on that arch or board.
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
    if build.test == "":
        # A sample without a yaml: built as it is, judged by our criteria.
        return "", {}
    # The named test, else the first console, ztest or shell test that is
    # not build_only and whose platform filters let it run on the board;
    # failing that, the first such test whose platform_exclude does not name
    # the board: a platform_allow lists the boards upstream runs the sample
    # on, which leaves out boards it would run on as well.
    if build.test is not None:
        return build.test, tests[build.test]
    runnable = [(name, test) for name, test in tests.items()
                if not test.get("build_only") and test.get("harness", "") in ("console", "ztest", "shell", "")]
    for name, test in runnable:
        if runs_on(test, zephyr_board):
            return name, test
    for name, test in runnable:
        if runs_on({**test, "platform_allow": None}, zephyr_board):
            return name, test
    raise LookupError(f"samples/{build.sample} has no test that runs on a console on {zephyr_board}")


def _list(value) -> list[str]:
    if value is None:
        return []
    return shlex.split(value) if isinstance(value, str) else list(value)


ROOT = Path(__file__).resolve().parents[2]

# The list-valued cache entries several sources add to: one -D each, joined.
LISTS = ("EXTRA_CONF_FILE", "EXTRA_DTC_OVERLAY_FILE")


def board_name(zephyr_board: str) -> str:
    return next(b for b, (z, _) in BOARDS.items() if z == zephyr_board)


def cmake_args(build: Build, test: dict, work: Path, zephyr_board: str) -> list[str]:
    args, lists = [], {name: [] for name in LISTS}
    # Without double quotes, as twister passes them (runner.py
    # cmake_assemble_args): a list entry may quote its value.
    for arg in board_configs([a.replace('"', "") for a in _list(test.get("extra_args"))], zephyr_board):
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
    if build.storage:
        lists["EXTRA_DTC_OVERLAY_FILE"].append(
            str(ROOT / f"test/zephyr_samples/overlays/storage_{board_name(zephyr_board)}.overlay"))
    lists["EXTRA_DTC_OVERLAY_FILE"] += [str(ROOT / o) for o in build.overlays]
    lists["EXTRA_CONF_FILE"] += [str(ROOT / c) for c in build.confs]
    args += [f"-D{name}=" + ";".join(values) for name, values in lists.items() if values]
    return args + [f"-D{a}" for a in board_configs(list(build.args), zephyr_board)]


def shell_commands(test: dict, sample_dir: Path) -> list[dict]:
    # A shell-harness test's commands, as twister's Shell harness takes them:
    # harness_config.shell_commands, else the sample's shell_commands_file
    # (test_shell.yml by default).
    config = test.get("harness_config") or {}
    if config.get("shell_commands"):
        return list(config["shell_commands"])
    path = sample_dir / config.get("shell_commands_file", "test_shell.yml")
    if not path.is_file():
        return []
    import yaml
    return list(yaml.safe_load(path.read_text()) or [])


def ihex_records(address: int, data: bytes) -> list[str]:
    # Intel HEX data records for `data` at `address`, with an extended linear
    # address record at each 64 KiB boundary.
    def record(kind: int, offset: int, payload: bytes) -> str:
        body = bytes((len(payload), offset >> 8, offset & 0xFF, kind)) + payload
        return ":" + body.hex().upper() + f"{-sum(body) & 0xFF:02X}"
    lines, upper = [], None
    for at in range(0, len(data), 16):
        here = address + at
        if here >> 16 != upper:
            upper = here >> 16
            lines.append(record(4, 0, upper.to_bytes(2, "big")))
        lines.append(record(0, here & 0xFFFF, data[at:at + 16]))
    return lines


def write_flash_image(elf: Path, hex_path: Path, board: str, objcopy: str) -> None:
    # The image as Intel HEX with an erased (0xff) block over the storage
    # partition after it, so a download of it erases what an earlier run
    # left there; the rig flashes it in place of the ELF (flash_image).
    subprocess.run([objcopy, "-O", "ihex", str(elf), str(hex_path)], check=True)
    lines = [line for line in hex_path.read_text().splitlines() if line and not line.startswith(":00000001")]
    address, size = STORAGE[board]
    lines += ihex_records(address, b"\xff" * size) + [":00000001FF"]
    hex_path.write_text("\n".join(lines) + "\n")


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
    if build.storage:
        objcopy = Path(os.environ["ZEPHYR_SDK_INSTALL_DIR"]) / "gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy"
        write_flash_image(image / "zephyr.elf", image / "zephyr.hex", board, str(objcopy))
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
        "shell_commands": shell_commands(test, sample_dir) if test.get("harness") == "shell" else [],
        "cmake_args": command[command.index("--") + 1:],
        "zephyr": zephyr_commit(base),
        # Whether the image prints the boot banner (a sample may turn it off).
        # Whether upstream's platform filters list the board for this test.
        "upstream_board": runs_on(test, zephyr_board),
        # What the rig downloads in place of zephyr.elf, beside it.
        **({"flash_image": "zephyr.hex"} if build.storage else {}),
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
