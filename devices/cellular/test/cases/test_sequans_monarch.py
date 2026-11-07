# SPDX-License-Identifier: Apache-2.0 OR MIT


from .common import ECHO_RING, NET_UP_TIMEOUT_S, NET_UP_WAIT_MS, PppModelCases, echo, net

# What modem_cellular's sqn,gm02s init script reads: AT+CGSN, +CGMM, +CGMI
# and +CGMR; IMSI and ICCID are never asked for.
IMEI = "354658090123453"
MODEL_NAME = "GM02S"
MANUFACTURER = "SEQUANS Communications"
REVISION = "UE8.2.0.5-60964"


class TestSequansMonarch(PppModelCases):
    MODEL = "sequans_monarch"
    IMAGE = "cellular/sequans_monarch"
    IDENTITY = {"imei": IMEI, "model": MODEL_NAME, "manufacturer": MANUFACTURER, "fw": REVISION, "imsi": "",
                "iccid": ""}
    # The scripts ask CEREG only; CREG and CGREG stay as the driver zeroed them.
    REGISTERED = {"gsm": 0, "gprs": 0, "lte": 1}
    # Powered from attach, booted once by the time the case looks.
    BEFORE_UP = {"powered": True, "power_cycles": 1}
    # No shutdown script and no power line: the driver leaves the part on.
    POWERED_AFTER_SUSPEND = True
    # The +CEREG: 0 URC makes the driver dormant before the model's
    # Terminate-Request: LCP closes from both ends and stays down.
    LONG_LOSS_DOWN_RC = 0

    def test_a_dut_reset_restarts_the_part_through_reset_n(self, dut, connected):
        # The DUT's reset holds RESET_N low on the RP2350's reset pull-down
        # until the image's hog: the part in CMUX with PPP up resets with
        # it. The fresh driver then pulses RESET_N itself.
        cycles = connected.get("power_cycles")
        dut.reset()
        dut.ping()
        dut.harness.sleep(2.0)
        assert connected.get("power_cycles") == cycles + 1
        assert connected.get("cmux") is False and connected.get("ppp_up") is False
        state = net(dut, "STATE")
        assert state["initialized"] is False and state["l4"] is False, state
        up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
        assert up["status"] == "ok" and up["pm_op"] == "init", up
        assert connected.get("power_cycles") == cycles + 2
        assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"
        assert connected.get("unknown_commands") == 0
