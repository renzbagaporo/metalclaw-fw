# SPDX-License-Identifier: Apache-2.0 OR MIT

import socket
import struct
import subprocess
from pathlib import Path

import pytest

from test.pico_examples.emulation import EMULATION, Emulation, Unwired
from test.rig.platforms.emulator import bench, platform


def bench_files(root, board="rpi_pi_pico"):
    layout = bench.LAYOUTS[board]
    for name in ("renode", layout.script, "roms/b2.elf", "roms/bootrom-combined.bin"):
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).touch()
    for name in ("harness.uf2", "zephyr.elf"):
        (root / name).touch()
    return bench.Bench(root, layout, root / "harness.uf2", root / "roms", root / "zephyr.elf", root / "work",
                       "0123456789ABCDEF")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def commands(quantum=None, dut_machine="pico2", settings=None):
    if settings is None and quantum:
        settings = bench.Settings((("quantum", f'"{quantum}"'),))
    return bench.monitor_commands(Path("/renode/bench.resc"), Path("/w/rom.bin"), Path("/w/flash.bin"),
                                  Path("/roms/dut.bin"), Path("/w/dut_flash.bin"), dut_machine, settings)


def test_a_bench_refuses_to_start_beside_another_renode(tmp_path, monkeypatch):
    b = bench_files(tmp_path)
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        server.listen()
        monkeypatch.setattr(bench, "USBIP_PORT", server.getsockname()[1])
        with pytest.raises(bench.BenchError, match="already in use; another Renode is running"):
            b.start()
    assert b.process is None


def test_a_bench_refuses_to_start_without_the_dut_image(tmp_path):
    b = bench_files(tmp_path)
    b.dut_image.unlink()
    with pytest.raises(bench.BenchError, match="missing .*zephyr.elf"):
        b.start()
    assert b.process is None


def test_a_bench_refuses_a_dut_image_it_cannot_load_into_flash(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    b = bench_files(tmp_path)
    b.dut_image.write_bytes(b"not an elf")
    with pytest.raises(bench.BenchError, match="zephyr.elf: the DUT image is not a little-endian 32-bit ELF"):
        b.start()
    assert b.process is None


def test_the_bench_script_gets_its_files_before_it_is_included():
    run = commands()
    include = run.index("include @/renode/bench.resc")
    assert set(run[:include]) == {
        "$harness_rom=@/w/rom.bin",
        "$harness_flash=@/w/flash.bin",
        "$dut_rom=@/roms/dut.bin",
        f"$usbip_port={bench.USBIP_PORT}",
    }


def test_the_dut_image_is_loaded_into_the_dut_flash_before_the_bench_starts():
    run = commands()
    assert run[run.index("include @/renode/bench.resc") + 1:] == [
        'mach set "pico2"',
        "xipMemory LoadBinary @/w/dut_flash.bin",
        "start",
    ]


def test_a_quantum_is_set_before_the_bench_script_is_included():
    run = commands("0.001")
    assert run.index('$quantum="0.001"') < run.index("include @/renode/bench.resc")
    assert not any(command.startswith("$quantum") for command in commands())


def test_the_dut_flash_is_loaded_into_the_layouts_dut_machine():
    assert commands(dut_machine="pico")[-3] == 'mach set "pico"'


def test_each_harness_board_has_a_bench_with_its_chips():
    assert bench.LAYOUTS["rpi_pi_pico"] == bench.Layout(
        "scripts/multi-node/metalclaw_fixture.resc", "rp2040", "rp2350", "pico2", "0123456789ABCDEF")
    carrier = bench.LAYOUTS["metalclaw_pico_rp2"]
    assert (carrier.script, carrier.harness_chip, carrier.dut_chip, carrier.dut_machine) == (
        "scripts/multi-node/metalclaw_carrier.resc", "rp2350", "rp2040", "pico")
    assert carrier.serial == "00000000C0FFEE01"


@pytest.mark.parametrize("board", sorted(bench.LAYOUTS))
def test_a_bench_refuses_to_start_without_its_script(tmp_path, board):
    b = bench_files(tmp_path, board)
    (tmp_path / b.layout.script).unlink()
    with pytest.raises(bench.BenchError, match=f"missing .*{b.layout.script}"):
        b.start()
    assert b.process is None


@pytest.mark.parametrize("board, rom", [("rpi_pi_pico", "bootrom-combined.bin"), ("metalclaw_pico_rp2", "b2.elf")])
def test_a_bench_refuses_to_start_without_its_duts_boot_rom(tmp_path, board, rom):
    b = bench_files(tmp_path, board)
    (tmp_path / "roms" / rom).unlink()
    with pytest.raises(bench.BenchError, match=f"missing .*roms/{rom}"):
        b.start()


def test_a_bench_refuses_a_harness_uf2_for_another_chip(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    monkeypatch.setattr(bench.dut_flash, "flash_contents", lambda elf: b"\xff")
    b = bench_files(tmp_path, "metalclaw_pico_rp2")
    b.harness_uf2.write_bytes(uf2_block(0x10000000, 0xE48BFF56))
    with pytest.raises(bench.BenchError, match="harness.uf2: the UF2 has no rp2350 flash blocks"):
        b.start()
    assert b.process is None


def uf2_block(address, family, flags=0x2000, payload=b"\x00" * 256):
    header = struct.pack("<8I", 0x0A324655, 0x9E5D5157, flags, address, len(payload), 0, 1, family)
    return header + payload + bytes(512 - 32 - len(payload) - 4) + struct.pack("<I", 0x0AB16F30)


class Launched(Exception):
    pass


# Starts a bench up to launching Renode and returns Renode's monitor commands.
def launch(tmp_path, monkeypatch, board, prepare=None):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    monkeypatch.setattr(bench.dut_flash, "flash_contents", lambda elf: b"dut")
    monkeypatch.setattr(bench.storage, "with_storage", lambda image: image + b"fat")
    extracted = []

    def extract(argv, **kwargs):
        extracted.append(Path(argv[2]).name)
        Path(argv[3]).write_bytes(b"mask rom")

    def popen(argv, **kwargs):
        raise Launched([argv[i + 1] for i, arg in enumerate(argv) if arg == "-e"])

    monkeypatch.setattr(bench.subprocess, "run", extract)
    monkeypatch.setattr(bench.subprocess, "Popen", popen)
    b = bench_files(tmp_path, board)
    if prepare is not None:
        prepare(b)
    chip = b.layout.harness_chip
    b.harness_uf2.write_bytes(uf2_block(0x10000000, {"rp2040": 0xE48BFF56, "rp2350": 0xE48BFF59}[chip]))
    with pytest.raises(Launched) as launched:
        b.start()
    # A start that fails stops the bench, which closes its log.
    assert b.log is None
    assert (b.work / "harness_flash.bin").read_bytes() == bytes(256) + b"fat"
    assert (b.work / "dut_flash.bin").read_bytes() == b"dut"
    return launched.value.args[0], extracted


def test_the_fixture_1_bench_gives_the_rp2040_mask_rom_to_the_harness(tmp_path, monkeypatch):
    run, extracted = launch(tmp_path, monkeypatch, "rpi_pi_pico")
    assert extracted == ["b2.elf"]
    assert f"$harness_rom=@{tmp_path}/work/harness_rom.bin" in run
    assert f"$dut_rom=@{tmp_path}/roms/bootrom-combined.bin" in run
    assert f"include @{tmp_path}/scripts/multi-node/metalclaw_fixture.resc" in run
    assert 'mach set "pico2"' in run


def test_the_carrier_bench_gives_the_rp2040_mask_rom_to_the_dut(tmp_path, monkeypatch):
    run, extracted = launch(tmp_path, monkeypatch, "metalclaw_pico_rp2")
    assert extracted == ["b2.elf"]
    assert f"$harness_rom=@{tmp_path}/roms/bootrom-combined.bin" in run
    assert f"$dut_rom=@{tmp_path}/work/dut_rom.bin" in run
    assert f"include @{tmp_path}/scripts/multi-node/metalclaw_carrier.resc" in run
    assert 'mach set "pico"' in run


def test_stopping_records_that_renode_exited_before_the_session_ended(tmp_path):
    b = bench_files(tmp_path)
    b.log = (tmp_path / "renode.log").open("w")
    b.process = subprocess.Popen(["sh", "-c", "exit 3"])
    b.process.wait()
    b.stop()
    assert (tmp_path / "renode.log").read_text() == "rig: Renode exited with 3 before the end of the session\n"


def test_stopping_records_that_renode_outlived_the_session(tmp_path, monkeypatch):
    # No monitor answers, so stop() kills the process.
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    b = bench_files(tmp_path)
    b.log = (tmp_path / "renode.log").open("w")
    b.process = subprocess.Popen(["sleep", "30"])
    b.stop()
    assert (tmp_path / "renode.log").read_text() == ("rig: Renode still running at the end of the session\n"
                                                     "rig: Renode's monitor did not take quit; sending SIGTERM\n")
    assert b.process.returncode is not None


def test_stopping_twice_records_once(tmp_path):
    b = bench_files(tmp_path)
    b.log = (tmp_path / "renode.log").open("w")
    b.process = subprocess.Popen(["sh", "-c", "exit 0"])
    b.process.wait()
    b.stop()
    b.stop()
    assert (tmp_path / "renode.log").read_text() == "rig: Renode exited with 0 before the end of the session\n"


def test_an_instance_defaults_to_0(monkeypatch):
    monkeypatch.delenv(bench.INSTANCE_ENV, raising=False)
    assert bench.instance() == 0


@pytest.mark.parametrize("value", ["-1", "8", "two"])
def test_an_instance_out_of_range_is_refused(monkeypatch, value):
    monkeypatch.setenv(bench.INSTANCE_ENV, value)
    with pytest.raises(bench.BenchError, match=f"invalid {bench.INSTANCE_ENV}"):
        bench.instance()


def test_each_carrier_instance_reports_its_own_serial():
    carrier = bench.LAYOUTS["metalclaw_pico_rp2"]
    assert [bench.harness_serial(carrier, n) for n in range(3)] == [
        "00000000C0FFEE01", "00000000C0FFEE02", "00000000C0FFEE03"]


def test_a_bench_script_with_a_fixed_serial_runs_only_instance_0():
    fixture = bench.LAYOUTS["rpi_pi_pico"]
    assert bench.harness_serial(fixture, 0) == "0123456789ABCDEF"
    with pytest.raises(bench.BenchError, match="only METALCLAW_RENODE_INSTANCE=0"):
        bench.harness_serial(fixture, 1)


def test_the_bench_script_gets_the_instances_usbip_port_and_chip_id():
    run = bench.monitor_commands(Path("/renode/bench.resc"), Path("/w/rom.bin"), Path("/w/flash.bin"),
                                 Path("/roms/dut.bin"), Path("/w/dut_flash.bin"), "pico", None, 3242,
                                 ("harness_chip_id", "00000000C0FFEE03"))
    include = run.index("include @/renode/bench.resc")
    assert "$usbip_port=3242" in run[:include]
    assert "$harness_chip_id=0x00000000C0FFEE03" in run[:include]


def test_an_instance_refuses_to_start_when_its_own_port_is_taken(tmp_path, monkeypatch):
    b = bench_files(tmp_path, "metalclaw_pico_rp2")
    b.instance = 2
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        server.listen()
        monkeypatch.setattr(bench, "USBIP_PORT", server.getsockname()[1] - 2)
        monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
        with pytest.raises(bench.BenchError, match=f"port {server.getsockname()[1]} is already in use"):
            b.start()
    assert b.process is None


# ---- per-session variables and unwired pins ---------------------------------

SCRIPT = '''$usbip_port?=3240
$quantum?="0.0001"
$wire_latency_us?=300
$spi_input_delay_ns?=600000
$i2c0_wiring?=@scripts/multi-node/metalclaw_carrier_i2c0_bus.resc
mach create "carrier"
'''
# A Renode package without the I2C0 wiring scripts.
OLD_SCRIPT = "\n".join(line for line in SCRIPT.splitlines() if "i2c0" not in line)


def carrier_script(tmp_path, text=SCRIPT, wirings=("bus", "wires")):
    script = tmp_path / "metalclaw_carrier.resc"
    script.write_text(text)
    for wiring in wirings:
        (tmp_path / f"metalclaw_carrier_i2c0_{wiring}.resc").touch()
    return script


def settings(emulation, script, quantum=None):
    return bench.bench_settings(emulation, script, script.read_text(), "pico", quantum)


def test_a_script_declares_its_variables_with_defaults():
    assert bench.script_variables(SCRIPT) == {
        "usbip_port": "3240", "quantum": '"0.0001"', "wire_latency_us": "300", "spi_input_delay_ns": "600000",
        "i2c0_wiring": "@scripts/multi-node/metalclaw_carrier_i2c0_bus.resc"}


def test_no_needs_set_nothing(tmp_path):
    assert settings(Emulation(), carrier_script(tmp_path)) == bench.Settings()


def test_a_relayed_jumper_sets_the_quantum_the_latency_and_the_spi_round_trip(tmp_path):
    s = settings(Emulation(quantum="0.000001", wire_latency_us=3), carrier_script(tmp_path))
    assert s.variables == (("quantum", '"0.000001"'), ("wire_latency_us", "3"), ("spi_input_delay_ns", "6000"))
    assert s.unmet == () and s.commands == ()


def test_the_wire_script_is_chosen_beside_the_bench_script(tmp_path):
    script = carrier_script(tmp_path)
    s = settings(Emulation(i2c0_wiring="wires"), script)
    assert s.variables == (("i2c0_wiring", f"@{tmp_path}/metalclaw_carrier_i2c0_wires.resc"),)
    assert settings(Emulation(i2c0_wiring="bus"), script).variables == (
        ("i2c0_wiring", f"@{tmp_path}/metalclaw_carrier_i2c0_bus.resc"),)


def test_a_package_without_the_i2c0_wiring_leaves_it_unmet(tmp_path):
    script = carrier_script(tmp_path, OLD_SCRIPT, wirings=())
    s = settings(Emulation(i2c0_wiring="wires"), script)
    assert s.variables == ()
    assert s.unmet == ("$i2c0_wiring wires (metalclaw_carrier_i2c0_wires.resc)",)
    # Its own I2C0 is the byte-level bus.
    assert settings(Emulation(i2c0_wiring="bus"), script) == bench.Settings()


def test_a_declared_wiring_whose_script_is_missing_is_unmet(tmp_path):
    s = settings(Emulation(i2c0_wiring="wires"), carrier_script(tmp_path, wirings=("bus",)))
    assert s.unmet == ("$i2c0_wiring wires (metalclaw_carrier_i2c0_wires.resc)",)


def test_a_script_without_the_variables_leaves_them_unmet(tmp_path):
    script = carrier_script(tmp_path, 'mach create "carrier"\n', wirings=())
    s = settings(Emulation(quantum="0.000001", wire_latency_us=3), script)
    assert s.variables == ()
    assert s.unmet == ("$quantum 0.000001", "$wire_latency_us 3")


@pytest.mark.parametrize("emulation, quantum", [
    (Emulation(quantum="0.000001", wire_latency_us=2), None),
    (Emulation(wire_latency_us=200), None),
    (Emulation(quantum="0.0002"), None),
    (Emulation(quantum="0.000001", wire_latency_us=3), "0.00001"),
])
def test_a_latency_of_two_quanta_or_less_is_refused(tmp_path, emulation, quantum):
    with pytest.raises(bench.BenchError, match="does not exceed two quanta"):
        settings(emulation, carrier_script(tmp_path), quantum)


@pytest.mark.parametrize("latency", [0, -3, 2.5, True])
def test_a_latency_in_anything_but_whole_microseconds_is_refused(tmp_path, latency):
    with pytest.raises(bench.BenchError, match="whole microseconds"):
        settings(Emulation(wire_latency_us=latency), carrier_script(tmp_path))


def test_the_environment_quantum_overrides_the_programs(tmp_path):
    s = settings(Emulation(quantum="0.000001", wire_latency_us=3), carrier_script(tmp_path), "0.0000012")
    assert dict(s.variables)["quantum"] == '"0.0000012"'


def test_variables_are_set_before_the_bench_script_is_included(tmp_path):
    s = settings(Emulation(quantum="0.000001", wire_latency_us=3, i2c0_wiring="wires"), carrier_script(tmp_path))
    run = commands(dut_machine="pico", settings=s)
    include = run.index("include @/renode/bench.resc")
    for name, value in s.variables:
        assert run.index(f"${name}={value}") < include


def test_a_starting_bench_sets_its_programs_needs_and_notes_them(tmp_path, monkeypatch):
    def emulated(b):
        b.emulation = Emulation(i2c0_wiring="wires", unwired=Unwired((0, 1)))
    launch(tmp_path, monkeypatch, "metalclaw_pico_rp2", emulated)
    log = (tmp_path / "work" / "renode.log").read_text()
    assert "rig: DUT pins unwired GP0 GP1\n" in log
    # The empty test script declares nothing.
    assert ("rig: scripts/multi-node/metalclaw_carrier.resc does not take $i2c0_wiring wires "
            "(metalclaw_carrier_i2c0_wires.resc)\n") in log


FIXTURE_SCRIPT = """mach create "pico1"
mach create "pico2"
emulation CreateDelayedDigitalWire "uart0_dut_tx" false $wire_latency_us
mach set "pico2"
uart0_dut_tx Connect gpio 0
uart0_harness_tx Connect gpio 1
gp18 Connect gpio 18
mach set "pico1"
uart0_dut_tx Connect gpio 1
uart0_harness_tx Connect gpio 0
"""


def test_the_wires_on_each_dut_pin_come_from_the_bench_script():
    assert bench.dut_pin_wires(FIXTURE_SCRIPT, "pico2") == {0: ("uart0_dut_tx",), 1: ("uart0_harness_tx",),
                                                            18: ("gp18",)}
    assert bench.dut_pin_wires(FIXTURE_SCRIPT, "pico1") == {1: ("uart0_dut_tx",), 0: ("uart0_harness_tx",)}


def test_unwired_pins_dispose_of_the_duts_wires_on_them_after_the_bench_script(tmp_path):
    script = tmp_path / "metalclaw_fixture.resc"
    script.write_text(FIXTURE_SCRIPT)
    s = bench.bench_settings(EMULATION["hello_pwm"], script, FIXTURE_SCRIPT, "pico2")
    assert s.commands == ("uart0_dut_tx Dispose", "uart0_harness_tx Dispose") and s.unmet == ()
    run = commands(dut_machine="pico2", settings=s)
    include = run.index("include @/renode/bench.resc")
    assert run[include + 1:] == [*s.commands, 'mach set "pico2"', "xipMemory LoadBinary @/w/dut_flash.bin", "start"]


def test_a_program_without_unwired_pins_disposes_of_nothing(tmp_path):
    script = tmp_path / "metalclaw_fixture.resc"
    script.write_text(FIXTURE_SCRIPT)
    assert bench.bench_settings(EMULATION["gps_uart"], script, FIXTURE_SCRIPT, "pico2").commands == ()


def test_a_starting_bench_without_unwired_pins_notes_none(tmp_path, monkeypatch):
    def emulated(b):
        b.emulation = Emulation(i2c0_wiring="wires")
    launch(tmp_path, monkeypatch, "metalclaw_pico_rp2", emulated)
    assert "unwired" not in (tmp_path / "work" / "renode.log").read_text()


ONE_MACHINE_SCRIPT = """mach create "bench"
machine LoadPlatformDescription @platforms/boards/metalclaw_fixture_one_machine.repl
emulation CreateDigitalWire "uart0_dut_tx" false
mach set "bench"
uart0_dut_tx Connect d_gpio 0
uart0_dut_tx Connect h_gpio 1
emulation CreateDigitalWire "gp7" true
gp7 Connect h_gpio 7
gp7 Connect d_gpio 7
"""


def test_a_one_machine_bench_finds_the_duts_wires_by_its_prefixed_gpio():
    assert bench.dut_pin_wires(ONE_MACHINE_SCRIPT, "bench", "d_") == {0: ("uart0_dut_tx",), 7: ("gp7",)}
    # Without the prefix the harness's and the DUT's banks would be confused.
    assert bench.dut_pin_wires(ONE_MACHINE_SCRIPT, "bench") == {}


def test_a_one_machine_bench_loads_the_duts_flash_into_its_prefixed_xip(tmp_path):
    layout = bench.ONE_MACHINE_LAYOUTS["rpi_pi_pico"]
    assert (layout.script, layout.dut_machine, layout.dut_prefix) == (
        "scripts/multi-node/metalclaw_fixture_one_machine.resc", "bench", "d_")
    commands = bench.monitor_commands(tmp_path / "s.resc", tmp_path / "h.bin", tmp_path / "hf.bin",
                                      tmp_path / "d.bin", tmp_path / "df.bin", layout.dut_machine,
                                      dut_prefix=layout.dut_prefix)
    assert commands[-3:] == ['mach set "bench"', f"d_xipMemory LoadBinary @{tmp_path / 'df.bin'}", "start"]


def test_the_layout_is_chosen_by_metalclaw_emulator_layout(monkeypatch):
    monkeypatch.setenv("METALCLAW_HARNESS_BOARD", "rpi_pi_pico")
    monkeypatch.delenv("METALCLAW_EMULATOR_LAYOUT", raising=False)
    assert platform.layout() is bench.LAYOUTS["rpi_pi_pico"]
    monkeypatch.setenv("METALCLAW_EMULATOR_LAYOUT", "one_machine")
    assert platform.layout() is bench.ONE_MACHINE_LAYOUTS["rpi_pi_pico"]
    monkeypatch.setenv("METALCLAW_HARNESS_BOARD", "metalclaw_pico_rp2")
    assert platform.layout() is bench.ONE_MACHINE_LAYOUTS["metalclaw_pico_rp2"]
    monkeypatch.setenv("METALCLAW_HARNESS_BOARD", "nope")
    with pytest.raises(pytest.exit.Exception, match="in one machine"):
        platform.layout()
    monkeypatch.setenv("METALCLAW_EMULATOR_LAYOUT", "bogus")
    with pytest.raises(pytest.exit.Exception, match="invalid METALCLAW_EMULATOR_LAYOUT"):
        platform.layout()


def test_a_one_machine_bench_drops_the_settings_that_tune_delayed_wires(tmp_path):
    from test.pico_examples.emulation import RELAYED_ON_GP4_GP5
    script = tmp_path / "metalclaw_carrier_one_machine.resc"
    script.write_text('$quantum?="0.00001"\n$usbip_port?=3240\n$i2c0_wiring?=@x\n')
    wires = tmp_path / "metalclaw_carrier_one_machine_i2c0_wires.resc"
    wires.write_text("")
    s = bench.bench_settings(RELAYED_ON_GP4_GP5, script, script.read_text(), "bench", dut_prefix="d_")
    # The quantum and wire latency are left out; the I2C0 pin wires the
    # program asks for still come from the one-machine wires script.
    assert s.variables == (("i2c0_wiring", f"@{wires}"),) and s.unmet == ()
    # A quantum the session asks for still applies.
    s = bench.bench_settings(RELAYED_ON_GP4_GP5, script, script.read_text(), "bench", "0.00001", dut_prefix="d_")
    assert s.variables == (("quantum", '"0.00001"'), ("i2c0_wiring", f"@{wires}"))
