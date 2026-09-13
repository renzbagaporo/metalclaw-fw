// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "mailbox.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif
#if defined(CONFIG_SPI)
#include <zephyr/drivers/spi.h>
#endif
#if defined(CONFIG_GNSS)
#include <zephyr/drivers/gnss.h>
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IMAGE_NAME
#define IMAGE_NAME "smoke"
#endif
#define IMAGE_REVISION "1"
#define IMAGE_BUILD (__DATE__ " " __TIME__)

#define RESULT_LINE_MAX 512U
#define RESULT_BODY_MAX (RESULT_LINE_MAX - 4U)
#define COMMAND_MAX (MCLW_DEBUG_PORT_COMMAND_CAPACITY + 1U)
#define POLL_INTERVAL_MS 1

static char result_line[RESULT_LINE_MAX];
static char command_line[COMMAND_MAX];

/* Survives a reset, not a power cycle: how many times this image booted. */
static uint32_t boot_magic __noinit;
static uint32_t boot_count __noinit;

static void emit_identity(const char *test)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"%s\",\"status\":\"ok\",\"image\":\"%s\","
		"\"revision\":\"%s\",\"build\":\"%s\",\"boots\":%u}\r\n",
		test, IMAGE_NAME, IMAGE_REVISION, IMAGE_BUILD, (unsigned int)boot_count);
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

#if defined(CONFIG_SENSOR) || defined(CONFIG_SPI)
struct token {
	const char *text;
	size_t length;
};

static size_t append(char *text, size_t size, size_t used, const char *format, ...)
{
	va_list arguments;
	int written;

	if (used >= size) {
		return size;
	}
	va_start(arguments, format);
	written = vsnprintf(&text[used], size - used, format, arguments);
	va_end(arguments);
	if (written < 0) {
		return used;
	}
	used += (size_t)written;
	return used < size ? used : size;
}

static bool next_token(const char **cursor, struct token *token)
{
	const char *text = *cursor;

	while (*text == ' ') {
		text++;
	}
	if (*text == '\0') {
		*cursor = text;
		return false;
	}
	token->text = text;
	while (*text != '\0' && *text != ' ') {
		text++;
	}
	token->length = (size_t)(text - token->text);
	*cursor = text;
	return true;
}

static bool token_equals(const struct token *token, const char *name)
{
	return strlen(name) == token->length && strncmp(token->text, name, token->length) == 0;
}

static void emit_error(const char *test, const char *detail)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"%s\",\"status\":\"error\",\"detail\":\"%s\"}\r\n",
		test, detail);
	mclw_debug_port_write(result_line);
}
#endif

#if defined(CONFIG_SENSOR)
#define SENSOR_CHANNELS_MAX 4U
#define SENSOR_VALUES_MAX 12U

struct sensor_alias {
	const char *name;
	const struct device *dev;
};

struct sensor_channel_name {
	const char *name;
	enum sensor_channel channel;
	size_t values;
};

#define SENSOR_ALIAS(id) IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(id)), \
	({STRINGIFY(id), DEVICE_DT_GET(DT_ALIAS(id))},))

static const struct sensor_alias sensor_aliases[] = {
	SENSOR_ALIAS(barometer)
	SENSOR_ALIAS(imu)
	{.name = NULL},
};

static const struct sensor_channel_name sensor_channel_names[] = {
	{"press", SENSOR_CHAN_PRESS, 1},
	{"ambient_temp", SENSOR_CHAN_AMBIENT_TEMP, 1},
	{"accel_xyz", SENSOR_CHAN_ACCEL_XYZ, 3},
	{"gyro_xyz", SENSOR_CHAN_GYRO_XYZ, 3},
	{"die_temp", SENSOR_CHAN_DIE_TEMP, 1},
	{.name = NULL},
};

static bool token_is_count(const struct token *token, unsigned long *count)
{
	static const char prefix[] = "count=";
	size_t offset = 0;
	unsigned long value = 0;

	if (token->length > sizeof(prefix) - 1U &&
		strncmp(token->text, prefix, sizeof(prefix) - 1U) == 0) {
		offset = sizeof(prefix) - 1U;
	} else if (token->text[0] < '0' || token->text[0] > '9') {
		return false;
	}
	for (size_t i = offset; i < token->length; i++) {
		if (token->text[i] < '0' || token->text[i] > '9') {
			return false;
		}
		value = value * 10U + (unsigned long)(token->text[i] - '0');
	}
	*count = value;
	return true;
}

static int fetch_channels(const struct device *dev,
	const struct sensor_channel_name *const *channels, size_t count, int64_t *out)
{
	struct sensor_value values[3];
	size_t at = 0;
	int rc = sensor_sample_fetch(dev);

	for (size_t i = 0; rc == 0 && i < count; i++) {
		rc = sensor_channel_get(dev, channels[i]->channel, values);
		for (size_t v = 0; rc == 0 && v < channels[i]->values; v++) {
			out[at++] = sensor_value_to_micro(&values[v]);
		}
	}
	return rc;
}

static void emit_sensor(const char *args)
{
	const struct sensor_channel_name *channels[SENSOR_CHANNELS_MAX];
	const char *cursor = args != NULL ? args : "";
	const struct device *dev = NULL;
	const char *detail = NULL;
	int64_t first[SENSOR_VALUES_MAX];
	int64_t again[SENSOR_VALUES_MAX];
	struct token token;
	size_t channel_count = 0;
	size_t values = 0;
	size_t used;
	unsigned long count = 1;
	unsigned long mismatches = 0;
	int rc = 0;

	if (!next_token(&cursor, &token)) {
		detail = "no alias";
	} else {
		for (const struct sensor_alias *a = sensor_aliases; a->name != NULL; a++) {
			if (token_equals(&token, a->name)) {
				dev = a->dev;
			}
		}
		if (dev == NULL) {
			detail = "unknown alias";
		}
	}
	while (detail == NULL && next_token(&cursor, &token)) {
		const struct sensor_channel_name *found = NULL;

		if (token_is_count(&token, &count)) {
			continue;
		}
		for (const struct sensor_channel_name *c = sensor_channel_names; c->name != NULL; c++) {
			if (token_equals(&token, c->name)) {
				found = c;
			}
		}
		if (found == NULL) {
			detail = "unknown channel";
		} else if (channel_count == SENSOR_CHANNELS_MAX ||
			values + found->values > SENSOR_VALUES_MAX) {
			detail = "too many channels";
		} else {
			channels[channel_count++] = found;
			values += found->values;
		}
	}
	if (detail == NULL && channel_count == 0) {
		detail = "no channel";
	}
	if (detail == NULL && count == 0) {
		detail = "bad count";
	}
	if (detail != NULL) {
		emit_error("test_sensor", detail);
		return;
	}
	if (!device_is_ready(dev)) {
		rc = -ENODEV;
	}
	for (unsigned long i = 0; rc == 0 && i < count; i++) {
		rc = fetch_channels(dev, channels, channel_count, i == 0 ? first : again);
		if (rc == 0 && i > 0 && memcmp(first, again, values * sizeof(first[0])) != 0) {
			mismatches++;
		}
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"errno\":%d,"
			"\"init_res\":%u}\r\n", -rc, (unsigned int)dev->state->init_res);
		mclw_debug_port_write(result_line);
		return;
	}
	used = append(result_line, RESULT_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"ok\","
		"\"count\":%lu,\"mismatches\":%lu", count, mismatches);
	values = 0;
	for (size_t i = 0; i < channel_count; i++) {
		used = append(result_line, RESULT_BODY_MAX, used, ",\"%s\":[", channels[i]->name);
		for (size_t v = 0; v < channels[i]->values; v++) {
			used = append(result_line, RESULT_BODY_MAX, used, "%s%lld",
				v == 0 ? "" : ",", (long long)first[values++]);
		}
		used = append(result_line, RESULT_BODY_MAX, used, "]");
	}
	append(result_line, sizeof(result_line), used, "}\r\n");
	mclw_debug_port_write(result_line);
}
#else
static void emit_sensor(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"detail\":\"no sensor in this image\"}\r\n");
	mclw_debug_port_write(result_line);
}
#endif

#if defined(CONFIG_SPI)
#define SPI_BYTES_MAX 16U
#define SPI_OPERATION (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

struct spi_alias {
	const char *name;
	struct spi_dt_spec spec;
};

#define SPI_ALIAS(id) IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(id)), \
	({STRINGIFY(id), SPI_DT_SPEC_GET(DT_ALIAS(id), SPI_OPERATION)},))

static const struct spi_alias spi_aliases[] = {
	SPI_ALIAS(adc)
	{.name = NULL},
};

/*
 * The PL022 driver reconfigures only when the config pointer changes, so
 * two configs alternate: every command is then applied with its own clock.
 */
static struct spi_config spi_configs[2];
static unsigned int spi_config_index;

static void emit_spi(const char *args)
{
	const struct spi_alias *alias = NULL;
	const char *cursor = args != NULL ? args : "";
	uint8_t transmit[SPI_BYTES_MAX] = {0};
	uint8_t first[SPI_BYTES_MAX] = {0};
	uint8_t receive[SPI_BYTES_MAX] = {0};
	struct spi_config *slot;
	struct spi_config config;
	struct token token;
	char *end;
	size_t used;
	unsigned long hz = 0;
	unsigned long bytes = 1;
	unsigned long count = 1;
	unsigned long gap_us = 0;
	unsigned long mismatches = 0;
	int rc = 0;

	if (!next_token(&cursor, &token)) {
		emit_error("test_spi", "no alias");
		return;
	}
	for (const struct spi_alias *a = spi_aliases; a->name != NULL; a++) {
		if (token_equals(&token, a->name)) {
			alias = a;
		}
	}
	if (alias == NULL) {
		emit_error("test_spi", "unknown alias");
		return;
	}
	hz = strtoul(cursor, &end, 10);
	if (*end != '\0') {
		bytes = strtoul(end, &end, 10);
	}
	if (*end != '\0') {
		count = strtoul(end, &end, 10);
	}
	if (*end != '\0') {
		gap_us = strtoul(end, &end, 10);
	}
	config = alias->spec.config;
	if (hz != 0) {
		config.frequency = (uint32_t)hz;
	}
	if (bytes == 0 || bytes > SPI_BYTES_MAX || count == 0) {
		rc = -EINVAL;
	}
	if (rc == 0 && !spi_is_ready_dt(&alias->spec)) {
		rc = -ENODEV;
	}
	slot = &spi_configs[spi_config_index ^= 1U];
	for (unsigned long i = 0; rc == 0 && i < count; i++) {
		const struct spi_buf tx_buf = {.buf = transmit, .len = bytes};
		const struct spi_buf rx_buf = {.buf = receive, .len = bytes};
		const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
		const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

		memset(receive, 0, sizeof(receive));
		if (i > 0 && gap_us > 0) {
			k_busy_wait((uint32_t)gap_us);
		}
		*slot = config;
		rc = spi_transceive(alias->spec.bus, slot, &tx, &rx);
		if (rc != 0) {
			break;
		}
		if (i == 0) {
			memcpy(first, receive, bytes);
		} else if (memcmp(first, receive, bytes) != 0) {
			mismatches++;
		}
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_spi\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
		mclw_debug_port_write(result_line);
		return;
	}
	used = append(result_line, RESULT_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_spi\",\"status\":\"ok\",\"hz\":%u,"
		"\"count\":%lu,\"mismatches\":%lu,\"frame\":\"",
		(unsigned int)config.frequency, count, mismatches);
	for (unsigned long i = 0; i < bytes; i++) {
		used = append(result_line, RESULT_BODY_MAX, used, "%02x", first[i]);
	}
	append(result_line, sizeof(result_line), used, "\"}\r\n");
	mclw_debug_port_write(result_line);
}
#else
static void emit_spi(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_spi\",\"status\":\"error\",\"detail\":\"no spi in this image\"}\r\n");
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

/*
 * `RAW <hex> [w]`: one chip-select window on the `raw` alias, a
 * vnd,spi-device node carrying the mode and clock of the part under test,
 * clocking the hex bytes out and reporting what came back. `w` sends a
 * write-only transfer, as drivers do for register writes.
 */
#if defined(CONFIG_SPI) && DT_NODE_EXISTS(DT_ALIAS(raw))
#define RAW_BYTES_MAX 32U

static const struct spi_dt_spec raw_spec = SPI_DT_SPEC_GET(DT_ALIAS(raw), SPI_OPERATION);

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static void emit_raw(const char *args)
{
	uint8_t transmit[RAW_BYTES_MAX] = {0};
	uint8_t receive[RAW_BYTES_MAX] = {0};
	char rx_hex[2 * RAW_BYTES_MAX + 1] = "";
	size_t bytes = 0;
	bool write_only = false;
	int rc = 0;

	if (args == NULL) {
		rc = -EINVAL;
	}
	while (rc == 0 && *args != '\0' && *args != ' ') {
		int hi = hex_nibble(args[0]);
		int lo = args[1] != '\0' ? hex_nibble(args[1]) : -1;

		if (hi < 0 || lo < 0 || bytes >= RAW_BYTES_MAX) {
			rc = -EINVAL;
			break;
		}
		transmit[bytes++] = (uint8_t)((hi << 4) | lo);
		args += 2;
	}
	if (rc == 0 && bytes == 0) {
		rc = -EINVAL;
	}
	if (rc == 0 && *args == ' ') {
		write_only = args[1] == 'w';
	}
	if (rc == 0 && !spi_is_ready_dt(&raw_spec)) {
		rc = -ENODEV;
	}
	if (rc == 0) {
		const struct spi_buf tx_buf = {.buf = transmit, .len = bytes};
		const struct spi_buf rx_buf = {.buf = receive, .len = bytes};
		const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
		const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

		rc = write_only ? spi_write_dt(&raw_spec, &tx) : spi_transceive_dt(&raw_spec, &tx, &rx);
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		for (size_t i = 0; i < bytes; i++) {
			snprintf(&rx_hex[2 * i], 3, "%02x", receive[i]);
		}
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"ok\",\"rx\":\"%s\"}\r\n", rx_hex);
	}
	mclw_debug_port_write(result_line);
}
#else
static void emit_raw(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"error\",\"detail\":\"no raw spi device in this image\"}\r\n");
	mclw_debug_port_write(result_line);
}
#endif

/*
 * `GNSS`: the last fix Zephyr's GNSS driver published for the `gnss` alias,
 * in the API's units, and how many it has published since boot. `GNSS RATE`,
 * `GNSS NAV` and `GNSS SYSTEMS` go to the driver's pull API, which polls the
 * receiver.
 */
#if defined(CONFIG_GNSS) && DT_NODE_EXISTS(DT_ALIAS(gnss))
static struct gnss_data gnss_last;
static unsigned long gnss_fixes;

static void gnss_data_cb(const struct device *dev, const struct gnss_data *data)
{
	(void)dev;
	gnss_last = *data;
	gnss_fixes++;
}
GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(DT_ALIAS(gnss)), gnss_data_cb);

static void emit_gnss(const char *args)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(gnss));
	int rc = device_is_ready(dev) ? 0 : -ENODEV;

	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"error\",\"errno\":%d,"
			"\"init_res\":%u}\r\n", -rc, (unsigned int)dev->state->init_res);
	} else if (args != NULL && strcmp(args, "RATE") == 0) {
		uint32_t rate = 0;

		rc = gnss_get_fix_rate(dev, &rate);
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"%s\",\"errno\":%d,"
			"\"fix_rate_ms\":%u}\r\n", rc == 0 ? "ok" : "error", -rc, (unsigned int)rate);
	} else if (args != NULL && strcmp(args, "NAV") == 0) {
		enum gnss_navigation_mode mode = 0;

		rc = gnss_get_navigation_mode(dev, &mode);
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"%s\",\"errno\":%d,"
			"\"navigation_mode\":%d}\r\n", rc == 0 ? "ok" : "error", -rc, (int)mode);
	} else if (args != NULL && strcmp(args, "SYSTEMS") == 0) {
		gnss_systems_t supported = 0;
		gnss_systems_t enabled = 0;

		rc = gnss_get_supported_systems(dev, &supported);
		if (rc == 0) {
			rc = gnss_get_enabled_systems(dev, &enabled);
		}
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"%s\",\"errno\":%d,"
			"\"supported\":%u,\"enabled\":%u}\r\n", rc == 0 ? "ok" : "error", -rc,
			(unsigned int)supported, (unsigned int)enabled);
	} else if (args != NULL) {
		rc = -EINVAL;
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		const struct gnss_data *g = &gnss_last;

		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"ok\",\"fixes\":%lu,"
			"\"fix_status\":%d,\"fix_quality\":%d,\"satellites\":%u,\"hdop\":%u,"
			"\"latitude_ndeg\":%lld,\"longitude_ndeg\":%lld,\"altitude_mm\":%ld,"
			"\"speed_mmps\":%lu,\"bearing_mdeg\":%lu,"
			"\"utc\":[%u,%u,%u,%u,%u,%u]}\r\n",
			gnss_fixes, (int)g->info.fix_status, (int)g->info.fix_quality,
			(unsigned int)g->info.satellites_cnt, (unsigned int)g->info.hdop,
			(long long)g->nav_data.latitude, (long long)g->nav_data.longitude,
			(long)g->nav_data.altitude, (unsigned long)g->nav_data.speed,
			(unsigned long)g->nav_data.bearing,
			(unsigned int)g->utc.century_year, (unsigned int)g->utc.month,
			(unsigned int)g->utc.month_day, (unsigned int)g->utc.hour,
			(unsigned int)g->utc.minute, (unsigned int)g->utc.millisecond);
	}
	mclw_debug_port_write(result_line);
}
#else
static void emit_gnss(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"error\",\"detail\":\"no gnss in this image\"}\r\n");
	mclw_debug_port_write(result_line);
}
#endif

static void dispatch(const char *command)
{
	if (strcmp(command, "PING") == 0) {
		emit_ping();
	} else if (strcmp(command, "ID") == 0) {
		emit_identity("test_id");
	} else if (strcmp(command, "SENSOR") == 0) {
		emit_sensor(NULL);
	} else if (strncmp(command, "SENSOR ", 7) == 0) {
		emit_sensor(command + 7);
	} else if (strcmp(command, "SPI") == 0) {
		emit_spi(NULL);
	} else if (strncmp(command, "SPI ", 4) == 0) {
		emit_spi(command + 4);
	} else if (strncmp(command, "RAW ", 4) == 0) {
		emit_raw(command + 4);
	} else if (strcmp(command, "GNSS") == 0) {
		emit_gnss(NULL);
	} else if (strncmp(command, "GNSS ", 5) == 0) {
		emit_gnss(command + 5);
	} else {
		emit_unknown();
	}
}

int main(void)
{
	if (boot_magic != 0x424f4f54U) {
		boot_magic = 0x424f4f54U;
		boot_count = 0;
	}
	boot_count++;
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
