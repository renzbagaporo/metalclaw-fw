# SPDX-License-Identifier: Apache-2.0 OR MIT
# Zephyr sample criteria from a build's example.json, and how a sample.yaml
# test becomes CMake arguments.

from test.zephyr_samples.build import cmake_args, choose_test
from test.zephyr_samples.builds import BUILDS, Build
from test.zephyr_samples.targets import OWN, zephyr_spec

BOOT = "*** Booting Zephyr OS build v4.4.0 ***\r\n"


def metadata(harness="console", config=None, target="zephyr/x"):
    return {"target": target, "kind": "zephyr", "sample": "x", "test": "sample.x", "harness": harness,
            "harness_config": config or {}}


def test_a_pico_examples_program_has_no_zephyr_spec():
    assert zephyr_spec({"target": "blink", "kind": "bare_board"}) is None
    assert zephyr_spec(None) is None


def test_one_line_takes_the_first_regex_after_the_banner():
    spec = zephyr_spec(metadata(config={"type": "one_line", "regex": ["Hello World! (.*)", "never"]}))
    assert spec.match(BOOT + "Hello World! rpi_pico/rp2040\r\n")[0] == []
    assert spec.match("Hello World! rpi_pico/rp2040\r\n")[0] != []


def test_multi_line_is_ordered_unless_the_yaml_says_otherwise():
    config = {"type": "multi_line", "regex": ["a", "b"]}
    assert zephyr_spec(metadata(config=config)).match(BOOT + "b\r\na\r\n")[0] == ["b"]
    config["ordered"] = False
    assert zephyr_spec(metadata(config=config)).match(BOOT + "b\r\na\r\n")[0] == []


def test_a_second_banner_or_a_fatal_error_fails():
    spec = zephyr_spec(metadata(config={"type": "one_line", "regex": ["ok"]}))
    assert spec.match(BOOT + "ok\r\n" + BOOT)[1]
    assert spec.match(BOOT + "ok\r\n>>> ZEPHYR FATAL ERROR 0: CPU exception\r\n")[1]
    assert spec.match(BOOT + "ok\r\n")[1] == []


def test_ztest_takes_the_project_summary():
    spec = zephyr_spec(metadata(harness="ztest"))
    assert spec.match(BOOT + "PROJECT EXECUTION SUCCESSFUL\r\n") == ([], [])
    assert spec.match(BOOT + "PROJECT EXECUTION FAILED\r\n")[1]


def test_another_harness_without_criteria_of_ours_is_not_judged():
    assert zephyr_spec(metadata(harness="pytest")) is None


def test_own_criteria_replace_the_yaml():
    for target in OWN:
        assert zephyr_spec(metadata(harness="led", target=target)) is OWN[target]


def test_a_test_becomes_twisters_cmake_arguments(tmp_path):
    test = {"extra_args": "DEBUG_PRINTF=1 EXTRA_CONF_FILE=prj_x.conf", "extra_configs": ["CONFIG_A=y"],
            "extra_dtc_overlay_files": ["a.overlay"]}
    args = cmake_args(Build("x", args=("CONFIG_B=n",)), test, tmp_path, "rpi_pico")
    assert args[0] == "-DDEBUG_PRINTF=1"
    assert f"-DEXTRA_CONF_FILE=prj_x.conf;{tmp_path / 'testsuite_extra.conf'}" in args
    assert sum(a.startswith("-DEXTRA_CONF_FILE=") for a in args) == 1
    assert (tmp_path / "testsuite_extra.conf").read_text() == "CONFIG_A=y\n"
    assert "-DEXTRA_DTC_OVERLAY_FILE=a.overlay" in args
    assert args[-1] == "-DCONFIG_B=n"


def test_the_first_console_test_is_chosen():
    tests = {"a": {"build_only": True, "harness": "console"}, "b": {"harness": "pytest"}, "c": {"harness": "console"}}
    assert choose_test(Build("x"), tests, "rpi_pico")[0] == "c"
    assert choose_test(Build("x", test="b"), tests, "rpi_pico")[0] == "b"


def test_a_test_for_other_platforms_is_passed_over():
    tests = {"nrf": {"harness": "console", "platform_allow": ["nrf52840dk/nrf52840"]},
             "not_pico": {"harness": "console", "platform_exclude": "rpi_pico"},
             "pico2": {"harness": "console", "platform_allow": ["rpi_pico2/rp2350a/m33"]},
             "any": {"harness": "console"}}
    assert choose_test(Build("x"), tests, "rpi_pico")[0] == "any"
    assert choose_test(Build("x"), tests, "rpi_pico2/rp2350a/m33")[0] == "not_pico"


def test_a_sample_upstream_runs_only_elsewhere_still_builds_unless_excluded():
    tests = {"nrf": {"harness": "console", "platform_allow": ["nrf52840dk/nrf52840"]},
             "pico_excluded": {"harness": "console", "platform_allow": ["qemu_x86"], "platform_exclude": ["rpi_pico"]}}
    assert choose_test(Build("x"), tests, "rpi_pico")[0] == "nrf"
    assert choose_test(Build("x"), {"pico_excluded": tests["pico_excluded"]}, "rpi_pico2/rp2350a/m33")[0] == \
        "pico_excluded"


def test_every_build_is_named_for_its_sample():
    for target, build in BUILDS.items():
        assert target == f"zephyr/{build.sample}" + (f"@{build.variant}" if build.variant else ""), target


def test_arch_and_platform_configs_apply_only_there(tmp_path):
    test = {"extra_configs": ["CONFIG_A=y", "arch:arm:CONFIG_ARM_MPU=n", "arch:arc:CONFIG_ARC_MPU_ENABLE=n",
                              "platform:rpi_pico:CONFIG_P=y", "platform:frdm_k64f:CONFIG_K=y"]}
    cmake_args(Build("x"), test, tmp_path, "rpi_pico")
    assert (tmp_path / "testsuite_extra.conf").read_text() == "CONFIG_A=y\nCONFIG_ARM_MPU=n\nCONFIG_P=y\n"


def test_an_image_without_the_banner_is_not_asked_for_it():
    m = {**metadata(config={"type": "one_line", "regex": [".*success"]}), "boot_banner": False}
    assert zephyr_spec(m).match("<inf> hashmap_sample: success\r\n")[0] == []


def test_a_shell_test_types_each_command_at_a_prompt_and_reads_its_reply():
    m = {**metadata(harness="shell"), "shell_commands": [
        {"command": "kernel cycles", "expected": "cycles: .* hw cycles"},
        {"command": "kernel version", "expected": "Zephyr version .*"},
        {"command": "kernel sleep 100"}]}
    spec = zephyr_spec(m)
    assert [data for _, data in spec.input] == [b"\n", b"kernel cycles\n", b"kernel version\n", b"kernel sleep 100\n"]
    text = (BOOT + "\x1b[1;32muart:~$ \x1b[mkernel cycles\r\ncycles: 123 hw cycles\r\n"
            "\x1b[1;32muart:~$ \x1b[mkernel version\r\nZephyr version 4.4.0\r\n")
    assert spec.match(text)[0] == []
    assert spec.match(text.replace("Zephyr version", "Version"))[0] == ["^(?:Zephyr version .*)"]


def test_our_overlays_and_confs_follow_the_tests_own(tmp_path):
    from test.zephyr_samples.build import ROOT
    test = {"extra_dtc_overlay_files": ["a.overlay"], "extra_args": "platform:other_board:SHIELD=x"}
    args = cmake_args(Build("x", overlays=("o.overlay",), confs=("c.conf",)), test, tmp_path, "rpi_pico")
    assert f"-DEXTRA_DTC_OVERLAY_FILE=a.overlay;{ROOT / 'o.overlay'}" in args
    assert f"-DEXTRA_CONF_FILE={ROOT / 'c.conf'}" in args
    assert not any("SHIELD" in a for a in args)


def test_a_zephyr_samples_parts_come_from_its_own_table():
    from test.pico_examples.parts import parts_for
    assert [p.model for p in parts_for("zephyr/sensor/pressure_polling", "pico")] == ["bmp388"]
    assert parts_for("zephyr/hello_world", "pico") == ()


def test_every_overlay_and_conf_a_build_names_exists():
    from test.zephyr_samples.build import ROOT
    for target, build in BUILDS.items():
        for path in (*build.overlays, *build.confs):
            assert (ROOT / path).is_file(), (target, path)


def test_the_banner_may_follow_what_came_before_the_reset():
    spec = zephyr_spec(metadata(config={"type": "one_line", "regex": ["^Enter a line$"]}))
    assert spec.match(" \x08 \x08*** Booting Zephyr OS build v4.4.0 ***\r\nEnter a line\r\n")[0] == []


def test_button_interrupt_reads_its_set_up_line():
    spec = OWN["zephyr/drivers/gpio/button_interrupt"]
    text = BOOT + "Set up button at gpio-port@0 pin 15\r\nButton pressed at 1\r\nButton pressed at 2\r\n"
    assert spec.match(text) == ([], [])


def test_the_radio_finds_its_pins_on_both_benches():
    from test.zephyr_samples.parts import ZEPHYR_PARTS
    from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE
    radio = ZEPHYR_PARTS["zephyr/drivers/lora/send"][0]
    for fixture in (FIXTURE, METALCLAW_PICO_RP2_FIXTURE):
        assert "metalclaw.attach('sx1262'" in radio.code(fixture)
    carrier = radio._pins(METALCLAW_PICO_RP2_FIXTURE)
    linked = [p for b in ("spi", "busy", "dio1") for p in carrier[b]]
    assert all(16 <= p < 48 for p in linked), carrier
    assert carrier["busy"] == (22,)


def test_every_zephyr_part_finds_its_pins_on_both_benches():
    from test.zephyr_samples.parts import ZEPHYR_PARTS
    from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE
    for target, parts in ZEPHYR_PARTS.items():
        for part in parts:
            for fixture in (FIXTURE, METALCLAW_PICO_RP2_FIXTURE):
                part.code(fixture)


def test_the_erased_storage_block_is_valid_intel_hex():
    from test.zephyr_samples.build import ihex_records
    lines = ihex_records(0x101FFFF0, b"\xff" * 32)
    assert lines[0] == ":02000004101FCB"
    assert lines[1].startswith(":10FFF000") and lines[2] == ":020000041020CA"
    for line in lines:
        assert sum(bytes.fromhex(line[1:])) & 0xFF == 0


def test_no_zephyr_pattern_asks_for_a_literal_backslash():
    from test.zephyr_samples.targets import OVERRIDES
    patterns = [p for spec in OWN.values() for p in spec.console]
    patterns += [p for fields in OVERRIDES.values() for p in fields.get("console", ())]
    assert not [p for p in patterns if "\\\\" in p], "a doubled backslash matches a literal backslash"
