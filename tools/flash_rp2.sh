#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Flash the harness over its own SWD pads with a J-Link, then reset and run it.
# Never depends on the firmware already on the board, so it is also the
# recovery path. Usage: tools/flash_rp2.sh <firmware.elf>
# METALCLAW_JLINK_SERIAL selects the probe when more than one is attached.

set -euo pipefail

elf="${1:?usage: flash_rp2.sh <firmware.elf>}"
test -f "$elf"

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

JLinkExe -NoGui 1 -device RP2040_M0_0 -if SWD -speed 4000 -autoconnect 1 "${select[@]}" -CommandFile "$script"
