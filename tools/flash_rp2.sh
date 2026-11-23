#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Flash the harness over its own SWD pads using JLinkExe, then reset and run it.
# Never depends on the firmware already on the board, so it is also a recovery
# path. Usage: tools/flash_rp2.sh <firmware.elf>
# METALCLAW_JLINK_DEVICE selects the target device (default RP2040_M0_0; use
# RP2350_M33_0 for RP235x harnesses). METALCLAW_JLINK_SERIAL selects the
# debugger when more than one is attached.

set -euo pipefail

elf="${1:?usage: flash_rp2.sh <firmware.elf>}"
test -f "$elf"

device="${METALCLAW_JLINK_DEVICE:-RP2040_M0_0}"
select=()
if [[ -n "${METALCLAW_JLINK_SERIAL:-}" ]]; then
    select=(-SelectEmuBySN "$METALCLAW_JLINK_SERIAL")
fi

script="$(mktemp)"
trap 'rm -f "$script"' EXIT
cat > "$script" <<CMDS
connect
halt
loadfile $elf
r
g
q
CMDS

JLinkExe -NoGui 1 -device "$device" -if SWD -speed 4000 -autoconnect 1 "${select[@]}" -CommandFile "$script"
