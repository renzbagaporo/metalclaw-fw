// SPDX-License-Identifier: Apache-2.0 OR MIT
// A USB-to-serial adapter on the DUT's console UART: the DUT finds a UART
// on its pins, the host finds a serial port on the harness's USB.

#ifndef MCLW_CONSOLE_BRIDGE_H
#define MCLW_CONSOLE_BRIDGE_H

#include "metalclaw/model.h"

#define CONSOLE_BRIDGE_DEFAULT_BAUD 115200u
#define CONSOLE_BRIDGE_BAUD_MIN 300
#define CONSOLE_BRIDGE_BAUD_MAX 3000000

// The interface string; hosts that list USB interfaces see it, Linux's
// by-id name does not carry it.
#define CONSOLE_BRIDGE_INTERFACE "Metalclaw DUT console"

extern const struct mclw_model mclw_model_console_bridge;

#endif
