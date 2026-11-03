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
#if defined(CONFIG_NET_SOCKETS)
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#if !defined(CONFIG_CRC)
#error "the socket commands report CRC-32s: set CONFIG_CRC=y"
#endif
#endif
#if defined(CONFIG_CRC)
#include <zephyr/sys/crc.h>
#endif
#if defined(CONFIG_MODEM_SIM7080)
#include <zephyr/drivers/modem/simcom-sim7080.h>
#endif

/*
 * The commands below the socket API, each compiled in only when the image
 * has what it calls: `UART` a `modem-uart` alias on an interrupt-driven
 * UART, `GPIO` a pin under /zephyr,user, `CELLULAR` a `modem` alias on
 * modem_cellular, `NET` the connection manager's L4 events.
 */
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && defined(CONFIG_RING_BUFFER) && \
	DT_NODE_EXISTS(DT_ALIAS(modem_uart))
#define DUT_UART 1
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#if !defined(CONFIG_CRC)
#error "UART RECV reports CRC-32s: set CONFIG_CRC=y"
#endif
#else
#define DUT_UART 0
#endif
#if defined(CONFIG_GPIO) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwrkey_gpios)
#define DUT_GPIO 1
#include <zephyr/drivers/gpio.h>
#else
#define DUT_GPIO 0
#endif
#if defined(CONFIG_MODEM_SIM7080) && defined(CONFIG_NET_SOCKETS) && DT_NODE_EXISTS(DT_ALIAS(modem))
#define DUT_MODEM 1
#else
#define DUT_MODEM 0
#endif
#if defined(CONFIG_MODEM_CELLULAR) && DT_NODE_EXISTS(DT_ALIAS(modem))
#define DUT_CELLULAR 1
#include <zephyr/drivers/cellular.h>
#else
#define DUT_CELLULAR 0
#endif
#if defined(CONFIG_NET_CONNECTION_MANAGER) && defined(CONFIG_NET_MGMT_EVENT)
#define DUT_NET 1
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#if defined(CONFIG_NET_L2_PPP_MGMT)
#include <zephyr/net/ppp.h>
#endif
#if defined(CONFIG_PM_DEVICE) && DT_NODE_EXISTS(DT_ALIAS(modem))
#include <zephyr/pm/device.h>
#define DUT_NET_PM 1
#else
#define DUT_NET_PM 0
#endif
#else
#define DUT_NET 0
#endif

#if defined(CONFIG_NET_SOCKETS) || DUT_UART || DUT_GPIO || DUT_CELLULAR || DUT_NET
#define DUT_LINES 1
#else
#define DUT_LINES 0
#endif
#if defined(CONFIG_SENSOR) || defined(CONFIG_SPI) || defined(CONFIG_LORA) || DUT_LINES
#define DUT_TOKENS 1
#else
#define DUT_TOKENS 0
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

#if DUT_TOKENS
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

static inline int hex_nibble(char c)
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
#endif

#if DUT_LINES
/*
 * Lines whose length depends on what a peer sent are built against the
 * mailbox's capacity, not the local buffer: a longer line would be cut
 * before its `\n` and never parse.
 */
#define LINE_BODY_MAX (MCLW_DUT_MAILBOX_RESULT_CAPACITY - 3U)
#define TOKEN_TEXT_MAX 64U

static void finish_line(size_t used)
{
	append(result_line, sizeof(result_line), used, "}\r\n");
	mclw_dut_mailbox_write(result_line);
}

/* One byte as it appears inside a JSON string; returns its length. */
static size_t json_unit(unsigned char c, char unit[7])
{
	if (c == '"' || c == '\\') {
		unit[0] = '\\';
		unit[1] = (char)c;
		return 2;
	}
	if (c == '\r' || c == '\n' || c == '\t') {
		unit[0] = '\\';
		unit[1] = c == '\r' ? 'r' : (c == '\n' ? 'n' : 't');
		return 2;
	}
	if (c < 0x20U || c >= 0x7fU) {
		return (size_t)snprintf(unit, 7, "\\u%04x", c);
	}
	unit[0] = (char)c;
	return 1;
}

/* A JSON string; bytes outside printable ASCII escaped, whole or not at all. */
static size_t append_json_text(char *text, size_t limit, size_t used, const char *s, size_t length)
{
	if (used + 2U > limit) {
		return used;
	}
	text[used++] = '"';
	for (size_t i = 0; i < length; i++) {
		char unit[7];
		size_t n = json_unit((unsigned char)s[i], unit);

		if (used + n + 1U > limit) {
			break;
		}
		memcpy(&text[used], unit, n);
		used += n;
	}
	text[used++] = '"';
	text[used] = '\0';
	return used;
}

static size_t append_hex(char *text, size_t limit, size_t used, const uint8_t *bytes, size_t count)
{
	used = append(text, limit, used, "\"");
	for (size_t i = 0; i < count && used + 3U < limit; i++) {
		used = append(text, limit, used, "%02x", bytes[i]);
	}
	return append(text, limit, used, "\"");
}

#if defined(CONFIG_NET_SOCKETS) || DUT_CELLULAR
static bool token_copy(const struct token *token, char *out, size_t size)
{
	if (token->length == 0U || token->length >= size) {
		return false;
	}
	memcpy(out, token->text, token->length);
	out[token->length] = '\0';
	return true;
}
#endif

static bool token_number(const struct token *token, const char *prefix, unsigned long *value)
{
	size_t skip = strlen(prefix);
	unsigned long result = 0;

	if (token->length <= skip || strncmp(token->text, prefix, skip) != 0) {
		return false;
	}
	for (size_t i = skip; i < token->length; i++) {
		if (token->text[i] < '0' || token->text[i] > '9') {
			return false;
		}
		result = result * 10U + (unsigned long)(token->text[i] - '0');
	}
	*value = result;
	return true;
}

#if DUT_UART || DUT_GPIO || DUT_CELLULAR || DUT_MODEM
/*
 * The device a command names is deferred or failed its init: `errno` 19,
 * `initialized` and the driver's `init_res`.
 */
static void emit_not_ready(const char *test, const struct device *dev)
{
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"%s\",\"status\":\"error\",\"errno\":%d,"
		"\"initialized\":%s,\"init_res\":%u}\r\n", test, ENODEV,
		dev->state->initialized ? "true" : "false", (unsigned int)dev->state->init_res);
	mclw_dut_mailbox_write(result_line);
}
#endif
#endif

#if defined(CONFIG_NET_SOCKETS) || DUT_UART
/*
 * `len=N` is a printable pattern, byte `i` being `0x21 + (7 i mod 94)`: the
 * SIM7080 offload driver cannot take a +CARECV chunk whose data starts with
 * CR or LF, and chunk boundaries are the modem's choice. Anything else is
 * the bytes as hex.
 */
static int payload_parse(const struct token *token, uint8_t *out, size_t size, size_t *length)
{
	unsigned long n;

	if (token_number(token, "len=", &n)) {
		if (n == 0U || n > size) {
			return -EINVAL;
		}
		for (size_t i = 0; i < n; i++) {
			out[i] = (uint8_t)(0x21U + (i * 7U) % 94U);
		}
		*length = n;
		return 0;
	}
	if (token->length == 0U || token->length % 2U != 0U || token->length / 2U > size) {
		return -EINVAL;
	}
	for (size_t i = 0; i < token->length / 2U; i++) {
		int hi = hex_nibble(token->text[2U * i]);
		int lo = hex_nibble(token->text[2U * i + 1U]);

		if (hi < 0 || lo < 0) {
			return -EINVAL;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	*length = token->length / 2U;
	return 0;
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

/*
 * `DNS`, `TCP`, `UDP`, `HTTP` and `SOCK`: the socket API on whatever stack
 * the image has, native or offloaded. Every wait is a poll() with a
 * deadline, because an offloading driver may ignore SO_RCVTIMEO.
 */
#if defined(CONFIG_NET_SOCKETS)
#define NET_PAYLOAD_MAX 1024U
#define NET_HEAD_BYTES 16U
#define NET_RECV_TIMEOUT_MS 5000
#define HTTP_TIMEOUT_MS 20000
#define HTTP_HEADER_MAX 384U
#define HTTP_STATUS_LINE_MAX 64U

static uint8_t net_tx[NET_PAYLOAD_MAX];
static uint8_t net_rx[NET_PAYLOAD_MAX];
static char http_header[HTTP_HEADER_MAX + 1U];

/*
 * An offloaded interface comes up only once its driver's init has run; a
 * socket call before that reaches a driver whose semaphores and socket table
 * were never initialised.
 */
static bool net_ready(const char *test)
{
	if (!net_if_is_up(net_if_get_default())) {
		emit_error(test, "network interface down");
		return false;
	}
	return true;
}

/* Offloading drivers return either -1 with errno or a negative errno itself. */
static int net_error(ssize_t rc)
{
	if (rc == -1) {
		return errno != 0 ? -errno : -EIO;
	}
	return (int)rc;
}

static int net_resolve(const char *host, int type, uint16_t port, struct net_sockaddr_in *out)
{
	struct zsock_addrinfo hints = {.ai_family = NET_AF_INET, .ai_socktype = type};
	struct zsock_addrinfo *res = NULL;
	int rc = zsock_getaddrinfo(host, NULL, &hints, &res);

	if (rc != 0) {
		return rc;
	}
	if (res == NULL || res->ai_addr == NULL) {
		return DNS_EAI_FAIL;
	}
	memcpy(out, res->ai_addr, sizeof(*out));
	zsock_freeaddrinfo(res);
	out->sin_family = NET_AF_INET;
	out->sin_port = net_htons(port);
	return 0;
}

struct net_session {
	const char *stage;
	int error;
	int fd;
	int64_t connect_ms;
};

static bool net_open(struct net_session *s, const char *host, uint16_t port, bool tcp)
{
	struct net_sockaddr_in addr;
	int64_t start;
	int rc;

	s->fd = -1;
	s->stage = "resolve";
	rc = net_resolve(host, tcp ? NET_SOCK_STREAM : NET_SOCK_DGRAM, port, &addr);
	if (rc != 0) {
		s->error = rc;
		return false;
	}
	s->stage = "socket";
	s->fd = zsock_socket(NET_AF_INET, tcp ? NET_SOCK_STREAM : NET_SOCK_DGRAM,
		tcp ? NET_IPPROTO_TCP : NET_IPPROTO_UDP);
	if (s->fd < 0) {
		s->error = net_error(s->fd);
		s->fd = -1;
		return false;
	}
	s->stage = "connect";
	start = k_uptime_get();
	rc = zsock_connect(s->fd, (struct net_sockaddr *)&addr, sizeof(addr));
	s->connect_ms = k_uptime_get() - start;
	if (rc != 0) {
		s->error = net_error(rc);
		return false;
	}
	s->error = 0;
	return true;
}

static int net_send_all(int fd, const uint8_t *bytes, size_t length)
{
	size_t sent = 0;

	while (sent < length) {
		ssize_t n = zsock_send(fd, &bytes[sent], length - sent, 0);

		if (n <= 0) {
			return n < 0 ? net_error(n) : -EIO;
		}
		sent += (size_t)n;
	}
	return 0;
}

/*
 * Whatever is waiting, after waiting until `deadline` at most: 0 with *n of
 * 0 on timeout, 1 when the peer closed, a negative errno on an error.
 */
static int net_read(int fd, uint8_t *buf, size_t size, int64_t deadline, size_t *n)
{
	*n = 0;
	while (true) {
		struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
		int64_t left = deadline - k_uptime_get();
		ssize_t got;
		int rc;

		if (left <= 0) {
			return 0;
		}
		rc = zsock_poll(&pfd, 1, (int)left);
		if (rc < 0) {
			return net_error(rc);
		}
		if (rc == 0) {
			k_msleep(1);
			continue;
		}
		got = zsock_recv(fd, buf, size, ZSOCK_MSG_DONTWAIT);
		if (got == 0) {
			return 1;
		}
		if (got < 0) {
			rc = net_error(got);
			if (rc == -EAGAIN) {
				continue;
			}
			return rc;
		}
		*n = (size_t)got;
		return 0;
	}
}

static int net_receive(int fd, uint8_t *buf, size_t want, int timeout_ms, size_t *got)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	*got = 0;
	while (*got < want) {
		size_t n;
		int rc = net_read(fd, &buf[*got], want - *got, deadline, &n);

		if (rc != 0) {
			return rc;
		}
		if (n == 0U) {
			break;
		}
		*got += n;
	}
	return 0;
}

static bool net_host_port(const char **cursor, char *host, unsigned long *port)
{
	struct token token;
	char *end;
	char digits[8];

	if (!next_token(cursor, &token) || !token_copy(&token, host, TOKEN_TEXT_MAX)) {
		return false;
	}
	if (!next_token(cursor, &token) || !token_copy(&token, digits, sizeof(digits))) {
		return false;
	}
	*port = strtoul(digits, &end, 10);
	return *end == '\0' && *port > 0U && *port <= 0xffffU;
}

static size_t net_head(const char *test, const struct net_session *s, bool ok)
{
	return append(result_line, LINE_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"%s\",\"status\":\"%s\",\"stage\":\"%s\",\"errno\":%d,"
		"\"fd\":%d,\"connect_ms\":%lld", test, ok ? "ok" : "error", s->stage, -s->error,
		s->fd, (long long)s->connect_ms);
}

/* `DNS <host>`: the IPv4 address the resolver (or its offload) returned. */
static void emit_dns(const char *cursor)
{
	struct net_sockaddr_in addr;
	struct token token;
	char host[TOKEN_TEXT_MAX];
	char ip[NET_INET_ADDRSTRLEN] = "";
	int64_t start;
	int rc;

	if (!next_token(&cursor, &token) || !token_copy(&token, host, sizeof(host))) {
		emit_error("test_dns", "usage: DNS <host>");
		return;
	}
	if (!net_ready("test_dns")) {
		return;
	}
	start = k_uptime_get();
	rc = net_resolve(host, NET_SOCK_STREAM, 0, &addr);
	if (rc == 0) {
		zsock_inet_ntop(NET_AF_INET, &addr.sin_addr, ip, sizeof(ip));
	}
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_dns\",\"status\":\"%s\",\"rc\":%d,\"ip\":\"%s\","
		"\"ms\":%lld}\r\n", rc == 0 ? "ok" : "error", rc, ip,
		(long long)(k_uptime_get() - start));
	mclw_dut_mailbox_write(result_line);
}

/*
 * `TCP|UDP <host> <port> <hex|len=N> [count=N] [chunk=N]`: connect, then
 * `count` rounds of sending the payload `chunk` bytes at a time and reading
 * each chunk's echo back before the next.
 */
static void emit_echo(const char *test, bool tcp, const char *cursor)
{
	struct net_session s = {.stage = "args", .fd = -1};
	struct token token;
	char host[TOKEN_TEXT_MAX];
	unsigned long port = 0;
	unsigned long count = 1;
	unsigned long chunk = NET_PAYLOAD_MAX;
	unsigned long value;
	unsigned long mismatches = 0;
	unsigned long rounds = 0;
	size_t length = 0;
	size_t sent = 0;
	size_t received = 0;
	size_t last = 0;
	uint32_t crc = 0;
	int64_t start = k_uptime_get();
	int64_t round_ms = 0;
	size_t used;
	bool ok = false;

	if (!net_host_port(&cursor, host, &port) || !next_token(&cursor, &token) ||
		payload_parse(&token, net_tx, sizeof(net_tx), &length) != 0) {
		emit_error(test, "usage: <host> <port> <hex|len=N> [count=N] [chunk=N]");
		return;
	}
	while (next_token(&cursor, &token)) {
		if (token_number(&token, "count=", &value) && value > 0U) {
			count = value;
		} else if (token_number(&token, "chunk=", &value) && value > 0U) {
			chunk = value;
		} else {
			emit_error(test, "bad option");
			return;
		}
	}
	if (!net_ready(test)) {
		return;
	}
	if (net_open(&s, host, (uint16_t)port, tcp)) {
		s.stage = "done";
		for (rounds = 0; rounds < count && s.error == 0; rounds++) {
			int64_t round_start = k_uptime_get();

			memset(net_rx, 0, length);
			last = 0;
			for (size_t at = 0; at < length && s.error == 0; at += chunk) {
				size_t n = MIN(chunk, length - at);
				size_t got = 0;

				s.error = net_send_all(s.fd, &net_tx[at], n);
				if (s.error != 0) {
					s.stage = "send";
					break;
				}
				sent += n;
				s.error = net_receive(s.fd, &net_rx[at], n, NET_RECV_TIMEOUT_MS, &got);
				received += got;
				last += got;
				if (s.error == 0 && got < n) {
					s.error = -ETIMEDOUT;
				}
				if (s.error != 0) {
					s.stage = "recv";
				}
			}
			if (memcmp(net_tx, net_rx, length) != 0) {
				mismatches++;
			}
			round_ms = MAX(round_ms, k_uptime_get() - round_start);
		}
		ok = s.error == 0;
		crc = crc32_ieee(net_rx, last);
	}
	if (s.fd >= 0) {
		(void)zsock_close(s.fd);
	}
	used = net_head(test, &s, ok);
	used = append(result_line, LINE_BODY_MAX, used,
		",\"length\":%u,\"rounds\":%lu,\"sent\":%u,\"received\":%u,\"mismatches\":%lu,"
		"\"crc32\":%u,\"round_ms\":%lld,\"total_ms\":%lld,\"head\":",
		(unsigned int)length, rounds, (unsigned int)sent, (unsigned int)received, mismatches,
		(unsigned int)crc, (long long)round_ms, (long long)(k_uptime_get() - start));
	used = append_hex(result_line, LINE_BODY_MAX, used, net_rx, MIN(last, NET_HEAD_BYTES));
	finish_line(used);
}

static long http_content_length(const char *header)
{
	static const char name[] = "\ncontent-length:";

	for (const char *p = header; *p != '\0'; p++) {
		size_t i = 0;

		while (name[i] != '\0' && p[i] != '\0' &&
			(p[i] == name[i] || (p[i] >= 'A' && p[i] <= 'Z' && p[i] + 32 == name[i]))) {
			i++;
		}
		if (name[i] == '\0') {
			return strtol(&p[i], NULL, 10);
		}
	}
	return -1;
}

/*
 * `HTTP <host> <port> <path>`: one GET with `Connection: close`, read until
 * Content-Length is satisfied, the peer closes, or the deadline.
 */
static void emit_http(const char *cursor)
{
	struct net_session s = {.stage = "args", .fd = -1};
	struct token token;
	char host[TOKEN_TEXT_MAX];
	char path[TOKEN_TEXT_MAX];
	unsigned long port = 0;
	size_t header_length = 0;
	size_t stored = 0;
	size_t body_length = 0;
	size_t status_length = 0;
	long content_length = -1;
	long status_code = -1;
	unsigned int terminator = 0;
	uint32_t crc = 0;
	int64_t start = k_uptime_get();
	int64_t deadline = start + HTTP_TIMEOUT_MS;
	bool closed = false;
	bool parsed = false;
	size_t used;
	int request;

	if (!net_host_port(&cursor, host, &port) || !next_token(&cursor, &token) ||
		!token_copy(&token, path, sizeof(path))) {
		emit_error("test_http", "usage: HTTP <host> <port> <path>");
		return;
	}
	if (!net_ready("test_http")) {
		return;
	}
	http_header[0] = '\0';
	request = snprintf((char *)net_tx, sizeof(net_tx),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	if (net_open(&s, host, (uint16_t)port, true)) {
		s.stage = "send";
		s.error = net_send_all(s.fd, net_tx, (size_t)request);
	}
	if (s.fd >= 0 && s.error == 0) {
		s.stage = "recv";
	}
	while (s.fd >= 0 && s.error == 0) {
		size_t n;
		size_t at = 0;
		int rc = net_read(s.fd, net_rx, sizeof(net_rx), deadline, &n);

		if (rc < 0) {
			s.error = rc;
			break;
		}
		if (rc > 0) {
			closed = true;
			break;
		}
		if (n == 0U) {
			s.error = -ETIMEDOUT;
			break;
		}
		while (at < n && terminator < 4U) {
			char c = (char)net_rx[at++];

			if (stored < HTTP_HEADER_MAX) {
				http_header[stored++] = c;
			}
			header_length++;
			terminator = c == "\r\n\r\n"[terminator] ? terminator + 1U : (c == '\r' ? 1U : 0U);
		}
		if (terminator == 4U && !parsed) {
			const char *space = strchr(http_header, ' ');

			parsed = true;
			http_header[stored] = '\0';
			status_length = strcspn(http_header, "\r");
			if (strncmp(http_header, "HTTP/", 5) == 0 && space != NULL) {
				status_code = strtol(space, NULL, 10);
			}
			content_length = http_content_length(http_header);
		}
		if (at < n) {
			crc = crc32_ieee_update(crc, &net_rx[at], n - at);
			body_length += n - at;
		}
		if (terminator == 4U && content_length >= 0 && body_length >= (size_t)content_length) {
			s.stage = "done";
			break;
		}
	}
	if (s.fd >= 0) {
		(void)zsock_close(s.fd);
	}
	if (closed && terminator == 4U) {
		s.stage = "done";
	}
	used = net_head("test_http", &s, strcmp(s.stage, "done") == 0);
	used = append(result_line, LINE_BODY_MAX, used,
		",\"code\":%ld,\"header_length\":%u,\"content_length\":%ld,\"body_length\":%u,"
		"\"body_crc32\":%u,\"closed\":%s,\"ms\":%lld,\"status_line\":",
		status_code, (unsigned int)header_length, content_length, (unsigned int)body_length,
		(unsigned int)crc, closed ? "true" : "false", (long long)(k_uptime_get() - start));
	used = append_json_text(result_line, LINE_BODY_MAX, used, http_header,
		MIN(status_length, HTTP_STATUS_LINE_MAX));
	finish_line(used);
}

/*
 * `SOCK OPEN <tcp|udp> <host> <port>`, `SOCK SEND <fd> <hex|len=N>`,
 * `SOCK RECV <fd> <max> [wait_ms]`, `SOCK CLOSE <fd>`: one socket held open
 * across commands, for what a single round trip cannot show.
 */
static void emit_sock(const char *cursor)
{
	struct net_session s = {.stage = "args", .fd = -1};
	struct token op;
	struct token token;
	unsigned long value = 0;
	size_t used;
	char *end;
	char digits[12];

	if (!next_token(&cursor, &op)) {
		emit_error("test_sock", "usage: SOCK OPEN|SEND|RECV|CLOSE ...");
		return;
	}
	if (!net_ready("test_sock")) {
		return;
	}
	if (token_equals(&op, "OPEN")) {
		char host[TOKEN_TEXT_MAX];
		bool tcp;

		if (!next_token(&cursor, &token) ||
			!(token_equals(&token, "tcp") || token_equals(&token, "udp")) ||
			!net_host_port(&cursor, host, &value)) {
			emit_error("test_sock", "usage: SOCK OPEN <tcp|udp> <host> <port>");
			return;
		}
		tcp = token_equals(&token, "tcp");
		if (net_open(&s, host, (uint16_t)value, tcp)) {
			s.stage = "done";
		} else if (s.fd >= 0) {
			(void)zsock_close(s.fd);
			s.fd = -1;
		}
		finish_line(net_head("test_sock", &s, s.error == 0));
		return;
	}
	if (!next_token(&cursor, &token) || !token_copy(&token, digits, sizeof(digits))) {
		emit_error("test_sock", "no fd");
		return;
	}
	s.fd = (int)strtol(digits, &end, 10);
	if (*end != '\0' || s.fd < 0) {
		emit_error("test_sock", "bad fd");
		return;
	}
	s.error = 0;
	if (token_equals(&op, "SEND")) {
		size_t length = 0;

		if (!next_token(&cursor, &token) ||
			payload_parse(&token, net_tx, sizeof(net_tx), &length) != 0) {
			emit_error("test_sock", "usage: SOCK SEND <fd> <hex|len=N>");
			return;
		}
		s.stage = "send";
		s.error = net_send_all(s.fd, net_tx, length);
		used = net_head("test_sock", &s, s.error == 0);
		finish_line(append(result_line, LINE_BODY_MAX, used, ",\"sent\":%u",
			s.error == 0 ? (unsigned int)length : 0U));
	} else if (token_equals(&op, "RECV")) {
		unsigned long wait_ms = NET_RECV_TIMEOUT_MS;
		size_t got = 0;
		int rc;

		if (!next_token(&cursor, &token) || !token_number(&token, "", &value) ||
			value == 0U || value > NET_PAYLOAD_MAX) {
			emit_error("test_sock", "usage: SOCK RECV <fd> <max> [wait_ms]");
			return;
		}
		if (next_token(&cursor, &token) && !token_number(&token, "", &wait_ms)) {
			emit_error("test_sock", "bad wait");
			return;
		}
		s.stage = "recv";
		rc = net_receive(s.fd, net_rx, value, (int)wait_ms, &got);
		s.error = rc < 0 ? rc : 0;
		used = net_head("test_sock", &s, s.error == 0);
		used = append(result_line, LINE_BODY_MAX, used,
			",\"received\":%u,\"closed\":%s,\"crc32\":%u,\"head\":", (unsigned int)got,
			rc > 0 ? "true" : "false", (unsigned int)crc32_ieee(net_rx, got));
		finish_line(append_hex(result_line, LINE_BODY_MAX, used, net_rx,
			MIN(got, NET_HEAD_BYTES)));
	} else if (token_equals(&op, "CLOSE")) {
		s.stage = "close";
		s.error = net_error(zsock_close(s.fd));
		finish_line(net_head("test_sock", &s, s.error == 0));
	} else {
		emit_error("test_sock", "unknown operation");
	}
}

static void emit_net(const char *command)
{
	const char *cursor = command;
	struct token verb;

	(void)next_token(&cursor, &verb);
	if (token_equals(&verb, "DNS")) {
		emit_dns(cursor);
	} else if (token_equals(&verb, "TCP")) {
		emit_echo("test_tcp", true, cursor);
	} else if (token_equals(&verb, "UDP")) {
		emit_echo("test_udp", false, cursor);
	} else if (token_equals(&verb, "HTTP")) {
		emit_http(cursor);
	} else {
		emit_sock(cursor);
	}
}
#else
static void emit_net(const char *command)
{
	(void)command;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_net\",\"status\":\"error\",\"detail\":\"no sockets in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `MODEM ...`: the SIM7080 offload driver's vendor API
 * (zephyr/drivers/modem/simcom-sim7080.h) on the `modem` alias. The node is
 * zephyr,deferred-init, so `MODEM INIT` runs the driver's init, which powers
 * the part through PWRKEY, identifies it and activates PDP before returning.
 */
#if DUT_MODEM
#define MODEM_SMS_SLOTS 4U
#define MODEM_FTP_CHUNK 256U
#define MODEM_FTP_TIMEOUT_MS 30000

static const struct device *const modem_dev = DEVICE_DT_GET(DT_ALIAS(modem));
static struct sim7080_sms modem_sms[MODEM_SMS_SLOTS];
static char modem_ftp_chunk[MODEM_FTP_CHUNK];

static size_t modem_head(const char *op, int rc, int64_t start)
{
	return append(result_line, LINE_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_modem\",\"status\":\"%s\",\"op\":\"%s\",\"rc\":%d,"
		"\"state\":%d,\"ms\":%lld", rc >= 0 ? "ok" : "error", op, rc,
		(int)mdm_sim7080_get_state(), (long long)(k_uptime_get() - start));
}

static size_t modem_text(size_t used, const char *name, const char *value)
{
	used = append(result_line, LINE_BODY_MAX, used, ",\"%s\":", name);
	return append_json_text(result_line, LINE_BODY_MAX, used, value != NULL ? value : "",
		value != NULL ? strlen(value) : 0U);
}

/*
 * The net stack's own bring-up at boot found the deferred device not ready
 * and the offloaded L2 refuses net_if_up() then (subsys/net/ip/net_if.c), so
 * the interface is brought up here once the device is.
 */
static void modem_init_command(int64_t start)
{
	struct net_if *iface = net_if_lookup_by_dev(modem_dev);
	int rc = device_init(modem_dev);
	int up = -ENODEV;

	if (rc == 0 && iface != NULL) {
		up = net_if_up(iface);
	}
	finish_line(append(result_line, LINE_BODY_MAX, modem_head("init", rc, start),
		",\"init_res\":%u,\"if_up\":%d", (unsigned int)modem_dev->state->init_res, up));
}

static void modem_sms_command(const char *cursor, int64_t start)
{
	struct sim7080_sms_buffer buffer = {.sms = modem_sms, .nsms = MODEM_SMS_SLOTS};
	struct token token = {.text = "", .length = 0};
	unsigned long value = 0;
	size_t used;
	int rc;

	if (next_token(&cursor, &token) && token_equals(&token, "DELETE")) {
		if (!next_token(&cursor, &token) || !token_number(&token, "", &value) || value > 0xffffU) {
			emit_error("test_modem", "usage: MODEM SMS DELETE <index>");
			return;
		}
		rc = mdm_sim7080_delete_sms((uint16_t)value);
		finish_line(append(result_line, LINE_BODY_MAX, modem_head("sms_delete", rc, start),
			",\"index\":%lu", value));
		return;
	}
	if (token.length > 0U && !token_equals(&token, "READ")) {
		emit_error("test_modem", "usage: MODEM SMS [READ [k]|DELETE <index>]");
		return;
	}
	if (next_token(&cursor, &token) && !token_number(&token, "", &value)) {
		emit_error("test_modem", "bad position");
		return;
	}
	memset(modem_sms, 0, sizeof(modem_sms));
	rc = mdm_sim7080_read_sms(&buffer);
	used = append(result_line, LINE_BODY_MAX, modem_head("sms_read", rc, start),
		",\"count\":%d,\"indices\":[", rc);
	for (int i = 0; i < rc && i < (int)MODEM_SMS_SLOTS; i++) {
		used = append(result_line, LINE_BODY_MAX, used, "%s%u", i == 0 ? "" : ",",
			(unsigned int)modem_sms[i].index);
	}
	used = append(result_line, LINE_BODY_MAX, used, "]");
	if (rc > 0 && value < (unsigned long)rc && value < MODEM_SMS_SLOTS) {
		const struct sim7080_sms *m = &modem_sms[value];

		used = append(result_line, LINE_BODY_MAX, used,
			",\"position\":%lu,\"index\":%u,\"stat\":%d,\"first_octet\":%u,\"pid\":%u,"
			"\"time\":[%u,%u,%u,%u,%u,%u,%u],\"length\":%u,\"text\":", value,
			(unsigned int)m->index, (int)m->stat, m->first_octet, m->tp_pid, m->time.year,
			m->time.month, m->time.day, m->time.hour, m->time.minute, m->time.second,
			m->time.timezone, m->data_len);
		used = append_json_text(result_line, LINE_BODY_MAX, used, m->data,
			strnlen(m->data, sizeof(m->data)));
	}
	finish_line(used);
}

static void modem_ftp_command(const char *cursor, int64_t start)
{
	char server[TOKEN_TEXT_MAX];
	char path[TOKEN_TEXT_MAX];
	char file[TOKEN_TEXT_MAX];
	char user[TOKEN_TEXT_MAX] = "anonymous";
	char password[TOKEN_TEXT_MAX] = "";
	const char *result = "error";
	struct token token;
	unsigned long reads = 0;
	size_t bytes = 0;
	uint32_t crc = 0;
	int64_t deadline;
	int rc;

	if (!next_token(&cursor, &token) || !token_copy(&token, server, sizeof(server)) ||
		!next_token(&cursor, &token) || !token_copy(&token, path, sizeof(path)) ||
		!next_token(&cursor, &token) || !token_copy(&token, file, sizeof(file)) ||
		(next_token(&cursor, &token) && !token_copy(&token, user, sizeof(user))) ||
		(next_token(&cursor, &token) && !token_copy(&token, password, sizeof(password)))) {
		emit_error("test_modem", "usage: MODEM FTP <server> <path> <file> [user] [password]");
		return;
	}
	rc = mdm_sim7080_ftp_get_start(server, user, password, file, path);
	deadline = k_uptime_get() + MODEM_FTP_TIMEOUT_MS;
	while (rc == 0) {
		size_t size = sizeof(modem_ftp_chunk);
		int read = mdm_sim7080_ftp_get_read(modem_ftp_chunk, &size);

		if (read == SIM7080_FTP_RC_FINISHED) {
			result = "finished";
			break;
		}
		if (read != SIM7080_FTP_RC_OK) {
			break;
		}
		reads++;
		crc = crc32_ieee_update(crc, (const uint8_t *)modem_ftp_chunk, size);
		bytes += size;
		if (k_uptime_get() > deadline) {
			result = "timeout";
			break;
		}
	}
	finish_line(append(result_line, LINE_BODY_MAX, modem_head("ftp", rc, start),
		",\"result\":\"%s\",\"reads\":%lu,\"bytes\":%u,\"crc32\":%u", result, reads,
		(unsigned int)bytes, (unsigned int)crc));
}

static void modem_gnss_query(int64_t start)
{
	struct sim7080_gnss_data g;
	size_t used;
	int rc;

	memset(&g, 0, sizeof(g));
	rc = mdm_sim7080_query_gnss(&g);
	used = append(result_line, LINE_BODY_MAX, modem_head("gnss_query", rc, start),
		",\"run\":%d,\"fix\":%d,\"lat\":%ld,\"lon\":%ld,\"alt\":%ld,\"hdop\":%u,\"cog\":%u,"
		"\"kmh\":%u", g.run_status ? 1 : 0, g.fix_status ? 1 : 0, (long)g.lat, (long)g.lon,
		(long)g.alt, g.hdop, g.cog, g.kmh);
	finish_line(modem_text(used, "utc", g.utc));
}

static void modem_cell(int64_t start)
{
	struct sim7080_ue_sys_info info;
	const struct sim7080_ue_sys_info_lte *lte = &info.cell.lte;
	size_t used;
	int rc;

	memset(&info, 0, sizeof(info));
	rc = mdm_sim7080_get_ue_sys_info(&info);
	used = append(result_line, LINE_BODY_MAX, modem_head("cell", rc, start),
		",\"sys_mode\":%d,\"op_mode\":%d,\"mcc\":%u,\"mnc\":%u,\"tac\":%u,\"sci\":%lu,"
		"\"pci\":%u,\"earfcn\":%u,\"dlbw\":%u,\"ulbw\":%u,\"rsrq\":%d,\"rsrp\":%d,"
		"\"rssi\":%d,\"rssnr\":%d,\"sinr\":%d", (int)info.sys_mode, (int)info.op_mode,
		lte->mcc, lte->mcn, lte->tac, (unsigned long)lte->sci, lte->pci, lte->earfcn,
		lte->dlbw, lte->ulbw, lte->rsrq, lte->rsrp, lte->rssi, lte->rssnr, lte->sinr);
	finish_line(modem_text(used, "band", (const char *)lte->band));
}

static void emit_modem(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	int64_t start = k_uptime_get();
	struct token op;
	struct token sub = {.text = "", .length = 0};
	int rc;

	if (!next_token(&cursor, &op)) {
		emit_error("test_modem", "usage: MODEM <operation>");
		return;
	}
	if (token_equals(&op, "INIT")) {
		modem_init_command(start);
		return;
	}
	if (token_equals(&op, "STATE")) {
		finish_line(append(result_line, LINE_BODY_MAX, modem_head("state", 0, start),
			",\"initialized\":%s,\"ready\":%s,\"init_res\":%u,\"if_up\":%s",
			modem_dev->state->initialized ? "true" : "false",
			device_is_ready(modem_dev) ? "true" : "false",
			(unsigned int)modem_dev->state->init_res,
			net_if_is_up(net_if_lookup_by_dev(modem_dev)) ? "true" : "false"));
		return;
	}
	if (!device_is_ready(modem_dev)) {
		emit_not_ready("test_modem", modem_dev);
		return;
	}
	(void)next_token(&cursor, &sub);
	if (token_equals(&op, "INFO")) {
		size_t used = modem_head("info", 0, start);

		used = modem_text(used, "manufacturer", mdm_sim7080_get_manufacturer());
		used = modem_text(used, "model", mdm_sim7080_get_model());
		used = modem_text(used, "revision", mdm_sim7080_get_revision());
		used = modem_text(used, "imei", mdm_sim7080_get_imei());
		finish_line(modem_text(used, "iccid", mdm_sim7080_get_iccid()));
	} else if (token_equals(&op, "POWER") && token_equals(&sub, "ON")) {
		rc = mdm_sim7080_power_on();
		finish_line(modem_head("power_on", rc, start));
	} else if (token_equals(&op, "POWER") && token_equals(&sub, "OFF")) {
		rc = mdm_sim7080_power_off();
		finish_line(modem_head("power_off", rc, start));
	} else if (token_equals(&op, "NET") && token_equals(&sub, "START")) {
		rc = mdm_sim7080_start_network();
		finish_line(modem_head("net_start", rc, start));
	} else if (token_equals(&op, "NET") && token_equals(&sub, "STOP")) {
		rc = mdm_sim7080_stop_network();
		finish_line(modem_head("net_stop", rc, start));
	} else if (token_equals(&op, "GNSS") && token_equals(&sub, "START")) {
		rc = mdm_sim7080_start_gnss();
		finish_line(modem_head("gnss_start", rc, start));
	} else if (token_equals(&op, "GNSS") && token_equals(&sub, "STOP")) {
		rc = mdm_sim7080_stop_gnss();
		finish_line(modem_head("gnss_stop", rc, start));
	} else if (token_equals(&op, "GNSS") && token_equals(&sub, "QUERY")) {
		modem_gnss_query(start);
	} else if (token_equals(&op, "SMS")) {
		modem_sms_command(sub.length > 0U ? sub.text : "", start);
	} else if (token_equals(&op, "BATTERY")) {
		uint8_t bcs = 0;
		uint8_t bcl = 0;
		uint16_t mv = 0;

		rc = mdm_sim7080_get_battery_charge(&bcs, &bcl, &mv);
		finish_line(append(result_line, LINE_BODY_MAX, modem_head("battery", rc, start),
			",\"bcs\":%u,\"bcl\":%u,\"mv\":%u", bcs, bcl, mv));
	} else if (token_equals(&op, "CELL")) {
		modem_cell(start);
	} else if (token_equals(&op, "TIME")) {
		struct tm t;

		memset(&t, 0, sizeof(t));
		rc = mdm_sim7080_get_local_time(&t);
		finish_line(append(result_line, LINE_BODY_MAX, modem_head("time", rc, start),
			",\"tm_year\":%d,\"tm_mon\":%d,\"tm_mday\":%d,\"tm_hour\":%d,\"tm_min\":%d,"
			"\"tm_sec\":%d,\"tm_isdst\":%d", t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour,
			t.tm_min, t.tm_sec, t.tm_isdst));
	} else if (token_equals(&op, "FTP")) {
		modem_ftp_command(sub.length > 0U ? sub.text : "", start);
	} else {
		emit_error("test_modem", "unknown operation");
	}
}
#else
static void emit_modem(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_modem\",\"status\":\"error\",\"detail\":\"no modem in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `CELLULAR ...`: the cellular driver API (zephyr/drivers/cellular.h) on the
 * `modem` alias. Events are counted from the moment the device exists:
 * modem_cellular sets up the lock its callback registration takes in its
 * init, which this image defers to `NET UP`.
 */
#if DUT_CELLULAR
#define CELLULAR_INFO_MAX 66U
/* get_signal answers -EBUSY while the driver's periodic script holds the chat. */
#define CELLULAR_SIGNAL_RETRY_MS 2000
#define CELLULAR_SIGNAL_RETRY_GAP_MS 20
#define CELLULAR_NETWORKS_MAX 8U

static const struct device *const cellular_dev = DEVICE_DT_GET(DT_ALIAS(modem));
static atomic_t cellular_info_events;
static atomic_t cellular_registration_events;
static atomic_t cellular_last_registration = ATOMIC_INIT(-1);
static atomic_t cellular_checks_ok;
static atomic_t cellular_checks_failed;
static bool cellular_subscribed;

static const struct {
	const char *name;
	enum cellular_modem_info_type type;
} cellular_info_fields[] = {
	{"imei", CELLULAR_MODEM_INFO_IMEI},
	{"model", CELLULAR_MODEM_INFO_MODEL_ID},
	{"manufacturer", CELLULAR_MODEM_INFO_MANUFACTURER},
	{"fw", CELLULAR_MODEM_INFO_FW_VERSION},
	{"imsi", CELLULAR_MODEM_INFO_SIM_IMSI},
	{"iccid", CELLULAR_MODEM_INFO_SIM_ICCID},
};

static const struct {
	const char *name;
	enum cellular_access_technology tech;
} cellular_reg_fields[] = {
	{"gsm", CELLULAR_ACCESS_TECHNOLOGY_GSM},
	{"gprs", CELLULAR_ACCESS_TECHNOLOGY_UTRAN},
	{"lte", CELLULAR_ACCESS_TECHNOLOGY_E_UTRAN},
};

static void cellular_event_cb(const struct device *dev, enum cellular_event event,
	const void *payload, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (event) {
	case CELLULAR_EVENT_MODEM_INFO_CHANGED:
		atomic_inc(&cellular_info_events);
		break;
	case CELLULAR_EVENT_REGISTRATION_STATUS_CHANGED:
		atomic_inc(&cellular_registration_events);
		if (payload != NULL) {
			const struct cellular_evt_registration_status *r = payload;

			atomic_set(&cellular_last_registration, (atomic_val_t)r->status);
		}
		break;
	case CELLULAR_EVENT_MODEM_COMMS_CHECK_RESULT:
		if (payload != NULL &&
			((const struct cellular_evt_modem_comms_check_result *)payload)->success) {
			atomic_inc(&cellular_checks_ok);
		} else {
			atomic_inc(&cellular_checks_failed);
		}
		break;
	default:
		break;
	}
}

static void cellular_subscribe(void)
{
	if (cellular_subscribed || !device_is_ready(cellular_dev)) {
		return;
	}
	cellular_subscribed = cellular_set_callback(cellular_dev,
		CELLULAR_EVENT_MODEM_INFO_CHANGED | CELLULAR_EVENT_REGISTRATION_STATUS_CHANGED |
		CELLULAR_EVENT_MODEM_COMMS_CHECK_RESULT, cellular_event_cb, NULL) == 0;
}

static size_t cellular_head(const char *op, int rc, int64_t start)
{
	return append(result_line, LINE_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_cellular\",\"status\":\"%s\",\"op\":\"%s\",\"rc\":%d,"
		"\"ms\":%lld", rc >= 0 ? "ok" : "error", op, rc, (long long)(k_uptime_get() - start));
}

static void cellular_info(int64_t start)
{
	char info[ARRAY_SIZE(cellular_info_fields)][CELLULAR_INFO_MAX];
	int first = 0;
	size_t used;

	for (size_t i = 0; i < ARRAY_SIZE(cellular_info_fields); i++) {
		int rc;

		memset(info[i], 0, sizeof(info[i]));
		rc = cellular_get_modem_info(cellular_dev, cellular_info_fields[i].type, info[i],
			sizeof(info[i]) - 1U);
		if (rc != 0 && first == 0) {
			first = rc;
		}
	}
	used = cellular_head("info", first, start);
	for (size_t i = 0; i < ARRAY_SIZE(cellular_info_fields); i++) {
		used = append(result_line, LINE_BODY_MAX, used, ",\"%s\":", cellular_info_fields[i].name);
		used = append_json_text(result_line, LINE_BODY_MAX, used, info[i], strlen(info[i]));
	}
	finish_line(used);
}

static void cellular_signal(const struct token *which, int64_t start)
{
	enum cellular_signal_type type;
	unsigned int tries = 0;
	int16_t value = 0;
	int rc;

	if (token_equals(which, "rssi")) {
		type = CELLULAR_SIGNAL_RSSI;
	} else if (token_equals(which, "rsrp")) {
		type = CELLULAR_SIGNAL_RSRP;
	} else if (token_equals(which, "rsrq")) {
		type = CELLULAR_SIGNAL_RSRQ;
	} else {
		emit_error("test_cellular", "usage: CELLULAR SIGNAL rssi|rsrp|rsrq");
		return;
	}
	do {
		if (tries > 0U) {
			k_msleep(CELLULAR_SIGNAL_RETRY_GAP_MS);
		}
		tries++;
		rc = cellular_get_signal(cellular_dev, type, &value);
	} while (rc == -EBUSY && k_uptime_get() - start < CELLULAR_SIGNAL_RETRY_MS);
	finish_line(append(result_line, LINE_BODY_MAX, cellular_head("signal", rc, start),
		",\"value\":%d,\"tries\":%u", (int)value, tries));
}

static void cellular_registration(const struct token *tech, int64_t start)
{
	enum cellular_registration_status status = CELLULAR_REGISTRATION_UNKNOWN;
	int values[ARRAY_SIZE(cellular_reg_fields)];
	unsigned long number = 0;
	size_t used;
	int first = 0;
	int rc;

	if (tech->length > 0U) {
		if (!token_number(tech, "", &number) || number > 255U) {
			emit_error("test_cellular", "usage: CELLULAR REG [tech]");
			return;
		}
		rc = cellular_get_registration_status(cellular_dev,
			(enum cellular_access_technology)number, &status);
		finish_line(append(result_line, LINE_BODY_MAX, cellular_head("reg", rc, start),
			",\"tech\":%lu,\"registration\":%d", number, rc == 0 ? (int)status : rc));
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(cellular_reg_fields); i++) {
		rc = cellular_get_registration_status(cellular_dev, cellular_reg_fields[i].tech, &status);
		values[i] = rc == 0 ? (int)status : rc;
		if (rc != 0 && first == 0) {
			first = rc;
		}
	}
	used = cellular_head("reg", first, start);
	for (size_t i = 0; i < ARRAY_SIZE(cellular_reg_fields); i++) {
		used = append(result_line, LINE_BODY_MAX, used, ",\"%s\":%d",
			cellular_reg_fields[i].name, values[i]);
	}
	finish_line(used);
}

static void cellular_networks(int64_t start)
{
	const struct cellular_network *networks = NULL;
	uint8_t count = 0;
	int rc = cellular_get_supported_networks(cellular_dev, &networks, &count);
	size_t used = append(result_line, LINE_BODY_MAX, cellular_head("networks", rc, start),
		",\"count\":%u,\"networks\":[", rc == 0 ? (unsigned int)count : 0U);

	for (uint8_t i = 0; rc == 0 && networks != NULL && i < count && i < CELLULAR_NETWORKS_MAX; i++) {
		used = append(result_line, LINE_BODY_MAX, used, "%s[%d,%u]", i == 0U ? "" : ",",
			(int)networks[i].technology, (unsigned int)networks[i].size);
	}
	finish_line(append(result_line, LINE_BODY_MAX, used, "]"));
}

static void emit_cellular(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	int64_t start = k_uptime_get();
	struct token op;
	struct token arg = {.text = "", .length = 0};

	if (!next_token(&cursor, &op)) {
		emit_error("test_cellular", "usage: CELLULAR INFO|SIGNAL|REG|APN|NETWORKS|EVENTS");
		return;
	}
	if (!device_is_ready(cellular_dev)) {
		emit_not_ready("test_cellular", cellular_dev);
		return;
	}
	cellular_subscribe();
	(void)next_token(&cursor, &arg);
	if (token_equals(&op, "INFO")) {
		cellular_info(start);
	} else if (token_equals(&op, "SIGNAL")) {
		cellular_signal(&arg, start);
	} else if (token_equals(&op, "REG")) {
		cellular_registration(&arg, start);
	} else if (token_equals(&op, "APN")) {
		char apn[TOKEN_TEXT_MAX];
		int rc;

		if (!token_copy(&arg, apn, sizeof(apn))) {
			emit_error("test_cellular", "usage: CELLULAR APN <apn>");
			return;
		}
		rc = cellular_set_apn(cellular_dev, apn);
		finish_line(cellular_head("apn", rc, start));
	} else if (token_equals(&op, "NETWORKS")) {
		cellular_networks(start);
	} else if (token_equals(&op, "EVENTS")) {
		finish_line(append(result_line, LINE_BODY_MAX, cellular_head("events", 0, start),
			",\"subscribed\":%s,\"info\":%ld,\"registration\":%ld,\"last_registration\":%ld,"
			"\"checks_ok\":%ld,\"checks_failed\":%ld", cellular_subscribed ? "true" : "false",
			(long)atomic_get(&cellular_info_events),
			(long)atomic_get(&cellular_registration_events),
			(long)atomic_get(&cellular_last_registration), (long)atomic_get(&cellular_checks_ok),
			(long)atomic_get(&cellular_checks_failed)));
	} else {
		emit_error("test_cellular", "unknown operation");
	}
}
#else
static void emit_cellular(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_cellular\",\"status\":\"error\",\"detail\":\"no cellular modem in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `NET UP|DOWN|WAIT|STATE`: the default interface on a native stack and the
 * device behind it, the `modem` alias. Up powers the device (its deferred
 * init the first time, a PM resume after that), brings the interface up
 * and waits for the connection manager's L4 event, which needs the address
 * IPCP assigns, not just an interface that is up. Down is the reverse.
 */
#if DUT_NET
#define NETIF_UP_TIMEOUT_MS 60000UL
#define NETIF_WAIT_TIMEOUT_MS 30000UL

static atomic_t netif_l4;
static atomic_t netif_l4_up;
static atomic_t netif_l4_down;
static atomic_t netif_dns_add;

static void netif_l4_event(uint64_t event, struct net_if *iface, void *info, size_t info_length,
	void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (event == NET_EVENT_L4_CONNECTED) {
		atomic_set(&netif_l4, 1);
		atomic_inc(&netif_l4_up);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		atomic_set(&netif_l4, 0);
		atomic_inc(&netif_l4_down);
	} else if (event == NET_EVENT_DNS_SERVER_ADD) {
		atomic_inc(&netif_dns_add);
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(dut_l4_events,
	NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED | NET_EVENT_DNS_SERVER_ADD,
	netif_l4_event, NULL);

#if defined(CONFIG_NET_L2_PPP_MGMT)
static atomic_t netif_ppp_running;
static atomic_t netif_ppp_dead;

static void netif_ppp_event(uint64_t event, struct net_if *iface, void *info, size_t info_length,
	void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (event == NET_EVENT_PPP_PHASE_RUNNING) {
		atomic_inc(&netif_ppp_running);
	} else if (event == NET_EVENT_PPP_PHASE_DEAD) {
		atomic_inc(&netif_ppp_dead);
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(dut_ppp_events,
	NET_EVENT_PPP_PHASE_RUNNING | NET_EVENT_PPP_PHASE_DEAD, netif_ppp_event, NULL);
#endif

#if DUT_NET_PM
static const struct device *const netif_dev = DEVICE_DT_GET(DT_ALIAS(modem));
#endif

static const char *json_bool(bool value)
{
	return value ? "true" : "false";
}

static void netif_dns_server(char *out, size_t size)
{
#if defined(CONFIG_DNS_RESOLVER)
	struct dns_resolve_context *ctx = dns_resolve_get_default();

	for (size_t i = 0; ctx != NULL && i < ARRAY_SIZE(ctx->servers); i++) {
		struct net_sockaddr *server = &ctx->servers[i].dns_server;

		if (server->sa_family == NET_AF_INET) {
			(void)net_addr_ntop(NET_AF_INET, &net_sin(server)->sin_addr, out, size);
			return;
		}
	}
#else
	ARG_UNUSED(size);
#endif
	out[0] = '\0';
}

static size_t netif_state(size_t used)
{
	struct net_if *iface = net_if_get_default();
	char ip[NET_INET_ADDRSTRLEN] = "";
	char dns[NET_INET_ADDRSTRLEN] = "";

	if (iface != NULL) {
		struct net_in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_ANY_STATE);

		if (addr != NULL) {
			(void)net_addr_ntop(NET_AF_INET, addr, ip, sizeof(ip));
		}
	}
	netif_dns_server(dns, sizeof(dns));
	used = append(result_line, LINE_BODY_MAX, used,
		",\"admin_up\":%s,\"up\":%s,\"carrier\":%s,\"dormant\":%s,\"l4\":%s,\"ip\":\"%s\","
		"\"dns\":\"%s\",\"l4_up\":%ld,\"l4_down\":%ld,\"dns_add\":%ld",
		json_bool(iface != NULL && net_if_is_admin_up(iface)),
		json_bool(iface != NULL && net_if_is_up(iface)),
		json_bool(iface != NULL && net_if_is_carrier_ok(iface)),
		json_bool(iface != NULL && net_if_is_dormant(iface)), json_bool(atomic_get(&netif_l4) != 0),
		ip, dns, (long)atomic_get(&netif_l4_up), (long)atomic_get(&netif_l4_down),
		(long)atomic_get(&netif_dns_add));
#if defined(CONFIG_NET_L2_PPP_MGMT)
	used = append(result_line, LINE_BODY_MAX, used, ",\"ppp_running\":%ld,\"ppp_dead\":%ld",
		(long)atomic_get(&netif_ppp_running), (long)atomic_get(&netif_ppp_dead));
#endif
#if DUT_NET_PM
	{
		enum pm_device_state state = PM_DEVICE_STATE_OFF;
		int rc = pm_device_state_get(netif_dev, &state);

		used = append(result_line, LINE_BODY_MAX, used,
			",\"pm\":\"%s\",\"initialized\":%s,\"init_res\":%u",
			rc == 0 ? pm_device_state_str(state) : "none",
			json_bool(netif_dev->state->initialized),
			(unsigned int)netif_dev->state->init_res);
	}
#endif
	return used;
}

static bool netif_wait(bool connected, unsigned long timeout_ms)
{
	int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;

	while ((atomic_get(&netif_l4) != 0) != connected) {
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(10);
	}
	return true;
}

static size_t netif_head(const char *op, bool ok, int64_t start)
{
	return append(result_line, LINE_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_net\",\"status\":\"%s\",\"op\":\"%s\",\"ms\":%lld",
		ok ? "ok" : "error", op, (long long)(k_uptime_get() - start));
}

static void netif_up(unsigned long timeout_ms, int64_t start)
{
	struct net_if *iface = net_if_get_default();
	const char *pm_op = "none";
	int pm_rc = 0;
	int up_rc;
	bool l4;

#if DUT_NET_PM
	if (!netif_dev->state->initialized) {
		pm_op = "init";
		pm_rc = device_init(netif_dev);
#if DUT_CELLULAR
		cellular_subscribe();
#endif
	} else {
		pm_op = "resume";
		pm_rc = pm_device_action_run(netif_dev, PM_DEVICE_ACTION_RESUME);
	}
#endif
	up_rc = iface != NULL ? net_if_up(iface) : -ENODEV;
	l4 = (up_rc == 0 || up_rc == -EALREADY) && netif_wait(true, timeout_ms);
	finish_line(netif_state(append(result_line, LINE_BODY_MAX, netif_head("up", l4, start),
		",\"pm_op\":\"%s\",\"pm_rc\":%d,\"up_rc\":%d", pm_op, pm_rc, up_rc)));
}

static void netif_down(int64_t start)
{
	struct net_if *iface = net_if_get_default();
	int down_rc = iface != NULL ? net_if_down(iface) : -ENODEV;
	int pm_rc = 0;
	bool ok = down_rc == 0 || down_rc == -EALREADY;

#if DUT_NET_PM
	/* Blocks until the driver has closed CMUX and powered the part off, 30 s at most. */
	pm_rc = netif_dev->state->initialized ?
		pm_device_action_run(netif_dev, PM_DEVICE_ACTION_SUSPEND) : -EALREADY;
	ok = ok && (pm_rc == 0 || pm_rc == -EALREADY);
#endif
	finish_line(netif_state(append(result_line, LINE_BODY_MAX, netif_head("down", ok, start),
		",\"down_rc\":%d,\"pm_rc\":%d", down_rc, pm_rc)));
}

static void emit_netif(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	int64_t start = k_uptime_get();
	struct token op;
	struct token arg = {.text = "", .length = 0};
	unsigned long timeout_ms = 0;

	if (!next_token(&cursor, &op)) {
		emit_error("test_net", "usage: NET UP [ms]|DOWN|WAIT UP|DOWN [ms]|STATE");
		return;
	}
	if (net_if_get_default() == NULL) {
		emit_error("test_net", "no network interface");
		return;
	}
	if (token_equals(&op, "UP")) {
		timeout_ms = NETIF_UP_TIMEOUT_MS;
		if (next_token(&cursor, &arg) && !token_number(&arg, "", &timeout_ms)) {
			emit_error("test_net", "usage: NET UP [ms]");
			return;
		}
		netif_up(timeout_ms, start);
	} else if (token_equals(&op, "DOWN")) {
		netif_down(start);
	} else if (token_equals(&op, "WAIT")) {
		struct token ms;
		bool connected;
		bool reached;

		timeout_ms = NETIF_WAIT_TIMEOUT_MS;
		if (!next_token(&cursor, &arg) ||
			!(token_equals(&arg, "UP") || token_equals(&arg, "DOWN")) ||
			(next_token(&cursor, &ms) && !token_number(&ms, "", &timeout_ms))) {
			emit_error("test_net", "usage: NET WAIT UP|DOWN [ms]");
			return;
		}
		connected = token_equals(&arg, "UP");
		reached = netif_wait(connected, timeout_ms);
		finish_line(netif_state(netif_head(connected ? "wait_up" : "wait_down", reached,
			start)));
	} else if (token_equals(&op, "STATE")) {
		finish_line(netif_state(netif_head("state", true, start)));
	} else {
		emit_error("test_net", "unknown operation");
	}
}
#else
static void emit_netif(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_net\",\"status\":\"error\",\"detail\":\"no network management in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `UART <alias> ...`: raw bytes on a UART no driver owns, for a part no
 * Zephyr driver speaks. Everything received since boot waits in a ring the
 * reads take from in order, so URCs that arrive between commands are kept
 * for the next read. A read stops at its condition, at its deadline, or
 * when the next byte would not fit in the result line (`more`); what it
 * did not take stays in the ring.
 */
#if DUT_UART
#define UART_RING_SIZE 4096U
#define UART_TAKE_MAX 480U
#define UART_SEND_MAX 1024U
#define UART_RECV_MAX 4096UL
#define UART_HEAD_BYTES 16U
#define UART_AT_TIMEOUT_MS 10000UL
#define UART_RECV_TIMEOUT_MS 5000UL
/* What the fields after `data` take at most. */
#define UART_TAIL_MAX 128U
/* The modem's `> ` prompt may come as `>` and a space a moment later. */
#define UART_PROMPT_GRACE_MS 5

struct uart_alias {
	const char *name;
	const struct device *dev;
};

#define UART_ALIAS(name, id) IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(id)), \
	({name, DEVICE_DT_GET(DT_ALIAS(id))},))

static const struct uart_alias uart_aliases[] = {
	UART_ALIAS("modem-uart", modem_uart)
};

struct uart_port {
	struct ring_buf ring;
	atomic_t overruns;
	int errors;
	uint8_t storage[UART_RING_SIZE];
};

static struct uart_port uart_ports[ARRAY_SIZE(uart_aliases)];

enum uart_stop {
	UART_STOP_FINAL,
	UART_STOP_TEXT,
	UART_STOP_LINE,
	UART_STOP_TIME,
};

struct uart_take {
	uint8_t data[UART_TAKE_MAX];
	size_t length;
	size_t cost;
	size_t budget;
	size_t line_start;
	bool found;
	bool more;
};

static struct uart_take uart_take;
static uint8_t uart_out[UART_SEND_MAX];

static void uart_port_isr(const struct device *dev, void *user_data)
{
	struct uart_port *port = user_data;
	uint8_t chunk[32];

	if (uart_irq_update(dev) < 0) {
		return;
	}
	while (uart_irq_rx_ready(dev) > 0) {
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));
		uint32_t put;

		if (n <= 0) {
			break;
		}
		put = ring_buf_put(&port->ring, chunk, (uint32_t)n);
		if (put < (uint32_t)n) {
			atomic_add(&port->overruns, (atomic_val_t)((uint32_t)n - put));
		}
	}
}

static void uart_ports_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(uart_aliases); i++) {
		const struct device *dev = uart_aliases[i].dev;
		struct uart_port *port = &uart_ports[i];

		ring_buf_init(&port->ring, sizeof(port->storage), port->storage);
		if (device_is_ready(dev) &&
			uart_irq_callback_user_data_set(dev, uart_port_isr, port) == 0) {
			uart_irq_rx_enable(dev);
		}
	}
}

static bool uart_peek(struct uart_port *port, uint8_t *c)
{
	unsigned int key = irq_lock();
	uint32_t n = ring_buf_peek(&port->ring, c, 1);

	irq_unlock(key);
	return n == 1U;
}

static uint32_t uart_discard(struct uart_port *port, uint32_t n)
{
	unsigned int key = irq_lock();
	uint32_t got = ring_buf_get(&port->ring, NULL, n);

	irq_unlock(key);
	return got;
}

static uint32_t uart_pending(struct uart_port *port)
{
	unsigned int key = irq_lock();
	uint32_t n = ring_buf_size_get(&port->ring);

	irq_unlock(key);
	return n;
}

/* The next byte within the deadline, taken from the ring; false on timeout. */
static bool uart_next(struct uart_port *port, uint8_t *c, int64_t deadline)
{
	while (!uart_peek(port, c)) {
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(1);
	}
	return true;
}

/* `\r`, `\n`, `\t`, `\\` and `\xHH`; everything else as it stands. */
static int uart_unescape(const char *s, uint8_t *out, size_t size, size_t *length)
{
	size_t n = 0;

	while (*s != '\0') {
		uint8_t c = (uint8_t)*s++;

		if (c == '\\') {
			char e = *s;

			if (e == 'r' || e == 'n' || e == 't' || e == '\\') {
				c = e == 'r' ? '\r' : (e == 'n' ? '\n' : (e == 't' ? '\t' : '\\'));
				s++;
			} else if (e == 'x' && s[1] != '\0' && s[2] != '\0' &&
				hex_nibble(s[1]) >= 0 && hex_nibble(s[2]) >= 0) {
				c = (uint8_t)((hex_nibble(s[1]) << 4) | hex_nibble(s[2]));
				s += 3;
			} else {
				return -EINVAL;
			}
		}
		if (n == size) {
			return -EINVAL;
		}
		out[n++] = c;
	}
	*length = n;
	return 0;
}

static void uart_write(const struct device *dev, const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		uart_poll_out(dev, bytes[i]);
	}
}

static bool take_ends_with(const struct uart_take *t, const void *s, size_t n)
{
	return n > 0U && t->length >= n && memcmp(&t->data[t->length - n], s, n) == 0;
}

static bool take_line_starts(const struct uart_take *t, const void *s, size_t n)
{
	return t->length - t->line_start >= n && memcmp(&t->data[t->line_start], s, n) == 0;
}

/*
 * What ends a command for TinyGSM's waitResponse and a V.250 reader: a
 * buffer ending in `OK` or `ERROR` and its CR LF, a whole `+CME ERROR` or
 * `+CMS ERROR` line, or a data-phase prompt.
 */
static bool take_final(const struct uart_take *t)
{
	uint8_t last = t->data[t->length - 1U];

	if (last == '\n') {
		return take_ends_with(t, "OK\r\n", 4) || take_ends_with(t, "ERROR\r\n", 7) ||
			take_line_starts(t, "+CME ERROR", 10) || take_line_starts(t, "+CMS ERROR", 10);
	}
	return take_ends_with(t, "\n>", 2) || take_ends_with(t, "\n> ", 3);
}

static void uart_collect(struct uart_port *port, enum uart_stop stop, const uint8_t *pattern,
	size_t pattern_length, int64_t deadline)
{
	struct uart_take *t = &uart_take;
	uint8_t c;

	while (!t->found) {
		char unit[7];
		size_t cost;

		if (!uart_peek(port, &c)) {
			if (k_uptime_get() >= deadline) {
				break;
			}
			k_msleep(1);
			continue;
		}
		cost = json_unit(c, unit);
		if (t->length == UART_TAKE_MAX || t->cost + cost > t->budget) {
			t->more = true;
			break;
		}
		(void)uart_discard(port, 1);
		t->data[t->length++] = c;
		t->cost += cost;
		if (stop == UART_STOP_FINAL) {
			t->found = take_final(t);
		} else if (stop == UART_STOP_TEXT) {
			t->found = take_ends_with(t, pattern, pattern_length);
		} else if (stop == UART_STOP_LINE && c == '\n') {
			t->found = take_line_starts(t, pattern, pattern_length);
		}
		if (c == '\n') {
			t->line_start = t->length;
		}
	}
	if (t->found && stop == UART_STOP_FINAL && t->data[t->length - 1U] == '>' &&
		t->length < UART_TAKE_MAX &&
		uart_next(port, &c, k_uptime_get() + UART_PROMPT_GRACE_MS) && c == ' ') {
		(void)uart_discard(port, 1);
		t->data[t->length++] = c;
	}
}

static size_t uart_head(const char *op, bool ok)
{
	return append(result_line, LINE_BODY_MAX, 0,
		"MCLW-HIL/1 {\"test\":\"test_uart\",\"status\":\"%s\",\"op\":\"%s\"", ok ? "ok" : "error",
		op);
}

static size_t uart_tail(size_t used, size_t index, int64_t start)
{
	struct uart_port *port = &uart_ports[index];
	int errors = uart_err_check(uart_aliases[index].dev);

	if (errors > 0) {
		port->errors |= errors;
	}
	return append(result_line, LINE_BODY_MAX, used,
		",\"pending\":%u,\"overruns\":%ld,\"errors\":%d,\"ms\":%lld",
		(unsigned int)uart_pending(port), (long)atomic_get(&port->overruns), port->errors,
		(long long)(k_uptime_get() - start));
}

/* A text read: the bytes taken as `data`, then whether the stop was reached. */
static void uart_read(const char *op, size_t index, enum uart_stop stop, const uint8_t *pattern,
	size_t pattern_length, unsigned long timeout_ms, int64_t start)
{
	struct uart_take *t = &uart_take;
	size_t used = uart_head(op, true);

	t->length = 0;
	t->cost = 0;
	t->line_start = 0;
	t->found = false;
	t->more = false;
	t->budget = LINE_BODY_MAX - used - UART_TAIL_MAX - sizeof(",\"data\":\"\"");
	uart_collect(&uart_ports[index], stop, pattern, pattern_length,
		start + (int64_t)timeout_ms);
	used = append(result_line, LINE_BODY_MAX, used, ",\"data\":");
	used = append_json_text(result_line, LINE_BODY_MAX, used, (const char *)t->data, t->length);
	used = append(result_line, LINE_BODY_MAX, used,
		",\"length\":%u,\"found\":%s,\"more\":%s", (unsigned int)t->length,
		t->found ? "true" : "false", t->more ? "true" : "false");
	finish_line(uart_tail(used, index, start));
}

/* `RECV <n>`: exactly n raw bytes, as a CRC-32 seeded with `crc=`, and as `data` if they fit. */
static void uart_recv(size_t index, unsigned long want, unsigned long timeout_ms, uint32_t seed,
	int64_t start)
{
	struct uart_port *port = &uart_ports[index];
	struct uart_take *t = &uart_take;
	uint8_t head[UART_HEAD_BYTES];
	int64_t deadline = start + (int64_t)timeout_ms;
	unsigned long received = 0;
	uint32_t crc = seed;
	bool fits = true;
	size_t used = uart_head("recv", true);

	t->length = 0;
	t->cost = 0;
	t->budget = LINE_BODY_MAX - used - UART_TAIL_MAX - sizeof(",\"data\":\"\"") -
		sizeof(",\"received\":4294967295,\"found\":false,\"crc32\":4294967295,\"head\":\"\"") -
		2U * UART_HEAD_BYTES;
	while (received < want) {
		uint8_t c;
		char unit[7];
		size_t cost;

		if (!uart_next(port, &c, deadline)) {
			break;
		}
		(void)uart_discard(port, 1);
		crc = crc32_ieee_update(crc, &c, 1);
		if (received < UART_HEAD_BYTES) {
			head[received] = c;
		}
		received++;
		cost = json_unit(c, unit);
		if (fits && t->length < UART_TAKE_MAX && t->cost + cost <= t->budget) {
			t->data[t->length++] = c;
			t->cost += cost;
		} else {
			fits = false;
		}
	}
	used = append(result_line, LINE_BODY_MAX, used,
		",\"received\":%lu,\"found\":%s,\"crc32\":%u,\"head\":", received,
		received == want ? "true" : "false", (unsigned int)crc);
	used = append_hex(result_line, LINE_BODY_MAX, used, head, MIN(received, UART_HEAD_BYTES));
	if (fits) {
		used = append(result_line, LINE_BODY_MAX, used, ",\"data\":");
		used = append_json_text(result_line, LINE_BODY_MAX, used, (const char *)t->data,
			t->length);
	}
	finish_line(uart_tail(used, index, start));
}

/* The text after one separating space: the rest of the command, spaces included. */
static const char *uart_rest(const char *cursor)
{
	return *cursor == ' ' ? cursor + 1 : cursor;
}

static void emit_uart(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	int64_t start = k_uptime_get();
	const struct device *dev;
	struct token alias;
	struct token op;
	struct token arg;
	size_t index = ARRAY_SIZE(uart_aliases);
	size_t length = 0;
	unsigned long ms = 0;

	if (!next_token(&cursor, &alias) || !next_token(&cursor, &op)) {
		emit_error("test_uart", "usage: UART <alias> AT|TX|WAIT|UNTIL|LINE|READ|SEND|RECV|FLUSH");
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(uart_aliases); i++) {
		if (token_equals(&alias, uart_aliases[i].name)) {
			index = i;
		}
	}
	if (index == ARRAY_SIZE(uart_aliases)) {
		emit_error("test_uart", "unknown alias");
		return;
	}
	dev = uart_aliases[index].dev;
	if (!device_is_ready(dev)) {
		emit_not_ready("test_uart", dev);
		return;
	}
	if (token_equals(&op, "AT") || token_equals(&op, "TX")) {
		bool at = token_equals(&op, "AT");

		if (uart_unescape(uart_rest(cursor), uart_out, sizeof(uart_out) - 1U, &length) != 0 ||
			(at && length == 0U)) {
			emit_error("test_uart", "bad text");
			return;
		}
		if (at) {
			uart_out[length++] = '\r';
		}
		uart_write(dev, uart_out, length);
		if (at) {
			uart_read("at", index, UART_STOP_FINAL, NULL, 0, UART_AT_TIMEOUT_MS, start);
		} else {
			finish_line(uart_tail(append(result_line, LINE_BODY_MAX, uart_head("tx", true),
				",\"written\":%u", (unsigned int)length), index, start));
		}
	} else if (token_equals(&op, "WAIT") || token_equals(&op, "READ")) {
		bool wait = token_equals(&op, "WAIT");

		if (!next_token(&cursor, &arg) || !token_number(&arg, "", &ms)) {
			emit_error("test_uart", "usage: UART <alias> WAIT|READ <ms>");
			return;
		}
		uart_read(wait ? "wait" : "read", index, wait ? UART_STOP_FINAL : UART_STOP_TIME,
			NULL, 0, ms, start);
	} else if (token_equals(&op, "UNTIL") || token_equals(&op, "LINE")) {
		bool until = token_equals(&op, "UNTIL");

		if (!next_token(&cursor, &arg) || !token_number(&arg, "", &ms) ||
			uart_unescape(uart_rest(cursor), uart_out, sizeof(uart_out), &length) != 0 ||
			length == 0U) {
			emit_error("test_uart", "usage: UART <alias> UNTIL|LINE <ms> <text>");
			return;
		}
		uart_read(until ? "until" : "line", index, until ? UART_STOP_TEXT : UART_STOP_LINE,
			uart_out, length, ms, start);
	} else if (token_equals(&op, "SEND")) {
		if (!next_token(&cursor, &arg) ||
			payload_parse(&arg, uart_out, sizeof(uart_out), &length) != 0) {
			emit_error("test_uart", "usage: UART <alias> SEND <hex|len=N>");
			return;
		}
		uart_write(dev, uart_out, length);
		finish_line(uart_tail(append(result_line, LINE_BODY_MAX, uart_head("send", true),
			",\"sent\":%u,\"crc32\":%u", (unsigned int)length,
			(unsigned int)crc32_ieee(uart_out, length)), index, start));
	} else if (token_equals(&op, "RECV")) {
		unsigned long want = 0;
		unsigned long seed = 0;

		ms = UART_RECV_TIMEOUT_MS;
		if (!next_token(&cursor, &arg) || !token_number(&arg, "", &want) || want == 0U ||
			want > UART_RECV_MAX) {
			emit_error("test_uart", "usage: UART <alias> RECV <n> [ms] [crc=<seed>]");
			return;
		}
		while (next_token(&cursor, &arg)) {
			if (!token_number(&arg, "crc=", &seed) && !token_number(&arg, "", &ms)) {
				emit_error("test_uart", "bad option");
				return;
			}
		}
		uart_recv(index, want, ms, (uint32_t)seed, start);
	} else if (token_equals(&op, "FLUSH")) {
		struct uart_port *port = &uart_ports[index];
		uint32_t discarded = uart_discard(port, UART_RING_SIZE);
		size_t used = append(result_line, LINE_BODY_MAX, uart_head("flush", true),
			",\"discarded\":%u", (unsigned int)discarded);

		used = uart_tail(used, index, start);
		atomic_set(&port->overruns, 0);
		port->errors = 0;
		finish_line(used);
	} else {
		emit_error("test_uart", "unknown operation");
	}
}
#else
static void emit_uart(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_uart\",\"status\":\"error\",\"detail\":\"no raw uart in this image\"}\r\n");
	mclw_dut_mailbox_write(result_line);
}
#endif

/*
 * `GPIO <name> SET 0|1|PULSE <ms>|GET`: an output the image declares as
 * `<name>-gpios` under /zephyr,user, driven in logical levels (1 is
 * asserted, low for an active-low line).
 */
#if DUT_GPIO
struct gpio_pin {
	const char *name;
	struct gpio_dt_spec spec;
};

#define GPIO_PIN(id) IF_ENABLED(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), id##_gpios), \
	({STRINGIFY(id), GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), id##_gpios)},))

static const struct gpio_pin gpio_pins[] = {
	GPIO_PIN(pwrkey)
};

static bool gpio_configured[ARRAY_SIZE(gpio_pins)];
static int gpio_levels[ARRAY_SIZE(gpio_pins)];

static int gpio_drive(size_t index, int level)
{
	const struct gpio_dt_spec *spec = &gpio_pins[index].spec;
	int rc = 0;

	if (!gpio_configured[index]) {
		rc = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
		gpio_configured[index] = rc == 0;
	}
	if (rc == 0) {
		rc = gpio_pin_set_dt(spec, level);
	}
	if (rc == 0) {
		gpio_levels[index] = level;
	}
	return rc;
}

static void emit_gpio(const char *args)
{
	const char *cursor = args != NULL ? args : "";
	int64_t start = k_uptime_get();
	struct token name;
	struct token op;
	struct token arg = {.text = "", .length = 0};
	size_t index = ARRAY_SIZE(gpio_pins);
	unsigned long value = 0;
	int rc;

	if (!next_token(&cursor, &name) || !next_token(&cursor, &op)) {
		emit_error("test_gpio", "usage: GPIO <name> SET 0|1|PULSE <ms>|GET");
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(gpio_pins); i++) {
		if (token_equals(&name, gpio_pins[i].name)) {
			index = i;
		}
	}
	if (index == ARRAY_SIZE(gpio_pins)) {
		emit_error("test_gpio", "unknown pin");
		return;
	}
	if (!gpio_is_ready_dt(&gpio_pins[index].spec)) {
		emit_not_ready("test_gpio", gpio_pins[index].spec.port);
		return;
	}
	(void)next_token(&cursor, &arg);
	if (token_equals(&op, "SET")) {
		if (!token_number(&arg, "", &value) || value > 1U) {
			emit_error("test_gpio", "usage: GPIO <name> SET 0|1");
			return;
		}
		rc = gpio_drive(index, (int)value);
	} else if (token_equals(&op, "PULSE")) {
		if (!token_number(&arg, "", &value) || value == 0U) {
			emit_error("test_gpio", "usage: GPIO <name> PULSE <ms>");
			return;
		}
		rc = gpio_drive(index, 1);
		if (rc == 0) {
			k_msleep((int32_t)value);
			rc = gpio_drive(index, 0);
		}
	} else if (token_equals(&op, "GET")) {
		rc = 0;
	} else {
		emit_error("test_gpio", "unknown operation");
		return;
	}
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_gpio\",\"status\":\"%s\",\"errno\":%d,\"level\":%d,"
		"\"read\":%d,\"ms\":%lld}\r\n", rc == 0 ? "ok" : "error", -rc, gpio_levels[index],
		gpio_configured[index] ? gpio_pin_get_dt(&gpio_pins[index].spec) : -1,
		(long long)(k_uptime_get() - start));
	mclw_dut_mailbox_write(result_line);
}
#else
static void emit_gpio(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_gpio\",\"status\":\"error\",\"detail\":\"no gpio in this image\"}\r\n");
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
	} else if (strcmp(command, "MODEM") == 0) {
		emit_modem(NULL);
	} else if (strncmp(command, "MODEM ", 6) == 0) {
		emit_modem(command + 6);
	} else if (strcmp(command, "CELLULAR") == 0) {
		emit_cellular(NULL);
	} else if (strncmp(command, "CELLULAR ", 9) == 0) {
		emit_cellular(command + 9);
	} else if (strcmp(command, "NET") == 0) {
		emit_netif(NULL);
	} else if (strncmp(command, "NET ", 4) == 0) {
		emit_netif(command + 4);
	} else if (strcmp(command, "UART") == 0) {
		emit_uart(NULL);
	} else if (strncmp(command, "UART ", 5) == 0) {
		emit_uart(command + 5);
	} else if (strcmp(command, "GPIO") == 0) {
		emit_gpio(NULL);
	} else if (strncmp(command, "GPIO ", 5) == 0) {
		emit_gpio(command + 5);
	} else if (strncmp(command, "DNS ", 4) == 0 || strncmp(command, "TCP ", 4) == 0 ||
		strncmp(command, "UDP ", 4) == 0 || strncmp(command, "HTTP ", 5) == 0 ||
		strncmp(command, "SOCK ", 5) == 0) {
		emit_net(command);
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
#if DUT_UART
	uart_ports_init();
#endif
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
