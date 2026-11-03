# SPDX-License-Identifier: Apache-2.0 OR MIT

from .common import EAGAIN, NET_DOWN_TIMEOUT_S, NET_UP_TIMEOUT_S, NET_UP_WAIT_MS, PppModelCases, net

# What modem_cellular's simcom,a76xx scripts read: the IMEI (AT+CGSN) and
# the model (AT+CGMM) only; the other four stay empty.
IMEI = "862205051234567"
MODEL_NAME = "A7672E-FASE"
# The driver's startup wait after the PWRKEY press when no APP RDY comes.
STARTUP_MS = 20000


class TestA7672x(PppModelCases):
    MODEL = "a7672x"
    IMAGE = "cellular/a7672x"
    IDENTITY = {"imei": IMEI, "model": MODEL_NAME, "manufacturer": "", "fw": "", "imsi": "", "iccid": ""}
    # The init and periodic scripts ask CREG, CEREG and CGREG alike.
    REGISTERED = {"gsm": 1, "gprs": 1, "lte": 1}
    # Off until PWRKEY.
    BEFORE_UP = {"powered": False, "power_cycles": 0}
    # The shutdown script's AT+CPOF powers the part off.
    POWERED_AFTER_SUSPEND = False
    # Zephyr's PPP L2 (v4.4.0): the model's Terminate-Request ends the link
    # before the driver has read the third registration URC (it is
    # deregistered only once CREG, CGREG and CEREG all are), so LCP restarts
    # with the carrier still on; the driver's carrier-off then leaves LCP
    # in Starting and the phase at Establish, and ppp_lcp_lower_down()
    # waits CONFIG_NET_L2_PPP_TIMEOUT for a Dead phase that never comes.
    LONG_LOSS_DOWN_RC = -EAGAIN

    def test_without_app_rdy_the_driver_waits_out_its_startup(self, dut, connected):
        connected.set(ready_urc=False)
        try:
            # Only the power-off matters here. After a resume, Zephyr v4.4.0
            # sometimes sends no LCP Terminate-Request on net_if_down and
            # returns -EAGAIN after waiting for the link to terminate; the
            # driver powers the part off either way.
            down = net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
            assert down["pm"] == "suspended" and connected.get("powered") is False, down
            up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
        finally:
            connected.set(ready_urc=True)
        assert up["status"] == "ok", up
        assert up["ms"] >= STARTUP_MS, up
        assert connected.get("unknown_commands") == 0
