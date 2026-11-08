# SPDX-License-Identifier: Apache-2.0 OR MIT

CPUID = 0xE000ED00
# CPUID part numbers of the cores a DUT may have: Cortex-M0+ (RP2040) and
# Cortex-M33 (RP2350).
CORTEX_M_PARTS = {0xC60, 0xD21}


def is_cortex_m(cpuid):
    return cpuid >> 24 == 0x41 and (cpuid >> 4) & 0xFFF in CORTEX_M_PARTS


def test_probe_is_attached(harness, probe):
    out = harness.run("import metalclaw\nprint([i.name for i in metalclaw.instances()])")
    assert "dut" in out
    assert "CMSIS-DAP v2" in harness.usb_interfaces()


def test_probe_reads_the_dut(dut):
    words = dut.probe.read_words(CPUID, 1)
    assert is_cortex_m(words[0]), hex(words[0])


def test_the_dap_client_reads_the_dut(dut):
    words = dut.memory.read_words(CPUID, 1)
    assert is_cortex_m(words[0]), hex(words[0])


def test_probe_counts_transfers(harness, dut):
    before = int(harness.run("import metalclaw\nprint(metalclaw.instances()[0].transfers)"))
    dut.probe.read_words(CPUID, 1)
    after = int(harness.run("import metalclaw\nprint(metalclaw.instances()[0].transfers)"))
    assert after > before


# A download is the slowest thing an emulated bench does, so the smoke
# image's run carries it for every image.
def test_the_probe_flashes_and_verifies_the_image(dut):
    dut.require_image("smoke")
    dut.flash()
    assert dut.ping()["status"] == "ok"
