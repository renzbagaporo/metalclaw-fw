# SPDX-License-Identifier: Apache-2.0 OR MIT


def test_probe_is_attached(harness, probe):
    out = harness.run("import metalclaw\nprint([i.name for i in metalclaw.instances()])")
    assert "dut" in out
    assert "CMSIS-DAP v2" in harness.usb_interfaces()


def test_probe_reads_the_dut(dut):
    words = dut.probe.read_words(0xE000ED00, 1)
    assert (words[0] >> 4) & 0xFFF == 0xD21, hex(words[0])


def test_probe_counts_transfers(harness, dut):
    before = int(harness.run("import metalclaw\nprint(metalclaw.instances()[0].transfers)"))
    dut.probe.read_words(0xE000ED00, 1)
    after = int(harness.run("import metalclaw\nprint(metalclaw.instances()[0].transfers)"))
    assert after > before
