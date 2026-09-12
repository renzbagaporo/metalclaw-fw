# SPDX-License-Identifier: Apache-2.0 OR MIT

set shell := ["bash", "-euo", "pipefail", "-c"]

project_dir := justfile_directory()
micropython_dir := project_dir / "core/third_party/micropython"
build_dir := env_var_or_default("METALCLAW_BUILD_DIR", project_dir / "build")
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
dut_board := "rpi_pico2/rp2350a/m33"

# Toolchain state lives outside the repository, next to it.
arm_toolchain_dir := env_var_or_default("METALCLAW_ARM_TOOLCHAIN_DIR", project_dir / "../.arm-none-eabi")
arm_toolchain_name := "arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
arm_toolchain_bin := arm_toolchain_dir / arm_toolchain_name / "bin"
arm_toolchain_url := "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz"
arm_toolchain_sha256 := "62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823"

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
    cd "{{project_dir}}" && "{{rig_python}}" test/rig/run.py --platform {{PLATFORM}} {{ARGS}}

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
