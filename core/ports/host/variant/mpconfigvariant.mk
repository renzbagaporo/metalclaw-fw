# SPDX-License-Identifier: Apache-2.0 OR MIT
# The Metalclaw host harness variant of MicroPython's unix port.

FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py

# Modules that need libraries or submodules the harness has no use for.
MICROPY_PY_FFI = 0
MICROPY_PY_BTREE = 0
MICROPY_PY_SSL = 0
MICROPY_PY_BLUETOOTH = 0
