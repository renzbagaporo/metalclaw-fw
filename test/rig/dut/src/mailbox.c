// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "mailbox.h"

#include <zephyr/sys/barrier.h>
#include <zephyr/toolchain.h>

#include <string.h>

struct mclw_debug_port_command mclw_debug_port_command __aligned(4);
struct mclw_debug_port_result mclw_debug_port_result __aligned(4);

static char result_line[MCLW_DEBUG_PORT_RESULT_CAPACITY];
static size_t result_used;
static uint32_t command_seen;

static void initialise_header(uint32_t *magic0, uint32_t *magic1, uint32_t *version,
	uint32_t *capacity, uint32_t magic1_value, uint32_t capacity_value)
{
	*capacity = capacity_value;
	*version = MCLW_DEBUG_PORT_VERSION;
	barrier_dmem_fence_full();
	*magic1 = magic1_value;
	barrier_dmem_fence_full();
	*magic0 = MCLW_DEBUG_PORT_MAGIC0;
}

void mclw_debug_port_init(void)
{
	memset(&mclw_debug_port_command, 0, sizeof(mclw_debug_port_command));
	memset(&mclw_debug_port_result, 0, sizeof(mclw_debug_port_result));
	result_used = 0U;
	command_seen = 0U;
	barrier_dmem_fence_full();
	initialise_header(&mclw_debug_port_command.magic0, &mclw_debug_port_command.magic1,
		&mclw_debug_port_command.version, &mclw_debug_port_command.capacity,
		MCLW_DEBUG_PORT_COMMAND_MAGIC1, MCLW_DEBUG_PORT_COMMAND_CAPACITY);
	initialise_header(&mclw_debug_port_result.magic0, &mclw_debug_port_result.magic1,
		&mclw_debug_port_result.version, &mclw_debug_port_result.capacity,
		MCLW_DEBUG_PORT_RESULT_MAGIC1, MCLW_DEBUG_PORT_RESULT_CAPACITY);
}

/*
 * The host reads the record in ascending address order, so the writer goes
 * the other way: sequence_end first, sequence_begin last. Any read that
 * overlaps a publish then sees begin != end. The old order (begin first)
 * let a whole publish hide inside one slow SWD read.
 */
static void publish_result(void)
{
	uint32_t next = mclw_debug_port_result.sequence_begin + 1U;

	mclw_debug_port_result.sequence_end = next;
	barrier_dmem_fence_full();
	memcpy(mclw_debug_port_result.payload, result_line, result_used);
	memset(mclw_debug_port_result.payload + result_used, 0,
		MCLW_DEBUG_PORT_RESULT_CAPACITY - result_used);
	mclw_debug_port_result.length = (uint32_t)result_used;
	barrier_dmem_fence_full();
	mclw_debug_port_result.sequence_begin = next;
}

void mclw_debug_port_write(const char *text)
{
	if (text == NULL) {
		return;
	}
	for (; *text != '\0'; text++) {
		if (result_used < sizeof(result_line)) {
			result_line[result_used++] = *text;
		}
		if (*text == '\n') {
			publish_result();
			result_used = 0U;
		}
	}
}

bool mclw_debug_port_poll(char *command, size_t capacity)
{
	uint32_t begin;
	uint32_t end;
	uint32_t length;

	if (command == NULL || capacity == 0U ||
		mclw_debug_port_command.magic0 != MCLW_DEBUG_PORT_MAGIC0 ||
		mclw_debug_port_command.magic1 != MCLW_DEBUG_PORT_COMMAND_MAGIC1 ||
		mclw_debug_port_command.version != MCLW_DEBUG_PORT_VERSION ||
		mclw_debug_port_command.capacity != MCLW_DEBUG_PORT_COMMAND_CAPACITY) {
		return false;
	}
	begin = mclw_debug_port_command.sequence_begin;
	barrier_dmem_fence_full();
	length = mclw_debug_port_command.length;
	if (length == 0U || length > MCLW_DEBUG_PORT_COMMAND_CAPACITY || length >= capacity) {
		return false;
	}
	memcpy(command, mclw_debug_port_command.payload, length);
	barrier_dmem_fence_full();
	end = mclw_debug_port_command.sequence_end;
	if (begin == 0U || begin != end || begin == command_seen) {
		return false;
	}
	command[length] = '\0';
	command_seen = end;
	return true;
}
