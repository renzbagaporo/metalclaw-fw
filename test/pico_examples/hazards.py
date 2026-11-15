# SPDX-License-Identifier: Apache-2.0 OR MIT
# pico-examples programs that harm a real bench, and why. The hardware
# platform never runs them; the emulator runs them all.

HARDWARE_HAZARDS: dict[str, str] = {
    # otp/hello_otp/hello_otp.c writes OTP rows 0x400 and 0x410: permanent,
    # so it would pass once per chip and never again.
    "hello_otp": "burns OTP rows on the DUT for good",
    # These need `picotool otp load` of a signing key, which turns on secure
    # boot for good (encrypted/*, bootloaders/encrypted/*).
    "enc_bootloader": "needs secure boot turned on in the DUT's OTP",
    "hello_encrypted": "needs secure boot turned on in the DUT's OTP",
    "hello_encrypted_mbedtls": "needs secure boot turned on in the DUT's OTP",
    "hello_serial_enc": "needs secure boot turned on in the DUT's OTP",
    # pwm/hello_pwm/hello_pwm.c drives PWM on GP1, the DUT's console RX,
    # which the harness's console bridge drives too.
    "hello_pwm": "drives GP1 against the harness's console TX",
}
