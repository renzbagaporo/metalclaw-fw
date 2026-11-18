// SPDX-License-Identifier: Apache-2.0 OR MIT
// MicroPython's unix port as the Metalclaw host harness: the standard
// variant's features, with the `metalclaw` module linked in (USER_C_MODULES).

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#include "../../../third_party/micropython/ports/unix/variants/mpconfigvariant_common.h"

// A line no side drives reads high, as on a board with pull-ups on every
// DUT line: the bridge carries levels, not currents, and a released line
// is high to both sides.
#define METALCLAW_LINE_PULLUPS (1)
