// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DUT_MAILBOX_H
#define MCLW_DUT_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_DEBUG_PORT_MAGIC0 0x574c434dU
#define MCLW_DEBUG_PORT_COMMAND_MAGIC1 0x31444d43U
#define MCLW_DEBUG_PORT_RESULT_MAGIC1 0x31534552U
#define MCLW_DEBUG_PORT_VERSION 1U

#define MCLW_DEBUG_PORT_COMMAND_CAPACITY 100U
#define MCLW_DEBUG_PORT_RESULT_CAPACITY 484U

struct mclw_debug_port_command {
	uint32_t magic0;
	uint32_t magic1;
	uint32_t version;
	uint32_t capacity;
	volatile uint32_t sequence_begin;
	volatile uint32_t length;
	char payload[MCLW_DEBUG_PORT_COMMAND_CAPACITY];
	volatile uint32_t sequence_end;
};

struct mclw_debug_port_result {
	uint32_t magic0;
	uint32_t magic1;
	uint32_t version;
	uint32_t capacity;
	volatile uint32_t sequence_begin;
	volatile uint32_t length;
	char payload[MCLW_DEBUG_PORT_RESULT_CAPACITY];
	volatile uint32_t sequence_end;
};

_Static_assert(sizeof(struct mclw_debug_port_command) == 128U,
	"debug-port command record must stay 128 bytes");
_Static_assert(sizeof(struct mclw_debug_port_result) == 512U,
	"debug-port result record must stay 512 bytes");

extern struct mclw_debug_port_command mclw_debug_port_command;
extern struct mclw_debug_port_result mclw_debug_port_result;

void mclw_debug_port_init(void);
void mclw_debug_port_write(const char *text);
bool mclw_debug_port_poll(char *command, size_t capacity);

#endif
