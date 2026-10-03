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
#if defined(CONFIG_GPIO)
#include <zephyr/drivers/gpio.h>
#endif
#if defined(CONFIG_GNSS)
#include <zephyr/drivers/gnss.h>
#endif
#if defined(CONFIG_LORA)
#include <zephyr/drivers/lora.h>
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
#define COMMAND_MAX (MCLW_DUT_MAILBOX_COMMAND_CAPACITY + 1U)
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
	mclw_dut_mailbox_write(result_line);
}

static void emit_ping(void)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_ping\",\"status\":\"ok\",\"image\":\"%s\"}\r\n",
		IMAGE_NAME);
	mclw_dut_mailbox_write(result_line);
}

static void emit_unknown(void)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_command\",\"status\":\"error\","
		"\"detail\":\"unknown command\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}

#if defined(CONFIG_SENSOR) || defined(CONFIG_SPI) || defined(CONFIG_LORA)
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
	mclw_dut_mailbox_write(result_line);
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
		mclw_dut_mailbox_write(result_line);
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
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_sensor(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"detail\":\"no sensor in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
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
		mclw_dut_mailbox_write(result_line);
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
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_spi(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_spi\",\"status\":\"error\",\"detail\":\"no spi in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `LORA CONFIG [key=value]...`, `LORA SEND <hex>|pattern=<len>`,
 * `LORA RECV <timeout_ms>`, `LORA AIRTIME <len>`: the LoRa API on the
 * `lora0` alias. CONFIG keys are freq (Hz), bw (kHz), sf, cr (5..8 for
 * 4/5..4/8), power (dBm), preamble, tx, iq, public, crc (0 or 1); a key left
 * out keeps its last value. A pattern payload is byte i = i * 7 + 1, for
 * payloads longer than a command line holds.
 */
#if defined(CONFIG_LORA) && DT_NODE_EXISTS(DT_ALIAS(lora0))
#define LORA_PAYLOAD_MAX 255U
#define LORA_PRINT_MAX 200U

static const struct device *const lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
static struct lora_modem_config lora_config_now = {
	.frequency = 868100000,
	.bandwidth = BW_125_KHZ,
	.datarate = SF_7,
	.coding_rate = CR_4_5,
	.preamble_len = 8,
	.tx_power = 14,
	.tx = true,
};
static uint8_t lora_payload[LORA_PAYLOAD_MAX];

static int lora_hex_nibble(char c)
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

static bool lora_key(const struct token *token, const char *key, long *value)
{
	size_t length = strlen(key);
	char text[16];
	char *end;

	if (token->length <= length + 1U || token->length - length - 1U >= sizeof(text) ||
		strncmp(token->text, key, length) != 0 || token->text[length] != '=') {
		return false;
	}
	memcpy(text, &token->text[length + 1U], token->length - length - 1U);
	text[token->length - length - 1U] = '\0';
	*value = strtol(text, &end, 10);
	return *end == '\0';
}

static int lora_parse_config(const char *cursor)
{
	struct lora_modem_config next = lora_config_now;
	struct token token;
	long v;

	while (next_token(&cursor, &token)) {
		if (lora_key(&token, "freq", &v)) {
			next.frequency = (uint32_t)v;
		} else if (lora_key(&token, "bw", &v)) {
			next.bandwidth = (enum lora_signal_bandwidth)v;
		} else if (lora_key(&token, "sf", &v)) {
			next.datarate = (enum lora_datarate)v;
		} else if (lora_key(&token, "cr", &v) && v >= 5 && v <= 8) {
			next.coding_rate = (enum lora_coding_rate)(v - 4);
		} else if (lora_key(&token, "power", &v)) {
			next.tx_power = (int8_t)v;
		} else if (lora_key(&token, "preamble", &v)) {
			next.preamble_len = (uint16_t)v;
		} else if (lora_key(&token, "tx", &v)) {
			next.tx = v != 0;
		} else if (lora_key(&token, "iq", &v)) {
			next.iq_inverted = v != 0;
		} else if (lora_key(&token, "public", &v)) {
			next.public_network = v != 0;
		} else if (lora_key(&token, "crc", &v)) {
			next.packet_crc_disable = v == 0;
		} else {
			return -EINVAL;
		}
	}
	lora_config_now = next;
	return lora_config(lora_dev, &lora_config_now);
}

static int lora_parse_payload(const char *cursor, size_t *length)
{
	struct token token;
	long v;

	if (!next_token(&cursor, &token)) {
		return -EINVAL;
	}
	if (lora_key(&token, "pattern", &v)) {
		if (v < 1 || v > (long)LORA_PAYLOAD_MAX) {
			return -EINVAL;
		}
		for (long i = 0; i < v; i++) {
			lora_payload[i] = (uint8_t)(i * 7 + 1);
		}
		*length = (size_t)v;
		return 0;
	}
	if (token.length % 2U != 0U || token.length / 2U > LORA_PAYLOAD_MAX) {
		return -EINVAL;
	}
	for (size_t i = 0; i < token.length / 2U; i++) {
		int hi = lora_hex_nibble(token.text[2U * i]);
		int lo = lora_hex_nibble(token.text[2U * i + 1U]);

		if (hi < 0 || lo < 0) {
			return -EINVAL;
		}
		lora_payload[i] = (uint8_t)((hi << 4) | lo);
	}
	*length = token.length / 2U;
	return 0;
}

static void emit_lora(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	struct token verb;
	size_t used;
	int rc;

	if (!next_token(&cursor, &verb)) {
		emit_error("test_lora", "no verb");
		return;
	}
	if (!device_is_ready(lora_dev)) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"error\",\"errno\":%d,"
			"\"init_res\":%u}\r\n", ENODEV, (unsigned int)lora_dev->state->init_res);
		mclw_dut_mailbox_write(result_line);
		return;
	}
	if (token_equals(&verb, "CONFIG")) {
		rc = lora_parse_config(cursor);
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"%s\",\"errno\":%d,"
			"\"freq\":%u,\"bw\":%d,\"sf\":%d,\"cr\":%d,\"power\":%d,\"preamble\":%u,"
			"\"tx\":%d,\"iq\":%d,\"public\":%d,\"crc\":%d}\r\n",
			rc == 0 ? "ok" : "error", -rc, (unsigned int)lora_config_now.frequency,
			(int)lora_config_now.bandwidth, (int)lora_config_now.datarate,
			(int)lora_config_now.coding_rate + 4, (int)lora_config_now.tx_power,
			(unsigned int)lora_config_now.preamble_len, (int)lora_config_now.tx,
			(int)lora_config_now.iq_inverted, (int)lora_config_now.public_network,
			(int)!lora_config_now.packet_crc_disable);
	} else if (token_equals(&verb, "SEND")) {
		size_t length = 0;
		int64_t start;
		int64_t elapsed = 0;

		rc = lora_parse_payload(cursor, &length);
		if (rc == 0) {
			start = k_uptime_get();
			rc = lora_send(lora_dev, lora_payload, (uint32_t)length);
			elapsed = k_uptime_get() - start;
		}
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"%s\",\"errno\":%d,"
			"\"len\":%u,\"elapsed_ms\":%lld}\r\n", rc == 0 ? "ok" : "error", -rc,
			(unsigned int)length, (long long)elapsed);
	} else if (token_equals(&verb, "RECV")) {
		unsigned long timeout_ms = strtoul(cursor, NULL, 10);
		int16_t rssi = 0;
		int8_t snr = 0;

		rc = lora_recv(lora_dev, lora_payload, (uint8_t)LORA_PAYLOAD_MAX, K_MSEC(timeout_ms),
			&rssi, &snr);
		if (rc < 0) {
			snprintf(result_line, sizeof(result_line),
				"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"error\",\"errno\":%d}\r\n",
				-rc);
		} else {
			used = append(result_line, RESULT_BODY_MAX, 0,
				"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"ok\",\"len\":%d,"
				"\"rssi\":%d,\"snr\":%d,\"data\":\"", rc, (int)rssi, (int)snr);
			for (int i = 0; i < rc && i < (int)LORA_PRINT_MAX; i++) {
				used = append(result_line, RESULT_BODY_MAX, used, "%02x", lora_payload[i]);
			}
			append(result_line, sizeof(result_line), used, "\"}\r\n");
		}
	} else if (token_equals(&verb, "AIRTIME")) {
		unsigned long length = strtoul(cursor, NULL, 10);

		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"ok\",\"len\":%lu,"
			"\"airtime_ms\":%u}\r\n", length,
			(unsigned int)lora_airtime(lora_dev, (uint32_t)length));
	} else {
		emit_error("test_lora", "unknown verb");
		return;
	}
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_lora(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_lora\",\"status\":\"error\",\"detail\":\"no lora in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
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
 * `RAW <hex> [w] [hz] [n=<count>] [gap=<us>]`: one chip-select window on the
 * `raw` alias, a vnd,spi-device node carrying the mode and clock of the part
 * under test, clocking the hex bytes out and reporting what came back. `w`
 * sends a write-only transfer, as drivers do for register writes; `hz`
 * overrides the node's clock. With a count, the window repeats that many
 * times `gap` microseconds apart, the first reply is reported and
 * `mismatches` counts the later ones that differed.
 */
#if defined(CONFIG_SPI) && DT_NODE_EXISTS(DT_ALIAS(raw))
#define RAW_BYTES_MAX 32U

static const struct spi_dt_spec raw_spec = SPI_DT_SPEC_GET(DT_ALIAS(raw), SPI_OPERATION);
/* Alternated for the same reason as spi_configs. */
static struct spi_dt_spec raw_specs[2];
static unsigned int raw_spec_index;

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
	uint8_t first[RAW_BYTES_MAX] = {0};
	bool write_only = false;
	unsigned long count = 1;
	unsigned long gap_us = 0;
	unsigned long mismatches = 0;
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
	struct spi_dt_spec *spec = &raw_specs[raw_spec_index ^= 1U];

	*spec = raw_spec;
	while (rc == 0 && *args == ' ') {
		args++;
		if (*args == 'w') {
			write_only = true;
			args++;
		} else if (*args >= '0' && *args <= '9') {
			spec->config.frequency = (uint32_t)strtoul(args, (char **)&args, 10);
		} else if (strncmp(args, "n=", 2) == 0) {
			count = strtoul(args + 2, (char **)&args, 10);
		} else if (strncmp(args, "gap=", 4) == 0) {
			gap_us = strtoul(args + 4, (char **)&args, 10);
		} else {
			rc = -EINVAL;
		}
	}
	if (rc == 0 && !spi_is_ready_dt(spec)) {
		rc = -ENODEV;
	}
	if (rc == 0 && count == 0) {
		rc = -EINVAL;
	}
	for (unsigned long i = 0; rc == 0 && i < count; i++) {
		const struct spi_buf tx_buf = {.buf = transmit, .len = bytes};
		const struct spi_buf rx_buf = {.buf = receive, .len = bytes};
		const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
		const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

		if (i > 0 && gap_us > 0) {
			k_busy_wait((uint32_t)gap_us);
		}
		memset(receive, 0, sizeof(receive));
		rc = write_only ? spi_write_dt(spec, &tx) : spi_transceive_dt(spec, &tx, &rx);
		if (i == 0) {
			memcpy(first, receive, bytes);
		} else if (memcmp(first, receive, bytes) != 0) {
			mismatches++;
		}
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		for (size_t i = 0; i < bytes; i++) {
			snprintf(&rx_hex[2 * i], 3, "%02x", first[i]);
		}
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"ok\",\"rx\":\"%s\","
			"\"count\":%lu,\"mismatches\":%lu}\r\n", rx_hex, count, mismatches);
	}
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_raw(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_raw\",\"status\":\"error\",\"detail\":\"no raw spi device in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `PULSE <us>`: holds the `reset` alias's line active for `us` microseconds,
 * as a host resets a part through its reset pin. The image drives the line
 * inactive from boot.
 */
#if defined(CONFIG_GPIO) && DT_NODE_EXISTS(DT_ALIAS(reset))
static const struct gpio_dt_spec pulse_line = GPIO_DT_SPEC_GET(DT_ALIAS(reset), gpios);

static void pulse_init(void)
{
	(void)gpio_pin_configure_dt(&pulse_line, GPIO_OUTPUT_INACTIVE);
}

static void emit_pulse(const char *args)
{
	char *end;
	unsigned long us = strtoul(args, &end, 10);
	int rc = end == args ? -EINVAL : gpio_pin_set_dt(&pulse_line, 1);

	if (rc == 0) {
		k_busy_wait((uint32_t)us);
		rc = gpio_pin_set_dt(&pulse_line, 0);
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_pulse\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_pulse\",\"status\":\"ok\",\"us\":%lu}\r\n", us);
	}
	mclw_dut_mailbox_write(result_line);
}
#else
static void pulse_init(void)
{
}

static void emit_pulse(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_pulse\",\"status\":\"error\",\"detail\":\"no reset line in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
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
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_gnss(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_gnss\",\"status\":\"error\",\"detail\":\"no gnss in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
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
	} else if (strncmp(command, "PULSE ", 6) == 0) {
		emit_pulse(command + 6);
	} else if (strncmp(command, "LORA ", 5) == 0) {
		emit_lora(command + 5);
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
	mclw_dut_mailbox_init();
	pulse_init();
	emit_identity("test_boot");
	while (true) {
		if (mclw_dut_mailbox_poll(command_line, sizeof(command_line))) {
			trim(command_line);
			dispatch(command_line);
		}
		k_msleep(POLL_INTERVAL_MS);
	}
}
