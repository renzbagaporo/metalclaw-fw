// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "mailbox.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
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

/*
 * A device overlay gives the image a `barometer` alias bound to Zephyr's own
 * driver for the simulated part; the DUT reads it exactly as production
 * firmware would. Values are reported in Pa and milli-degrees.
 */
#if defined(CONFIG_SENSOR) && DT_NODE_EXISTS(DT_ALIAS(barometer))
static void emit_sensor(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(barometer));
	struct sensor_value press;
	struct sensor_value temp;
	int rc;

	if (!device_is_ready(dev)) {
		rc = -ENODEV;
	} else {
		rc = sensor_sample_fetch(dev);
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		int64_t pressure_pa = (int64_t)press.val1 * 1000 + press.val2 / 1000;
		int64_t temperature_mc = (int64_t)temp.val1 * 1000 + temp.val2 / 1000;
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"ok\","
			"\"pressure_pa\":%lld,\"temperature_mc\":%lld}\r\n",
			(long long)pressure_pa, (long long)temperature_mc);
	}
	mclw_debug_port_write(result_line);
}
#else
static void emit_sensor(void)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"detail\":\"no sensor in this image\"}\r\n");
	mclw_debug_port_write(result_line);
}
#endif

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
	} else if (strcmp(command, "SENSOR") == 0) {
		emit_sensor();
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
