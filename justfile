# SPDX-License-Identifier: Apache-2.0 OR MIT

set shell := ["bash", "-euo", "pipefail", "-c"]

project_dir := justfile_directory()
micropython_dir := project_dir / "core/third_party/micropython"
# A relative METALCLAW_BUILD_DIR is taken from the project directory; make
# runs from the MicroPython port directory and would put it inside the submodule.
build_dir := absolute_path(env_var_or_default("METALCLAW_BUILD_DIR", project_dir / "build"))
jobs := env_var_or_default("METALCLAW_JOBS", num_cpus())

# The board directory may live anywhere, e.g. a bench-specific board kept
# outside the tree. Its basename is the MicroPython board name.
board_dir := env_var_or_default("METALCLAW_BOARD_DIR", project_dir / "core/ports/rp2/boards/rpi_pi_pico")
board := file_name(board_dir)
rp2_build_dir := build_dir / "rp2" / board

# Pinned submodule commits. A clean checkout plus `just setup` selects exactly
# these; the recipes refuse to build against anything else.
micropython_commit := "e0e9fbb17ed6fd06bb76e266ae554784c9c80804"   # v1.28.0
pico_sdk_commit := "a1438dff1d38bd9c65dbd693f0e5db4b9ae91779"      # 2.2.0, as pinned by MicroPython

# The DUT fixture firmware is a Zephyr app. Zephyr, its modules and the SDK
# live outside the repository as well.
zephyr_workspace := env_var_or_default("METALCLAW_ZEPHYR_WORKSPACE", project_dir / "../.zephyr-workspace")
zephyr_base := zephyr_workspace / "zephyr"
zephyr_sdk := env_var_or_default("ZEPHYR_SDK_INSTALL_DIR", env_var("HOME") / "zephyr-sdk-1.0.1")
zephyr_sdk_url := "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz"
zephyr_sdk_sha256 := "ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb"
# The DUT's Zephyr board: the Pico 2 by default, `rpi_pico` for a Pico 1.
dut_board := env_var_or_default("METALCLAW_DUT_BOARD", "rpi_pico2/rp2350a/m33")

# Toolchain state lives outside the repository, next to it.
arm_toolchain_dir := env_var_or_default("METALCLAW_ARM_TOOLCHAIN_DIR", project_dir / "../.arm-none-eabi")
arm_toolchain_name := "arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
arm_toolchain_bin := arm_toolchain_dir / arm_toolchain_name / "bin"
arm_toolchain_url := "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz"
arm_toolchain_sha256 := "62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823"

# The emulator platform: Renode's portable package, released from the fork
# that carries the RP2040 and RP2350 models and the bench script, the two
# boot ROMs, and probe-rs with host timeouts that stretch for emulated SWD.
renode_commit := "f9b0719629786a3219851fec46ea70b6ab220464"
renode_package_url := "https://github.com/renzbagaporo/renode/releases/download/rp2-portable-" + replace_regex(renode_commit, "^(.{12}).*", "$1") + "/renode-1.17.0.linux-portable-headless.tar.gz"
renode_package_sha256 := "91805169ac8b86b14c329f94f3f000dc64e34a03128848b09236765b094b3abe"
renode_home := env_var_or_default("METALCLAW_RENODE_HOME", project_dir / "../.renode")
renode_dir := env_var_or_default("METALCLAW_RENODE_DIR", renode_home / renode_commit)
renode_roms := env_var_or_default("METALCLAW_RENODE_ROMS", renode_home / "roms")
rp2040_rom_url := "https://raw.githubusercontent.com/matgla/Renode_RP2040/5aca847c9f57ed96603e55d28e8ceeefa09e55f6/bootroms/rp2040/b2.elf"
rp2040_rom_sha256 := "3a3c4c617d83bc9b910a6fa7bfdffa11ce26ff89370271d861507be821cc49aa"
rp2350_rom_url := "https://github.com/raspberrypi/pico-bootrom-rp2350/releases/download/A2/bootrom-combined.bin"
rp2350_rom_sha256 := "196c503fbb17b2ad21a1f496574786af698f9376af9a5f79e5e75293c9e12191"
probe_rs_commit := "eab9c5e0ecb0eb7875050070719cf5370ac62d32"   # v0.30.0
probe_rs_patch := project_dir / "test/rig/platforms/emulator/probe_rs_0.30.0_emulation_timeouts.patch"
probe_rs_emulation_dir := env_var_or_default("METALCLAW_PROBE_RS_EMULATION_DIR", project_dir / "../.probe-rs-emulation")

default:
    @just --list

# Initialize the pinned submodules and install the ARM toolchain.
setup:
    #!/usr/bin/env bash
    set -euo pipefail
    git -C "{{project_dir}}" submodule update --init core/third_party/micropython
    git -C "{{micropython_dir}}" submodule update --init lib/pico-sdk lib/tinyusb lib/micropython-lib lib/mbedtls
    test "$(git -C "{{micropython_dir}}" rev-parse HEAD)" = "{{micropython_commit}}"
    test "$(git -C "{{micropython_dir}}/lib/pico-sdk" rev-parse HEAD)" = "{{pico_sdk_commit}}"
    if [[ ! -x "{{arm_toolchain_bin}}/arm-none-eabi-gcc" ]]; then
        mkdir -p "{{arm_toolchain_dir}}"
        tarball="{{arm_toolchain_dir}}/{{arm_toolchain_name}}.tar.xz"
        if [[ ! -f "${tarball}" ]]; then
            curl -fL --retry 3 --no-progress-meter -o "${tarball}.part" "{{arm_toolchain_url}}"
            mv "${tarball}.part" "${tarball}"
        fi
        echo "{{arm_toolchain_sha256}}  ${tarball}" | sha256sum --check --status
        tar -xJf "${tarball}" -C "{{arm_toolchain_dir}}"
        rm -f "${tarball}"
    fi
    test -x "{{arm_toolchain_bin}}/arm-none-eabi-gcc"
    echo "MicroPython $(git -C "{{micropython_dir}}" describe --tags)"
    echo "pico-sdk    $(git -C "{{micropython_dir}}/lib/pico-sdk" describe --tags)"
    echo "arm-gcc     $("{{arm_toolchain_bin}}/arm-none-eabi-gcc" -dumpversion)"

# Build the host mpy-cross compiler the firmware build needs.
build-mpy-cross:
    make -C "{{micropython_dir}}/mpy-cross" -j{{jobs}}

# Build the RP2040 harness image.
build: build-mpy-cross
    #!/usr/bin/env bash
    set -euo pipefail
    export PATH="{{arm_toolchain_bin}}:${PATH}"
    # METALCLAW_LINE_PULLUPS=ON or OFF sets a board's line pull-up option. The
    # build directory's CMake cache keeps it until it is set again.
    if [[ -n "${METALCLAW_LINE_PULLUPS:-}" ]]; then
        export CMAKE_ARGS="-DMETALCLAW_LINE_PULLUPS=$METALCLAW_LINE_PULLUPS"
        if [[ -f "{{rp2_build_dir}}/CMakeCache.txt" ]]; then
            cmake -DMETALCLAW_LINE_PULLUPS="$METALCLAW_LINE_PULLUPS" "{{rp2_build_dir}}" > /dev/null
        fi
    fi
    make -C "{{micropython_dir}}/ports/rp2" -j{{jobs}} \
        BOARD_DIR="{{board_dir}}" \
        BUILD="{{rp2_build_dir}}" \
        USER_C_MODULES="{{project_dir}}/core/micropython/micropython.cmake"
    test -f "{{board_dir}}/mpconfigboard.h"
    test -f "{{rp2_build_dir}}/firmware.uf2"
    ls -l "{{rp2_build_dir}}/firmware.uf2"

# Python environment for the rig, tools and client.
venv := project_dir / ".venv"
rig_python := venv / "bin/python"

setup-rig:
    #!/usr/bin/env bash
    set -euo pipefail
    test -x "{{rig_python}}" || /usr/bin/python3 -m venv "{{venv}}"
    "{{rig_python}}" -m pip install --quiet --upgrade pip
    "{{rig_python}}" -m pip install --quiet -r "{{project_dir}}/test/rig/requirements.txt"
    "{{rig_python}}" -c "import pytest, serial; print('rig python ok:', pytest.__version__, serial.__version__)"

# Run the cases on a platform. Extra arguments go to pytest.
test PLATFORM="hardware" *ARGS: setup-rig
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{PLATFORM}}" == emulator ]]; then
        export METALCLAW_RENODE_DIR="{{renode_dir}}" METALCLAW_RENODE_ROMS="{{renode_roms}}"
        # The board `just build` builds picks the bench.
        export METALCLAW_HARNESS_BOARD="{{board}}"
        export METALCLAW_PROBE_RS="${METALCLAW_PROBE_RS:-{{probe_rs_emulation_dir}}/bin/probe-rs}"
        echo "probe-rs: $("$METALCLAW_PROBE_RS" --version | head -1), patched with $(basename "{{probe_rs_patch}}")"
    fi
    cd "{{project_dir}}" && METALCLAW_ARM_TOOLCHAIN_DIR="{{arm_toolchain_dir}}" \
        "{{rig_python}}" test/rig/run.py --platform {{PLATFORM}} {{ARGS}}

# The emulator platform's tools: the boot ROMs, patched probe-rs and Renode.
setup-emulator: setup-emulator-roms setup-emulator-probe-rs setup-renode

setup-emulator-roms:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{renode_roms}}"
    fetch() {
        local url=$1 sha=$2 out="{{renode_roms}}/$(basename "$1")"
        if ! echo "$sha  $out" | sha256sum --check --status 2>/dev/null; then
            curl -fL --retry 3 --no-progress-meter -o "$out.part" "$url"
            echo "$sha  $out.part" | sha256sum --check --status
            mv "$out.part" "$out"
        fi
    }
    fetch "{{rp2040_rom_url}}" "{{rp2040_rom_sha256}}"
    fetch "{{rp2350_rom_url}}" "{{rp2350_rom_sha256}}"
    ls -l "{{renode_roms}}"

# probe-rs 0.30.0 with three host-side timeouts made configurable; unset,
# they keep upstream's defaults. Needs cargo.
setup-emulator-probe-rs:
    #!/usr/bin/env bash
    set -euo pipefail
    stamp="{{probe_rs_emulation_dir}}/built-from"
    want="{{probe_rs_commit}} $(sha256sum "{{probe_rs_patch}}" | cut -d' ' -f1)"
    if [[ -x "{{probe_rs_emulation_dir}}/bin/probe-rs" && "$(cat "$stamp" 2>/dev/null)" == "$want" ]]; then
        "{{probe_rs_emulation_dir}}/bin/probe-rs" --version; exit 0
    fi
    src="{{probe_rs_emulation_dir}}/src"
    rm -rf "$src"
    git init --quiet "$src"
    git -C "$src" fetch --quiet --depth 1 https://github.com/probe-rs/probe-rs.git "{{probe_rs_commit}}"
    git -C "$src" checkout --quiet FETCH_HEAD
    git -C "$src" -c user.name=metalclaw -c user.email=metalclaw@localhost am --quiet "{{probe_rs_patch}}"
    cargo install --quiet --locked --path "$src/probe-rs-tools" --bin probe-rs --root "{{probe_rs_emulation_dir}}"
    echo "$want" > "$stamp"
    "{{probe_rs_emulation_dir}}/bin/probe-rs" --version

# Renode's portable package for the pinned commit, which bundles .NET.
setup-renode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -x "{{renode_dir}}/renode" ]]; then "{{renode_dir}}/renode" --version; exit 0; fi
    package="{{renode_dir}}.tar.gz"
    rm -rf "{{renode_dir}}.part"
    mkdir -p "{{renode_dir}}.part"
    curl -fL --retry 3 --no-progress-meter -o "$package" "{{renode_package_url}}"
    echo "{{renode_package_sha256}}  $package" | sha256sum --check --status
    tar -xzf "$package" -C "{{renode_dir}}.part" --strip-components=1
    rm "$package"
    mv "{{renode_dir}}.part" "{{renode_dir}}"
    "{{renode_dir}}/renode" --version

# Create the Zephyr workspace for the DUT fixture from test/rig/dut/west.yml.
# METALCLAW_ZEPHYR_REFERENCE may name a directory holding local clones
# (zephyr/, modules/...) to borrow objects from instead of downloading.
setup-dut: setup-rig
    #!/usr/bin/env bash
    set -euo pipefail
    "{{rig_python}}" -m pip install --quiet west
    mkdir -p "{{zephyr_workspace}}/manifest"
    cp "{{project_dir}}/test/rig/dut/west.yml" "{{zephyr_workspace}}/manifest/west.yml"
    if [[ ! -f "{{zephyr_workspace}}/.west/config" ]]; then
        "{{venv}}/bin/west" init -l "{{zephyr_workspace}}/manifest"
    fi
    ref="${METALCLAW_ZEPHYR_REFERENCE:-}"
    if [[ -n "$ref" && ! -d "{{zephyr_base}}/.git" ]]; then
        git clone --quiet --reference-if-able "$ref/zephyr" https://github.com/zephyrproject-rtos/zephyr.git "{{zephyr_base}}"
    fi
    cd "{{zephyr_workspace}}" && "{{venv}}/bin/west" update --narrow --fetch-opt=--filter=blob:none
    "{{rig_python}}" -m pip install --quiet -r "{{zephyr_base}}/scripts/requirements-base.txt"
    "{{venv}}/bin/west" -v list
    echo "Zephyr $(git -C "{{zephyr_base}}" describe --tags)"

# Install the Zephyr SDK with only the Arm toolchain, at ZEPHYR_SDK_INSTALL_DIR.
setup-zephyr-sdk:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -x "{{zephyr_sdk}}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]]; then exit 0; fi
    part="{{zephyr_sdk}}.part"
    rm -rf "$part"; mkdir -p "$part"
    curl -fL --retry 3 --no-progress-meter -o "$part/sdk.tar.xz" "{{zephyr_sdk_url}}"
    echo "{{zephyr_sdk_sha256}}  $part/sdk.tar.xz" | sha256sum --check --status
    tar -xJf "$part/sdk.tar.xz" -C "$part" --strip-components=1
    rm "$part/sdk.tar.xz"
    (cd "$part" && ./setup.sh -t arm-zephyr-eabi)
    mv "$part" "{{zephyr_sdk}}"
    "{{zephyr_sdk}}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" -dumpversion

# Build the DUT fixture firmware for the Pico 2. With a DEVICE, its
# devices/<DEVICE>/test/dut/rp2.{overlay,conf} are applied so Zephyr's real
# driver for that part is in the image; DEVICE/VARIANT uses the files under
# test/dut/VARIANT/ instead. Output: build/dut/<DEVICE|smoke>/.
build-dut DEVICE="": setup-rig
    #!/usr/bin/env bash
    set -euo pipefail
    test -f "{{zephyr_base}}/VERSION" || { echo "no Zephyr workspace at {{zephyr_workspace}}; run 'just setup-dut'" >&2; exit 2; }
    test -d "{{zephyr_sdk}}" || { echo "no Zephyr SDK at {{zephyr_sdk}}; set ZEPHYR_SDK_INSTALL_DIR" >&2; exit 2; }
    export ZEPHYR_BASE="{{zephyr_base}}" ZEPHYR_SDK_INSTALL_DIR="{{zephyr_sdk}}" ZEPHYR_TOOLCHAIN_VARIANT=zephyr
    export PATH="{{venv}}/bin:$PATH"
    name="{{DEVICE}}"; extra=()
    if [[ -n "$name" ]]; then
        dut="{{project_dir}}/devices/${name%%/*}/test/dut"
        [[ "$name" == */* ]] && dut="$dut/${name#*/}"
        test -f "$dut/rp2.overlay" || { echo "no $dut/rp2.overlay" >&2; exit 2; }
        extra+=(-DDTC_OVERLAY_FILE="$dut/rp2.overlay")
        test -f "$dut/rp2.conf" && extra+=(-DEXTRA_CONF_FILE="$dut/rp2.conf")
    else
        name=smoke
    fi
    build="{{build_dir}}/dut/$name"
    cmake -S "{{project_dir}}/test/rig/dut" -B "$build" -GNinja \
        -DBOARD="{{dut_board}}" -DPython3_EXECUTABLE="{{rig_python}}" \
        -DMETALCLAW_DUT_IMAGE_NAME="$name" "${extra[@]}"
    cmake --build "$build"
    ls -l "$build/zephyr/zephyr.elf"

# Remove build output.
clean:
    rm -rf "{{build_dir}}"
