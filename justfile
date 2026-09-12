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

# Remove build output.
clean:
    rm -rf "{{build_dir}}"
