// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/onewire.h"

enum {
    // Waiting for a reset: a command not for this part, or nothing yet.
    STATE_IDLE,
    STATE_ROM,
    // A search: the part sends its ROM bit, then its complement, then
    // reads the master's choice and stays only if it matches (figure 11).
    STATE_SEARCH_BIT,
    STATE_SEARCH_COMPLEMENT,
    STATE_SEARCH_CHOICE,
    STATE_MATCH,
    STATE_FUNCTION,
    STATE_TX,
    STATE_RX,
    STATE_STATUS,
};

// Where a transmission leads: READ ROM's to a function command, a
// function's to idle or its status.
#define AFTER_TX_FUNCTION 0xffu

static bool MCLW_RAM_FUNC(rom_bit)(const struct mclw_onewire *ow, uint8_t index) {
    return ((ow->rom[index / 8u] >> (index % 8u)) & 1u) != 0u;
}

static bool MCLW_RAM_FUNC(tx_bit)(const struct mclw_onewire *ow) {
    return ((ow->tx[ow->index / 8u] >> (ow->index % 8u)) & 1u) != 0u;
}

static bool MCLW_RAM_FUNC(start_tx)(struct mclw_onewire *ow, const uint8_t *data, uint8_t length,
    uint8_t after) {
    memcpy(ow->tx, data, length);
    ow->tx_bits = (uint8_t)(length * 8u);
    ow->index = 0;
    ow->function = after;
    ow->state = STATE_TX;
    return tx_bit(ow);
}

// Shifts a written bit in, LSB first; true once a byte is complete.
static bool MCLW_RAM_FUNC(shift_in)(struct mclw_onewire *ow, bool level) {
    ow->shift = (uint8_t)((ow->shift >> 1) | (level ? 0x80u : 0u));
    ow->bits++;
    if (ow->bits < 8u) {
        return false;
    }
    ow->bits = 0;
    return true;
}

static bool MCLW_RAM_FUNC(selected)(struct mclw_onewire *ow) {
    ow->counts.selected++;
    ow->state = STATE_FUNCTION;
    ow->bits = 0;
    return true;
}

static bool MCLW_RAM_FUNC(after_function)(struct mclw_onewire *ow) {
    const struct mclw_onewire_function *f = &ow->functions[ow->function];

    if ((f->flags & MCLW_ONEWIRE_STATUS) != 0u) {
        ow->state = STATE_STATUS;
        return ow->status;
    }
    ow->state = STATE_IDLE;
    return true;
}

static bool MCLW_RAM_FUNC(rom_command)(struct mclw_onewire *ow, uint8_t command) {
    ow->index = 0;
    switch (command) {
        case MCLW_ONEWIRE_READ_ROM:
            ow->counts.selected++;
            return start_tx(ow, ow->rom, 8u, AFTER_TX_FUNCTION);
        case MCLW_ONEWIRE_MATCH_ROM:
            ow->state = STATE_MATCH;
            return true;
        case MCLW_ONEWIRE_SKIP_ROM:
            return selected(ow);
        case MCLW_ONEWIRE_ALARM_SEARCH:
        case MCLW_ONEWIRE_SEARCH_ROM:
            if (command == MCLW_ONEWIRE_ALARM_SEARCH && !ow->alarm) {
                break;
            }
            ow->state = STATE_SEARCH_BIT;
            return rom_bit(ow, 0);
        default:
            break;
    }
    ow->state = STATE_IDLE;
    return true;
}

static bool MCLW_RAM_FUNC(function_command)(struct mclw_onewire *ow, uint8_t command) {
    for (uint8_t i = 0; i < ow->function_count; i++) {
        const struct mclw_onewire_function *f = &ow->functions[i];

        if (f->command != command) {
            continue;
        }
        ow->function = i;
        ow->issued |= 1u << i;
        ow->counts.commands++;
        if ((f->flags & MCLW_ONEWIRE_BUSY) != 0u) {
            ow->status = false;
        }
        if (f->tx_length > 0u && f->tx_offset < MCLW_ONEWIRE_MEMORY) {
            uint8_t room = (uint8_t)(MCLW_ONEWIRE_MEMORY - f->tx_offset);
            return start_tx(ow, &ow->memory[f->tx_offset], f->tx_length < room ? f->tx_length : room, i);
        }
        if (f->rx_length > 0u) {
            ow->rx_count = 0;
            ow->index = 0;
            ow->bits = 0;
            ow->state = STATE_RX;
            return true;
        }
        return after_function(ow);
    }
    ow->state = STATE_IDLE;
    return true;
}

void MCLW_RAM_FUNC(mclw_onewire_reset)(struct mclw_onewire *ow) {
    ow->counts.resets++;
    ow->state = STATE_ROM;
    ow->shift = 0;
    ow->bits = 0;
    ow->index = 0;
}

bool MCLW_RAM_FUNC(mclw_onewire_slot)(struct mclw_onewire *ow, bool level) {
    switch (ow->state) {
        case STATE_ROM:
            return shift_in(ow, level) ? rom_command(ow, ow->shift) : true;
        case STATE_SEARCH_BIT:
            ow->state = STATE_SEARCH_COMPLEMENT;
            return !rom_bit(ow, ow->index);
        case STATE_SEARCH_COMPLEMENT:
            ow->state = STATE_SEARCH_CHOICE;
            return true;
        case STATE_SEARCH_CHOICE:
            if (level != rom_bit(ow, ow->index)) {
                ow->state = STATE_IDLE;
                return true;
            }
            if (++ow->index == 64u) {
                ow->counts.searches++;
                return selected(ow);
            }
            ow->state = STATE_SEARCH_BIT;
            return rom_bit(ow, ow->index);
        case STATE_MATCH:
            if (level != rom_bit(ow, ow->index)) {
                ow->state = STATE_IDLE;
                return true;
            }
            if (++ow->index == 64u) {
                ow->counts.matches++;
                return selected(ow);
            }
            return true;
        case STATE_FUNCTION:
            return shift_in(ow, level) ? function_command(ow, ow->shift) : true;
        case STATE_TX:
            if (++ow->index < ow->tx_bits) {
                return tx_bit(ow);
            }
            if (ow->function == AFTER_TX_FUNCTION) {
                return selected(ow);
            }
            return after_function(ow);
        case STATE_RX:
            if (shift_in(ow, level) && ow->rx_count < MCLW_ONEWIRE_RX_MAX) {
                ow->rx[ow->rx_count++] = ow->shift;
                if (ow->rx_count == ow->functions[ow->function].rx_length) {
                    return after_function(ow);
                }
            }
            return true;
        case STATE_STATUS:
            return ow->status;
        default:
            return true;
    }
}

void mclw_onewire_init(struct mclw_onewire *ow, const uint8_t rom[8],
    const struct mclw_onewire_function *functions, uint8_t function_count) {
    uint32_t state = mclw_hal_critical_enter();

    memcpy(ow->rom, rom, sizeof(ow->rom));
    ow->functions = functions;
    ow->function_count = function_count < MCLW_ONEWIRE_FUNCTIONS_MAX ? function_count :
        (uint8_t)MCLW_ONEWIRE_FUNCTIONS_MAX;
    ow->issued = 0;
    ow->rx_count = 0;
    memset(&ow->counts, 0, sizeof(ow->counts));
    ow->state = STATE_IDLE;
    ow->shift = 0;
    ow->bits = 0;
    ow->index = 0;
    mclw_hal_critical_exit(state);
}

uint32_t mclw_onewire_take(struct mclw_onewire *ow, uint8_t *rx, uint8_t *rx_count) {
    uint32_t issued = ow->issued;

    ow->issued = 0;
    if (rx != NULL && rx_count != NULL) {
        *rx_count = ow->rx_count;
        memcpy(rx, ow->rx, ow->rx_count);
    }
    return issued;
}

uint8_t mclw_onewire_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (unsigned b = 0; b < 8u; b++) {
            bool mix = ((crc ^ byte) & 1u) != 0u;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8cu;
            }
            byte >>= 1;
        }
    }
    return crc;
}
