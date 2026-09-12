// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "mailbox.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
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
			"MCLW-HIL/1 {\"test\":\"test_sensor\",\"status\":\"error\",\"errno\":%d,"
			"\"init_res\":%u}\r\n", -rc, (unsigned int)dev->state->init_res);
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

/*
 * `IMU [count]`: `count` fetches from the `imu` alias through Zephyr's own
 * driver, reporting the first reading's accel, gyro and die temperature in
 * the sensor API's micro-units and how many later fetches differed from it.
 */
#if defined(CONFIG_SENSOR) && DT_NODE_EXISTS(DT_ALIAS(imu))
static int fetch_imu(const struct device *dev, int64_t out[7])
{
	struct sensor_value v[3];
	int rc = sensor_sample_fetch(dev);

	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v);
		for (int i = 0; rc == 0 && i < 3; i++) {
			out[i] = sensor_value_to_micro(&v[i]);
		}
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, v);
		for (int i = 0; rc == 0 && i < 3; i++) {
			out[3 + i] = sensor_value_to_micro(&v[i]);
		}
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_DIE_TEMP, v);
		out[6] = sensor_value_to_micro(&v[0]);
	}
	return rc;
}

static void emit_imu(const char *args)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(imu));
	unsigned long count = args != NULL ? strtoul(args, NULL, 10) : 1;
	unsigned long mismatches = 0;
	int64_t first[7];
	int64_t now[7];
	int rc = device_is_ready(dev) ? 0 : -ENODEV;

	if (count == 0) {
		rc = -EINVAL;
	}
	for (unsigned long i = 0; rc == 0 && i < count; i++) {
		rc = fetch_imu(dev, i == 0 ? first : now);
		if (rc == 0 && i > 0 && memcmp(first, now, sizeof(first)) != 0) {
			mismatches++;
		}
	}
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_imu\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_imu\",\"status\":\"ok\","
			"\"accel_ums2\":[%lld,%lld,%lld],\"gyro_urads\":[%lld,%lld,%lld],"
			"\"temp_uc\":%lld,\"count\":%lu,\"mismatches\":%lu}\r\n",
			(long long)first[0], (long long)first[1], (long long)first[2],
			(long long)first[3], (long long)first[4], (long long)first[5],
			(long long)first[6], count, mismatches);
	}
	mclw_debug_port_write(result_line);
}
#else
static void emit_imu(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_imu\",\"status\":\"error\",\"detail\":\"no imu in this image\"}\r\n");
	mclw_debug_port_write(result_line);
}
#endif

/*
 * `ADC [hz] [bytes] [count] [gap_us]`: clock `count` windows of `bytes` bytes
 * at `hz` on the `adc` alias, a converter with no in-tree driver, `gap_us`
 * apart, and report the first frame, the 12-bit sample it carries in the
 * ADS7883 layout, and how many of the later windows differed from the first.
 * Defaults: the overlay's clock, 2 bytes, 1 window, no gap.
 */
#if defined(CONFIG_SPI) && DT_NODE_EXISTS(DT_ALIAS(adc))
#define ADC_BYTES_MAX 16U

static const struct spi_dt_spec adc_spec = SPI_DT_SPEC_GET(DT_ALIAS(adc),
	SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));

/*
 * The PL022 driver reconfigures only when the config pointer changes, so
 * two configs alternate: every command is then applied with its own clock.
 */
static struct spi_config adc_configs[2];
static unsigned int adc_config_index;

static void emit_adc(const char *args)
{
	struct spi_config *config_slot = &adc_configs[adc_config_index ^= 1U];
	struct spi_config config = adc_spec.config;
	uint8_t transmit[ADC_BYTES_MAX] = {0};
	uint8_t first[ADC_BYTES_MAX];
	uint8_t receive[ADC_BYTES_MAX];
	char last_hex[2 * ADC_BYTES_MAX + 1];
	const struct spi_buf tx_buf = {.buf = transmit, .len = 0};
	const struct spi_buf rx_buf = {.buf = receive, .len = 0};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};
	unsigned long hz = 0;
	unsigned long bytes = 2;
	unsigned long count = 1;
	unsigned long gap_us = 0;
	unsigned long mismatches = 0;
	uint32_t started = 0;
	uint32_t elapsed_us = 0;
	unsigned long first_bad = 0;
	uint8_t bad[ADC_BYTES_MAX] = {0};
	char bad_hex[2 * ADC_BYTES_MAX + 1];
	char frame_hex[2 * ADC_BYTES_MAX + 1];
	int rc = 0;

	if (args != NULL) {
		char *end;

		hz = strtoul(args, &end, 10);
		if (*end != '\0') {
			bytes = strtoul(end, &end, 10);
		}
		if (*end != '\0') {
			count = strtoul(end, &end, 10);
		}
		if (*end != '\0') {
			gap_us = strtoul(end, &end, 10);
		}
	}
	if (bytes == 0 || bytes > ADC_BYTES_MAX || count == 0) {
		rc = -EINVAL;
	}
	if (hz != 0) {
		config.frequency = (uint32_t)hz;
	}
	if (rc == 0 && !spi_is_ready_dt(&adc_spec)) {
		rc = -ENODEV;
	}
	for (unsigned long i = 0; rc == 0 && i < count; i++) {
		struct spi_buf tx_now = tx_buf;
		struct spi_buf rx_now = rx_buf;
		struct spi_buf_set tx_set = tx;
		struct spi_buf_set rx_set = rx;

		tx_now.len = bytes;
		rx_now.len = bytes;
		tx_set.buffers = &tx_now;
		rx_set.buffers = &rx_now;
		memset(receive, 0, sizeof(receive));
		if (i > 0 && gap_us > 0) {
			k_busy_wait((uint32_t)gap_us);
		}
		*config_slot = config;
		if (i == 0) {
			started = k_cycle_get_32();
		}
		rc = spi_transceive(adc_spec.bus, config_slot, &tx_set, &rx_set);
		if (rc != 0) {
			break;
		}
		if (i == 0) {
			memcpy(first, receive, bytes);
		} else if (memcmp(first, receive, bytes) != 0) {
			if (mismatches == 0) {
				first_bad = i;
				memcpy(bad, receive, bytes);
			}
			mismatches++;
		}
	}
	elapsed_us = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - started);
	if (rc != 0) {
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_adc\",\"status\":\"error\",\"errno\":%d}\r\n", -rc);
	} else {
		unsigned int frame = ((unsigned int)first[0] << 8) | first[1];

		for (unsigned long i = 0; i < bytes; i++) {
			snprintf(&frame_hex[2 * i], 3, "%02x", first[i]);
			snprintf(&last_hex[2 * i], 3, "%02x", receive[i]);
			snprintf(&bad_hex[2 * i], 3, "%02x", bad[i]);
		}
		snprintf(result_line, sizeof(result_line),
			"MCLW-HIL/1 {\"test\":\"test_adc\",\"status\":\"ok\","
			"\"hz\":%u,\"frame\":\"%s\",\"sample\":%u,\"leading\":%u,\"trailing\":%u,"
			"\"count\":%lu,\"mismatches\":%lu,\"last\":\"%s\",\"elapsed_us\":%u,"
			"\"first_bad\":%lu,\"bad\":\"%s\"}\r\n",
			(unsigned int)config.frequency, frame_hex, (frame >> 2) & 0x0fffU, frame >> 14,
			frame & 0x3U, count, mismatches, last_hex, (unsigned int)elapsed_us, first_bad, bad_hex);
	}
	mclw_debug_port_write(result_line);
}
#else
static void emit_adc(const char *args)
{
	(void)args;
	snprintf(result_line, sizeof(result_line),
		"MCLW-HIL/1 {\"test\":\"test_adc\",\"status\":\"error\",\"detail\":\"no adc in this image\"}\r\n");
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

static const struct spi_dt_spec raw_spec = SPI_DT_SPEC_GET(DT_ALIAS(raw),
	SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));

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
		emit_sensor();
	} else if (strcmp(command, "IMU") == 0) {
		emit_imu(NULL);
	} else if (strncmp(command, "IMU ", 4) == 0) {
		emit_imu(command + 4);
	} else if (strcmp(command, "ADC") == 0) {
		emit_adc(NULL);
	} else if (strncmp(command, "ADC ", 4) == 0) {
		emit_adc(command + 4);
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
