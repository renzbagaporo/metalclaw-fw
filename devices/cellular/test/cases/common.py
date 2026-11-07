# SPDX-License-Identifier: Apache-2.0 OR MIT
# What every model's cases share: attaching the model, its properties and
# regions over the REPL, the fake network's test data and the DUT's socket
# commands, SMS-DELIVER PDUs for the inbox, raw AT through the DUT's UART,
# and the cases of the models Zephyr's modem_cellular drives over CMUX and
# PPP.

import binascii
import time
import zlib

import pytest

NAME = "modem"

# The regions of the shared parts (devices/cellular/net.h and sms.h).
HOSTS_SIZE = 192
REQUEST_SIZE = 96
SMS_INBOX_SIZE = 384

HOSTS = {"echo.test": "10.0.0.7", "web.test": "10.0.0.80", "ftp.test": "10.0.0.21"}

# Printable, so no chunk of it a modem hands the DUT starts with CR or LF,
# which line-oriented drivers mistake for the end of an answer.
FILE = bytes(0x20 + (i * 37 + 11) % 95 for i in range(1024))
FILE_PATH, FILE_NAME = "/pub/", "test_1k.bin"

ECHO_PORT, HTTP_PORT, CLOSED_PORT = 7, 80, 81
# A socket's receive ring: an echo larger than this overflows unless the
# DUT reads it back between sends.
ECHO_RING = 256

# The DUT's libc (picolibc, newlib numbering), not the host's.
EAGAIN, EALREADY, ENOTCONN = 11, 120, 128
EBUSY, EINVAL, ENODATA, ENOSYS, ECONNREFUSED = 16, 22, 61, 88, 111
# zephyr/net/dns_resolve.h
DNS_EAI_FAIL = -4


def pattern(length):
    # The DUT's `len=N` payload (test/rig/dut/src/main.c net_payload).
    return bytes(0x21 + (i * 7) % 94 for i in range(length))


def attach(fixture, model, pin="pwrkey"):
    # `pin` is the model's second binding, its PWRKEY or its RESET_N, and
    # the fixture's gpio key for the line the DUT drives it with.
    uart = fixture["uart"]
    pins = (uart["tx"], uart["rx"])
    line = fixture["gpio"][pin]
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach({model!r}, {NAME!r}, uart={pins}, {pin}={line})
metalclaw.resume()
"""


def pin_binding(harness, model):
    # The name of the model's power or reset line, from its schema.
    names = eval(harness.run(f"import metalclaw\nprint(repr(list(metalclaw.schema({model!r})['bindings'])))\n").strip())
    pins = [n for n in names if n != "uart"]
    assert len(pins) == 1, names
    return pins[0]


SELECT = f"""import metalclaw
modem = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Modem:
    def __init__(self, harness):
        self.harness = harness

    def set(self, **values):
        args = ", ".join(f"{k}={v!r}" for k, v in values.items())
        self.harness.run(SELECT + f"modem.set({args})\n")

    def get(self, name):
        return eval(self.harness.run(SELECT + f"print(repr(modem.get({name!r})))\n").strip())

    def write_region(self, name, data, size=None):
        if size is not None:
            assert len(data) <= size
            data = data + b"\0" * (size - len(data))
        for offset in range(0, len(data), 256):
            chunk = binascii.hexlify(data[offset:offset + 256]).decode()
            self.harness.run(SELECT + "import binascii\n"
                             f"modem.write_region({name!r}, {offset}, binascii.unhexlify({chunk!r}))\n")

    def read_region(self, name, size):
        return eval(self.harness.run(SELECT + f"print(repr(modem.read_region({name!r}, 0, {size})))\n").strip())

    def text_region(self, name, size):
        return self.read_region(name, size).rstrip(b"\0")

    def has(self, name):
        # Whether the attached model has the property: a case skips what a
        # model lacks rather than guessing.
        if not hasattr(self, "_properties"):
            self._properties = set(eval(self.harness.run(
                SELECT + "print(repr(list(modem.schema()['properties'])))\n").strip()))
        return name in self._properties

    def wait_for(self, name, value, timeout):
        deadline = self.harness.clock() + timeout
        while True:
            current = self.get(name)
            if current == value or self.harness.clock() >= deadline:
                return current
            time.sleep(0.2)

    def load_network(self):
        # The hosts table and the file the HTTP and FTP servers serve.
        self.write_region("hosts", "".join(f"{k}={v}\n" for k, v in HOSTS.items()).encode(), HOSTS_SIZE)
        self.write_region("file", FILE)
        self.set(file_length=len(FILE))


def echo(dut, protocol, host, payload, count, chunk, timeout=60.0):
    result = dut.command(f"{protocol} {host} {ECHO_PORT} {payload} count={count} chunk={chunk}", timeout)
    assert result["test"] == f"test_{protocol.lower()}", result
    return result


GSM7 = ("@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?"
        "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà")


def semi_octets(digits):
    digits = digits + ("F" if len(digits) % 2 else "")
    return "".join(digits[i + 1] + digits[i] for i in range(0, len(digits), 2))


def pack_septets(text):
    bits, count, out = 0, 0, bytearray()
    for c in text:
        bits |= GSM7.index(c) << count
        count += 7
        while count >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            count -= 8
    if count:
        out.append(bits & 0xFF)
    return bytes(out)


def deliver_pdu(sender, text, stamp, smsc=None):
    # 3GPP 23.040 SMS-DELIVER, 7-bit default alphabet, international sender.
    if smsc is None:
        head = "00"
    else:
        number = semi_octets(smsc.lstrip("+"))
        head = "%02X91%s" % (len(number) // 2 + 1, number)
    number = sender.lstrip("+")
    address = "%02X91%s" % (len(number), semi_octets(number))
    scts = "".join(semi_octets("%02d" % v) for v in stamp)
    body = pack_septets(text)
    return (head + "04" + address + "00" + "00" + scts + "%02X" % len(text) + body.hex().upper()).encode()


# Raw AT through the DUT's `UART <alias>` command (test/rig/dut/README.md).
UART_ALIAS = "modem-uart"
DUT_COMMAND_MAX = 100


def uart_escape(data):
    # The DUT's text escapes: \r, \n, \\ and \xHH. Its command trim would
    # eat a trailing space, so that one is escaped too.
    out = []
    for b in data:
        if b == 0x0D:
            out.append("\\r")
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append(f"\\x{b:02x}")
    if out and out[-1] == " ":
        out[-1] = "\\x20"
    return out


class Uart:
    def __init__(self, dut, alias=UART_ALIAS):
        self.dut = dut
        self.alias = alias

    def command(self, operation, timeout=15.0):
        line = f"UART {self.alias} {operation}"
        assert len(line) <= DUT_COMMAND_MAX, line
        result = self.dut.command(line, timeout)
        assert result["test"] == "test_uart" and result["status"] == "ok", result
        return result

    def _read(self, operation, again, ms):
        # A read that ran out of result line stops with `more`; the rest is
        # still in the DUT's ring and the next read continues there.
        text = ""
        while True:
            result = self.command(operation, ms / 1000 + 5.0)
            text += result["data"]
            if result["found"] or not result["more"]:
                return text, result["found"]
            operation = again

    def at(self, line, ms=10000):
        # One command line and everything up to its final result.
        text, found = self._read(f"AT {''.join(uart_escape(line.encode()))}", f"WAIT {ms}", ms)
        assert found, (line, text)
        return text

    def wait(self, ms):
        text, found = self._read(f"WAIT {ms}", f"WAIT {ms}", ms)
        assert found, text
        return text

    def until(self, ms, end):
        operation = f"UNTIL {ms} {''.join(uart_escape(end.encode()))}"
        text, found = self._read(operation, operation, ms)
        assert found and text.endswith(end), (end, text)
        return text

    def line(self, ms, prefix):
        # Up to and including the first whole line starting with `prefix`;
        # returns that line without its CR LF.
        operation = f"LINE {ms} {''.join(uart_escape(prefix.encode()))}"
        text, found = self._read(operation, operation, ms)
        assert found, (prefix, text)
        return text.rstrip("\r\n").split("\n")[-1].rstrip("\r")

    def read(self, ms):
        text, _ = self._read(f"READ {ms}", f"READ {ms}", ms)
        return text

    def tx(self, data):
        # As many TX commands as the escaped bytes need.
        units = uart_escape(data)
        room = DUT_COMMAND_MAX - len(f"UART {self.alias} TX ")
        while units:
            take, size = 0, 0
            while take < len(units) and size + len(units[take]) <= room:
                size += len(units[take])
                take += 1
            if take < len(units) and units[take - 1] == " ":
                units[take - 1] = "\\x20"
                continue
            self.command(f"TX {''.join(units[:take])}")
            units = units[take:]

    def send(self, payload):
        return self.command(f"SEND {payload}")

    def recv(self, n, ms=5000, crc=0):
        result = self.command(f"RECV {n} {ms} crc={crc}", ms / 1000 + 5.0)
        assert result["found"], result
        return result

    def flush(self):
        return self.command("FLUSH")


def info_lines(text):
    # The non-empty lines of an answer, without the final OK.
    return [line for line in (part.strip("\r") for part in text.split("\n")) if line and line != "OK"]


def answer(text, prefix):
    for line in info_lines(text):
        if line.startswith(prefix):
            return line[len(prefix):]
    raise AssertionError(f"no {prefix!r} in {text!r}")


def gpio(dut, operation, timeout=15.0):
    result = dut.command(f"GPIO {operation}", timeout)
    assert result["test"] == "test_gpio" and result["status"] == "ok", result
    return result


# The models Zephyr's modem_cellular drives (drivers/modem/modem_cellular.c):
# the DUT's NET command powers the part through its pin and brings PPP up
# over CMUX; the socket commands then run on Zephyr's own IP stack.
NET_UP_WAIT_MS = 90000
NET_UP_TIMEOUT_S = 100.0
# A suspend blocks until the driver closed CMUX and powered off, 30 s at most.
NET_DOWN_TIMEOUT_S = 40.0
# What modem_cellular takes to give up on a lost network: three dial
# scripts 2 s apart, a GM02S's each holding a fixed 10 s after AT+CFUN=1.
DIAL_GIVE_UP_S = 60.0
# The periodic script runs every 2 s (CONFIG_MODEM_CELLULAR_PERIODIC_SCRIPT_MS).
PERIODIC_S = 2.5
DEFAULT_RSSI = 20


def net(dut, operation, timeout=15.0):
    result = dut.command(f"NET {operation}", timeout)
    assert result["test"] == "test_net", result
    return result


def cellular(dut, operation, timeout=15.0):
    result = dut.command(f"CELLULAR {operation}", timeout)
    assert result["test"] == "test_cellular", result
    return result


def ipv4(values):
    return ".".join(str(v) for v in values)


def ensure_connected(dut, modem):
    # Back to L4 connected from wherever a case left the driver. Down then
    # up works from every state: the driver may have powered the part off
    # by itself, which the PM framework never hears of.
    state = net(dut, "STATE")
    if state["l4"] and modem.get("ppp_up"):
        return state
    net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
    up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
    assert up["status"] == "ok" and up["l4"], up
    return up


def restart_driver(dut, modem):
    # Zephyr v4.4.0's PPP L2 stays wedged after the driver gave up on a lost
    # network: LCP no longer sends Terminate-Request on net_if_down, which
    # then waits CONFIG_NET_L2_PPP_TIMEOUT and returns -EAGAIN, until the DUT
    # resets. Power the part off (the wedge only costs the wait), since a
    # fresh driver's PWRKEY press would turn a powered part off; then reset
    # the DUT and bring the interface up as the class fixture does, so the
    # cases after start from a fresh driver.
    net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
    dut.reset()
    dut.ping()
    # A part on RESET_N takes the DUT's reset for its own; let it boot.
    dut.harness.sleep(2.0)
    up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
    assert up["status"] == "ok" and up["l4"] and modem.get("ppp_up"), up


def rsrp_dbm(rssi):
    # A model's AT+CESQ puts RSRP 10 dB below the rssi's -113 + 2n dBm
    # and codes it as 27.007 tables it, index n for [-141 + n, -140 + n)
    # dBm; modem_cellular reads -140 + n, the top of that range.
    return -113 + 2 * rssi - 10 + 1


class PppModelCases:
    # A subclass names the model and what the driver reads from it:
    # IDENTITY, the six cellular_get_modem_info fields; REGISTERED, the
    # statuses the driver caches per technology while registered;
    # BEFORE_UP, the model's power state between attach and the driver's
    # first resume; POWERED_AFTER_SUSPEND, whether the driver's power-off
    # leaves the part powered; LONG_LOSS_DOWN_RC, what net_if_down returns
    # once the driver gave up on a lost network by itself.
    MODEL = None
    IMAGE = None
    IDENTITY = {}
    REGISTERED = {}
    BEFORE_UP = {}
    POWERED_AFTER_SUSPEND = None
    LONG_LOSS_DOWN_RC = None

    @pytest.fixture(scope="class")
    @classmethod
    def modem(cls, harness, dut, fixture):
        dut.require_image(cls.IMAGE)
        # The DUT first: its reset leaves the power line on the RP2350's
        # reset pull-down until the image's hog, which a part on RESET_N
        # takes for a reset.
        dut.reset()
        dut.ping()
        pin = pin_binding(harness, cls.MODEL)
        harness.run(attach(fixture, cls.MODEL, pin))
        m = Modem(harness)
        m.load_network()
        if m.has("ready_urc"):
            # APP RDY ends the driver's 20 s startup wait early.
            m.set(ready_urc=True)
        # A part on RESET_N boots from attach; let that boot finish.
        harness.sleep(2.0)
        m.before_up = {k: m.get(k) for k in ("powered", "power_cycles")}
        m.up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
        m.after_up = {k: m.get(k) for k in ("powered", "power_cycles", "pdp", "cmux", "ppp_up", "unknown_commands")}
        yield m

    @pytest.fixture
    def connected(self, dut, modem):
        ensure_connected(dut, modem)
        return modem

    def test_net_up_powers_the_part_through_its_pin(self, dut, modem):
        assert modem.before_up == self.BEFORE_UP, modem.before_up
        up = modem.up
        assert up["status"] == "ok", up
        assert up["pm_op"] == "init" and up["pm_rc"] == 0 and up["up_rc"] == 0, up
        assert up["l4"] is True and up["pm"] == "active" and up["init_res"] == 0
        assert modem.after_up == {"powered": True, "power_cycles": self.BEFORE_UP["power_cycles"] + 1,
                                  "pdp": True, "cmux": True, "ppp_up": True, "unknown_commands": 0}

    def test_ipcp_hands_over_the_address_and_the_dns_server(self, dut, connected):
        state = net(dut, "STATE")
        assert state["up"] and state["carrier"] and not state["dormant"], state
        assert state["ip"] == ipv4(connected.get("ip"))
        assert state["dns"] == ipv4(connected.get("dns"))
        assert state["dns_add"] >= 1

    def test_modem_info_is_what_the_init_script_read(self, dut, connected):
        info = cellular(dut, "INFO")
        assert info["rc"] == 0, info
        assert {k: info[k] for k in self.IDENTITY} == self.IDENTITY

    def test_registration_status_per_technology(self, dut, connected):
        reg = cellular(dut, "REG")
        assert reg["rc"] == 0, reg
        assert {k: reg[k] for k in self.REGISTERED} == self.REGISTERED
        # NR 5G maps to none of CREG, CGREG and CEREG.
        assert cellular(dut, "REG 11")["rc"] == -ENODATA

    @pytest.mark.parametrize("rssi", [0, 17, 31])
    def test_rssi_follows_the_property(self, dut, connected, rssi):
        connected.set(rssi=rssi)
        try:
            result = cellular(dut, "SIGNAL rssi")
        finally:
            connected.set(rssi=DEFAULT_RSSI)
        assert result["rc"] == 0 and result["value"] == -113 + 2 * rssi, result

    def test_an_unknown_rssi_has_no_value(self, dut, connected):
        connected.set(rssi=99)
        try:
            result = cellular(dut, "SIGNAL rssi")
        finally:
            connected.set(rssi=DEFAULT_RSSI)
        assert result["rc"] == -EINVAL, result

    def test_rsrp_and_rsrq(self, dut, connected):
        rsrp = cellular(dut, "SIGNAL rsrp")
        assert rsrp["rc"] == 0 and rsrp["value"] == rsrp_dbm(DEFAULT_RSSI), rsrp
        rsrq = cellular(dut, "SIGNAL rsrq")
        assert rsrq["rc"] == 0 and rsrq["value"] == -10, rsrq

    def test_the_driver_reports_its_events(self, dut, connected):
        before = cellular(dut, "EVENTS")
        dut.harness.sleep(2 * PERIODIC_S)
        events = cellular(dut, "EVENTS")
        assert events["subscribed"] is True
        assert events["registration"] >= 1 and events["last_registration"] == 1, events
        # The counters run from the DUT's boot, through whatever earlier
        # cases did to the link; on a steady link every periodic script on
        # DLCI 2 is answered within its 4 s.
        assert events["checks_ok"] >= before["checks_ok"] + 1, (before, events)
        assert events["checks_failed"] == before["checks_failed"], (before, events)

    def test_the_apn_is_fixed_once_dialled(self, dut, connected):
        assert cellular(dut, "APN internet")["rc"] == -EALREADY
        assert cellular(dut, "APN other")["rc"] == -EBUSY

    def test_networks_are_not_configurable(self, dut, connected):
        assert cellular(dut, "NETWORKS")["rc"] == -ENOSYS

    @pytest.mark.parametrize("host", sorted(HOSTS))
    def test_dns_resolves_the_hosts_region(self, dut, connected, host):
        result = dut.command(f"DNS {host}", 30.0)
        assert result["status"] == "ok", result
        assert result["ip"] == HOSTS[host]

    def test_a_literal_address_resolves_to_itself(self, dut, connected):
        result = dut.command("DNS 192.0.2.17")
        assert result["status"] == "ok" and result["ip"] == "192.0.2.17", result

    def test_an_unknown_name_fails_at_once(self, dut, connected):
        # The model answers NXDOMAIN, which Zephyr's resolver reports as
        # EAI_FAIL, a reply without answer records (resolve.c
        # dns_validate_msg); without an answer it would wait out
        # CONFIG_NET_SOCKETS_DNS_TIMEOUT, 10 s, and report EAI_AGAIN.
        result = dut.command("DNS nowhere.test", 30.0)
        assert result["status"] == "error" and result["rc"] == DNS_EAI_FAIL, result
        assert result["ms"] < 5000, result

    @pytest.mark.parametrize("length", [1, 16, 255, 256, 512, 1000])
    def test_tcp_echo_round_trips(self, dut, connected, length):
        before = {k: connected.get(k) for k in ("connections", "bytes_up", "bytes_down", "drops")}
        result = echo(dut, "TCP", "echo.test", f"len={length}", 3, ECHO_RING)
        assert result["status"] == "ok", result
        assert result["rounds"] == 3 and result["mismatches"] == 0
        assert result["sent"] == result["received"] == 3 * length
        assert result["crc32"] == zlib.crc32(pattern(length))
        assert result["head"] == pattern(length)[:16].hex()
        assert connected.get("connections") == before["connections"] + 1
        assert connected.get("bytes_up") == before["bytes_up"] + 3 * length
        assert connected.get("bytes_down") == before["bytes_down"] + 3 * length
        assert connected.get("drops") == before["drops"]

    def test_binary_bytes_survive_the_echo(self, dut, connected):
        # PPP escapes 0x7e and 0x7d and CMUX frames on 0xf9; none of it may
        # reach the payload.
        payload = bytes([0x7E, 0x7D, 0xF9, 0x00, 0x0D, 0x0A, 0xFF, 0x11, 0x13, 0x1A])
        result = echo(dut, "TCP", HOSTS["echo.test"], payload.hex(), 2, ECHO_RING)
        assert result["status"] == "ok", result
        assert result["mismatches"] == 0
        assert result["crc32"] == zlib.crc32(payload)
        assert result["head"] == payload.hex()

    @pytest.mark.parametrize("length", [1, 64, 256])
    def test_udp_echo_round_trips(self, dut, connected, length):
        before = connected.get("drops")
        result = echo(dut, "UDP", "echo.test", f"len={length}", 3, length)
        assert result["status"] == "ok", result
        assert result["rounds"] == 3 and result["mismatches"] == 0
        assert result["sent"] == result["received"] == 3 * length
        assert result["crc32"] == zlib.crc32(pattern(length))
        assert connected.get("drops") == before

    def test_http_get_returns_the_file_intact(self, dut, connected):
        result = dut.command(f"HTTP web.test {HTTP_PORT} {FILE_PATH}{FILE_NAME}", 60.0)
        assert result["status"] == "ok", result
        assert result["code"] == 200
        assert result["status_line"] == "HTTP/1.1 200 OK"
        assert result["content_length"] == len(FILE)
        assert result["body_length"] == len(FILE)
        assert result["body_crc32"] == zlib.crc32(FILE)
        assert connected.text_region("request", REQUEST_SIZE) == f"GET {FILE_PATH}{FILE_NAME} HTTP/1.1".encode()

    def test_http_answers_404_without_a_file(self, dut, connected):
        connected.set(file_length=0)
        try:
            result = dut.command(f"HTTP web.test {HTTP_PORT} /missing", 60.0)
        finally:
            connected.set(file_length=len(FILE))
        assert result["status"] == "ok", result
        assert result["code"] == 404
        assert result["content_length"] == 0 and result["body_length"] == 0

    def test_a_closed_port_refuses_the_connection(self, dut, connected):
        before = connected.get("connections")
        result = dut.command(f"TCP web.test {CLOSED_PORT} len=4", 30.0)
        assert result["status"] == "error" and result["stage"] == "connect", result
        assert result["errno"] == ECONNREFUSED
        assert connected.get("connections") == before

    def test_no_frame_arrived_with_a_bad_fcs(self, dut, connected):
        assert connected.get("frames_bad_fcs") == 0

    def test_a_short_loss_of_registration_redials(self, dut, connected):
        before = net(dut, "STATE")
        cycles = connected.get("power_cycles")
        connected.set(registration=0)
        try:
            down = net(dut, "WAIT DOWN 15000", 20.0)
            assert down["status"] == "ok" and down["l4"] is False, down
            # Deregistered: DORMANT closes LCP, then the driver redials.
            assert down["dormant"] or not down["carrier"], down
        finally:
            connected.set(registration=1)
        up = net(dut, f"WAIT UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
        assert up["status"] == "ok", up
        assert up["l4_down"] == before["l4_down"] + 1 and up["l4_up"] == before["l4_up"] + 1, up
        assert up["ppp_dead"] > before["ppp_dead"] and up["ppp_running"] > before["ppp_running"], up
        assert connected.get("power_cycles") == cycles
        assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"
        assert connected.get("unknown_commands") == 0

    def test_a_long_loss_of_registration_powers_down_until_resumed(self, dut, connected):
        cycles = connected.get("power_cycles")
        try:
            connected.set(registration=0)
            try:
                assert net(dut, "WAIT DOWN 15000", 20.0)["status"] == "ok"
                # Three failed dials and the driver suspends itself: CMUX closes.
                assert connected.wait_for("cmux", False, DIAL_GIVE_UP_S) is False
                assert connected.wait_for("powered", self.POWERED_AFTER_SUSPEND, 10.0) is self.POWERED_AFTER_SUSPEND
            finally:
                connected.set(registration=1)
            dut.harness.sleep(PERIODIC_S)
            # The driver stays down, and PM still thinks it is active.
            state = net(dut, "STATE")
            assert state["l4"] is False and state["pm"] == "active", state
            down = net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
            assert down["down_rc"] == self.LONG_LOSS_DOWN_RC, down
            assert down["pm_rc"] == 0 and down["pm"] == "suspended" and down["l4"] is False, down
            up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
            assert up["status"] == "ok" and up["pm_op"] == "resume", up
            assert connected.get("power_cycles") == cycles + 1
            assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"
            assert connected.get("unknown_commands") == 0
        finally:
            restart_driver(dut, connected)

    def test_power_off_and_on(self, dut, connected):
        cycles = connected.get("power_cycles")
        down = net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
        assert down["status"] == "ok" and down["down_rc"] == 0 and down["pm_rc"] == 0, down
        assert down["pm"] == "suspended" and down["admin_up"] is False and down["l4"] is False, down
        assert connected.wait_for("cmux", False, 5.0) is False
        assert connected.get("ppp_up") is False
        assert connected.get("powered") is self.POWERED_AFTER_SUSPEND
        again = net(dut, "DOWN", NET_DOWN_TIMEOUT_S)
        assert again["down_rc"] == -EALREADY and again["pm_rc"] == -EALREADY, again
        up = net(dut, f"UP {NET_UP_WAIT_MS}", NET_UP_TIMEOUT_S)
        assert up["status"] == "ok" and up["pm_op"] == "resume" and up["pm_rc"] == 0, up
        assert connected.get("power_cycles") == cycles + 1
        assert connected.get("cmux") is True and connected.get("ppp_up") is True
        assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"
        # Every command the driver sent, through init, dial, the periodic
        # and signal scripts, redials, shutdown and resume, was modelled.
        assert connected.get("unknown_commands") == 0
