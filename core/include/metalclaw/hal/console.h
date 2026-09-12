// SPDX-License-Identifier: Apache-2.0 OR MIT
// The harness's own console: the channel the REPL runs over. The port owns
// the transport; the binding only reads and writes it.

#ifndef METALCLAW_HAL_CONSOLE_H
#define METALCLAW_HAL_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

size_t metalclaw_hal_console_write(const void *buf, size_t size);
size_t metalclaw_hal_console_read(void *buf, size_t size);
bool metalclaw_hal_console_readable(void);
bool metalclaw_hal_console_writable(void);

#endif
