# SPDX-License-Identifier: Apache-2.0 OR MIT
# The `metalclaw` user C module for a make-built MicroPython port (the host
# harness: MicroPython's unix port). The core and the host port are built
# first by CMake (core/ports/host/CMakeLists.txt); METALCLAW_HOST_LIBS names
# the two static libraries, linked as a group since each calls the other.

METALCLAW_MOD_DIR := $(USERMOD_DIR)

SRC_USERMOD_C += $(METALCLAW_MOD_DIR)/modmetalclaw.c
SRC_USERMOD_C += $(METALCLAW_MOD_DIR)/instance.c

CFLAGS_USERMOD += -I$(METALCLAW_MOD_DIR) -I$(METALCLAW_MOD_DIR)/../include

LDFLAGS_USERMOD += -Wl,--start-group $(METALCLAW_HOST_LIBS) -Wl,--end-group -lpthread
# The host port's terminal.c: stdin keeps what was typed ahead.
LDFLAGS_USERMOD += -Wl,--wrap=tcsetattr
