// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "mailbox.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_NAME "smoke"
#define IMAGE_REVISION "1"
#define IMAGE_BUILD (__DATE__ " " __TIME__)

#define RESULT_LINE_MAX 256U
#define COMMAND_MAX (MCLW_DEBUG_PORT_COMMAND_CAPACITY + 1U)
#define POLL_INTERVAL_MS 1

static char result_line[RESULT_LINE_MAX];
static char command_line[COMMAND_MAX];

static void emit_identity(const char *test)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"%s\",\"status\":\"ok\",\"image\":\"%s\","
		"\"revision\":\"%s\",\"build\":\"%s\"}\r\n",
		test, IMAGE_NAME, IMAGE_REVISION, IMAGE_BUILD);
	mclw_debug_port_write(result_line);
}

static void emit_ping(void)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_ping\",\"status\":\"ok\",\"image\":\"%s\"}\r\n",
		IMAGE_NAME);
	mclw_debug_port_write(result_line);
}

static void emit_unknown(void)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_command\",\"status\":\"error\","
		"\"detail\":\"unknown command\"}\r\n");
	mclw_debug_port_write(result_line);
}

static void trim(char *text)
{
	size_t length = strlen(text);

	while (length > 0U) {
		char last = text[length - 1U];

		if (last != ' ' && last != '\t' && last != '\r' && last != '\n') {
			break;
		}
		text[--length] = '\0';
	}
}

static void dispatch(const char *command)
{
	if (strcmp(command, "PING") == 0) {
		emit_ping();
	} else if (strcmp(command, "ID") == 0) {
		emit_identity("test_id");
	} else {
		emit_unknown();
	}
}

int main(void)
{
	mclw_debug_port_init();
	emit_identity("test_boot");
	while (true) {
		if (mclw_debug_port_poll(command_line, sizeof(command_line))) {
			trim(command_line);
			dispatch(command_line);
		}
		k_msleep(POLL_INTERVAL_MS);
	}
}
