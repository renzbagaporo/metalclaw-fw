// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/runtime.h"
#include "metalclaw/version.h"

#include "dap.h"

#define DAP_ID_INFO 0x00u
#define DAP_ID_HOST_STATUS 0x01u
#define DAP_ID_CONNECT 0x02u
#define DAP_ID_DISCONNECT 0x03u
#define DAP_ID_TRANSFER_CONFIGURE 0x04u
#define DAP_ID_TRANSFER 0x05u
#define DAP_ID_TRANSFER_BLOCK 0x06u
#define DAP_ID_TRANSFER_ABORT 0x07u
#define DAP_ID_WRITE_ABORT 0x08u
#define DAP_ID_DELAY 0x09u
#define DAP_ID_RESET_TARGET 0x0au
#define DAP_ID_SWJ_PINS 0x10u
#define DAP_ID_SWJ_CLOCK 0x11u
#define DAP_ID_SWJ_SEQUENCE 0x12u
#define DAP_ID_SWD_CONFIGURE 0x13u
#define DAP_ID_SWD_SEQUENCE 0x1du
#define DAP_ID_INVALID 0xffu

#define DAP_INFO_VENDOR 0x01u
#define DAP_INFO_PRODUCT 0x02u
#define DAP_INFO_SERIAL 0x03u
#define DAP_INFO_FIRMWARE 0x04u
#define DAP_INFO_CAPABILITIES 0xf0u
#define DAP_INFO_PACKET_COUNT 0xfeu
#define DAP_INFO_PACKET_SIZE 0xffu

#define DAP_STATUS_OK 0x00u
#define DAP_STATUS_ERROR 0xffu

#define DAP_CONNECT_FAILED 0x00u
#define DAP_CONNECT_DEFAULT 0x00u
#define DAP_CONNECT_SWD 0x01u

#define DAP_CAP_SWD 0x01u

#define DAP_TRANSFER_APnDP (1u << 0)
#define DAP_TRANSFER_RnW (1u << 1)
#define DAP_TRANSFER_A2 (1u << 2)
#define DAP_TRANSFER_A3 (1u << 3)
#define DAP_TRANSFER_VALUE_MATCH (1u << 4)
#define DAP_TRANSFER_MATCH_MASK (1u << 5)
#define DAP_TRANSFER_TIMESTAMP (1u << 7)

#define DAP_REQUEST_RDBUFF (DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3)
#define DAP_REQUEST_DP_ABORT 0x00u

#define DAP_ACK_NONE 0x07u

#define DAP_RESET_TARGET_US 2000u
#define DAP_SWJ_PINS_WAIT_MAX_US 3000000u

static const char probe_vendor[] = "Metalclaw";
static const char probe_product[] = "Metalclaw";

static void note_error(struct mclw_dap *dap, uint8_t request, uint16_t seq, int16_t result) {
    dap->errors++;
    mclw_trace(dap->inst, MCLW_TRACE_ERROR, request, seq, result);
}

static bool wire_ready(const struct mclw_dap *dap) {
    return dap->enabled && dap->swd != NULL;
}

static void store_word(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static uint32_t load_word(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static int response_need(size_t capacity, size_t need, size_t *length) {
    if (capacity < need) {
        return -EMSGSIZE;
    }
    *length = need;
    return 0;
}

static int response_status(uint8_t *response, size_t capacity, uint8_t command, uint8_t status,
    size_t *length) {
    int ret = response_need(capacity, 2u, length);
    if (ret != 0) {
        return ret;
    }
    response[0] = command;
    response[1] = status;
    return 0;
}

static const char *info_string(uint8_t info) {
    switch (info) {
        case DAP_INFO_VENDOR:
            return probe_vendor;
        case DAP_INFO_PRODUCT:
            return probe_product;
        case DAP_INFO_SERIAL:
            return mclw_hal_usb_serial();
        case DAP_INFO_FIRMWARE:
            return MCLW_VERSION_STRING;
        default:
            return NULL;
    }
}

static int response_info(const uint8_t *request, size_t request_length, uint8_t *response,
    size_t capacity, size_t *length) {
    if (request_length < 2u) {
        return response_status(response, capacity, DAP_ID_INFO, DAP_STATUS_ERROR, length);
    }

    uint8_t info = request[1];
    if (info == DAP_INFO_CAPABILITIES || info == DAP_INFO_PACKET_COUNT) {
        int ret = response_need(capacity, 3u, length);
        if (ret != 0) {
            return ret;
        }
        response[0] = DAP_ID_INFO;
        response[1] = 1u;
        response[2] = info == DAP_INFO_CAPABILITIES ? (uint8_t)DAP_CAP_SWD :
            (uint8_t)MCLW_DAP_PACKET_COUNT;
        return 0;
    }
    if (info == DAP_INFO_PACKET_SIZE) {
        int ret = response_need(capacity, 4u, length);
        if (ret != 0) {
            return ret;
        }
        response[0] = DAP_ID_INFO;
        response[1] = 2u;
        response[2] = (uint8_t)(MCLW_DAP_PACKET_SIZE & 0xffu);
        response[3] = (uint8_t)(MCLW_DAP_PACKET_SIZE >> 8);
        return 0;
    }

    const char *value = info_string(info);
    if (value == NULL) {
        int ret = response_need(capacity, 2u, length);
        if (ret != 0) {
            return ret;
        }
        response[0] = DAP_ID_INFO;
        response[1] = 0u;
        return 0;
    }

    size_t value_length = strlen(value);
    if (value_length > 0xfeu || value_length + 3u > capacity) {
        return -EMSGSIZE;
    }
    response[0] = DAP_ID_INFO;
    response[1] = (uint8_t)(value_length + 1u);
    memcpy(&response[2], value, value_length);
    response[2u + value_length] = '\0';
    *length = value_length + 3u;
    return 0;
}

static int transfer_with_retry(struct mclw_dap *dap, uint8_t request, uint32_t *value,
    uint8_t *ack) {
    uint32_t attempt = 0u;
    int ret;

    do {
        ret = mclw_hal_swd_transfer(dap->swd, request, value, dap->idle_cycles, ack);
        if (ret != 0 || *ack != MCLW_SWD_ACK_WAIT) {
            return ret;
        }
        attempt++;
    } while (attempt <= (uint32_t)dap->retry_count);
    return 0;
}

static int write_abort(struct mclw_dap *dap, uint32_t value) {
    uint32_t data = value;
    uint8_t ack = DAP_ACK_NONE;
    int ret = mclw_hal_swd_transfer(dap->swd, DAP_REQUEST_DP_ABORT, &data, dap->idle_cycles,
        &ack);

    if (ret != 0) {
        return ret;
    }
    return ack == MCLW_SWD_ACK_OK ? 0 : -EIO;
}

static int process_transfer(struct mclw_dap *dap, const uint8_t *request, size_t request_length,
    uint8_t *response, size_t capacity, size_t *length, bool block) {
    uint8_t command = request[0];
    uint32_t transfer_count;
    size_t request_offset;
    size_t data_offset;

    if (block) {
        if (request_length < 5u) {
            return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
        }
        transfer_count = (uint32_t)request[2] | ((uint32_t)request[3] << 8);
        request_offset = 4u;
        data_offset = 5u;
    } else {
        if (request_length < 3u) {
            return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
        }
        transfer_count = request[2];
        request_offset = 3u;
        data_offset = request_offset;
    }
    if (transfer_count == 0u ||
        (block ? request_length < data_offset : request_length < request_offset + 1u)) {
        return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
    }
    if (capacity < (block ? 4u : 3u)) {
        return -EMSGSIZE;
    }

    size_t response_offset = block ? 4u : 3u;
    size_t cursor = data_offset;
    uint32_t completed = 0u;
    uint32_t pending_count = 0u;
    size_t pending_start = 0u;
    uint8_t status = DAP_STATUS_ERROR;

    response[0] = command;
    response[1] = 0u;
    if (block) {
        response[2] = 0u;
        response[3] = DAP_STATUS_ERROR;
    } else {
        response[2] = DAP_STATUS_ERROR;
    }

    for (uint32_t i = 0u; i < transfer_count; i++) {
        uint8_t transfer_request;
        uint8_t ack = DAP_ACK_NONE;
        uint32_t value = 0u;
        bool read;
        bool ap_read;
        int ret;

        if (block) {
            transfer_request = request[request_offset];
        } else {
            if (cursor >= request_length) {
                status = DAP_STATUS_ERROR;
                break;
            }
            transfer_request = request[cursor++];
        }
        read = (transfer_request & DAP_TRANSFER_RnW) != 0u;
        ap_read = read && (transfer_request & DAP_TRANSFER_APnDP) != 0u;

        if (!ap_read && pending_count != 0u) {
            uint32_t posted_value = 0u;
            uint8_t posted_ack = DAP_ACK_NONE;

            ret = transfer_with_retry(dap, DAP_REQUEST_RDBUFF, &posted_value, &posted_ack);
            if (ret != 0) {
                note_error(dap, DAP_REQUEST_RDBUFF, (uint16_t)i, (int16_t)ret);
                status = DAP_STATUS_ERROR;
                break;
            }
            if (posted_ack != MCLW_SWD_ACK_OK) {
                note_error(dap, DAP_REQUEST_RDBUFF, (uint16_t)i, (int16_t)posted_ack);
                status = posted_ack;
                break;
            }
            store_word(&response[pending_start + (pending_count - 1u) * 4u], posted_value);
            pending_count = 0u;
        }

        if ((transfer_request &
            (DAP_TRANSFER_VALUE_MATCH | DAP_TRANSFER_MATCH_MASK | DAP_TRANSFER_TIMESTAMP)) != 0u) {
            mclw_trace(dap->inst, MCLW_TRACE_ERROR, transfer_request, (uint16_t)i,
                -ENOTSUP);
            status = DAP_STATUS_ERROR;
            break;
        }
        if (!read) {
            if (cursor + 4u > request_length) {
                status = DAP_STATUS_ERROR;
                break;
            }
            value = load_word(&request[cursor]);
            cursor += 4u;
        }

        ret = transfer_with_retry(dap, (uint8_t)(transfer_request & 0x0fu), &value, &ack);
        if (ret != 0) {
            note_error(dap, transfer_request, (uint16_t)i, (int16_t)ret);
            status = DAP_STATUS_ERROR;
            break;
        }
        status = ack;
        if (ack == MCLW_SWD_ACK_OK) {
            completed++;
        }
        if (read && ack == MCLW_SWD_ACK_OK) {
            if (response_offset + 4u > capacity) {
                return -EMSGSIZE;
            }
            if (ap_read) {
                if (pending_count == 0u) {
                    pending_start = response_offset;
                } else {
                    store_word(&response[pending_start + (pending_count - 1u) * 4u], value);
                }
                pending_count++;
                response_offset += 4u;
            } else {
                store_word(&response[response_offset], value);
                response_offset += 4u;
            }
        }
        if (ack != MCLW_SWD_ACK_OK) {
            note_error(dap, transfer_request, (uint16_t)i, (int16_t)ack);
            break;
        }
    }

    if (pending_count != 0u) {
        uint32_t posted_value = 0u;
        uint8_t posted_ack = DAP_ACK_NONE;
        int ret = transfer_with_retry(dap, DAP_REQUEST_RDBUFF, &posted_value, &posted_ack);

        if (ret == 0 && posted_ack == MCLW_SWD_ACK_OK) {
            store_word(&response[pending_start + (pending_count - 1u) * 4u], posted_value);
        } else {
            note_error(dap, DAP_REQUEST_RDBUFF, (uint16_t)transfer_count,
                ret != 0 ? (int16_t)ret : (int16_t)posted_ack);
            if (status == MCLW_SWD_ACK_OK) {
                status = ret != 0 ? (uint8_t)DAP_STATUS_ERROR : posted_ack;
            }
        }
    }

    response[1] = (uint8_t)completed;
    if (block) {
        response[2] = (uint8_t)(completed >> 8);
        response[3] = status;
    } else {
        response[2] = status;
    }
    *length = response_offset;
    dap->transfers += completed;
    mclw_trace(dap->inst, MCLW_TRACE_COMMAND, command, (uint16_t)completed,
        (int16_t)status);
    return 0;
}

static int process_sequence(struct mclw_dap *dap, const uint8_t *request, size_t request_length,
    uint8_t *response, size_t capacity, size_t *length, bool swd) {
    uint8_t command = request[0];
    uint32_t bits;
    size_t bytes;
    int ret = 0;

    if (request_length < (swd ? 3u : 2u)) {
        return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
    }

    if (!swd) {
        bits = request[1] == 0u ? 256u : request[1];
        bytes = (bits + 7u) / 8u;
        if (request_length < 2u + bytes) {
            return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
        }
        ret = mclw_hal_swd_output_sequence(dap->swd, bits, &request[2]);
        if (ret != 0) {
            note_error(dap, command, 0u, (int16_t)ret);
        }
        return response_status(response, capacity, command,
            ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR, length);
    }

    uint32_t sequence_count = request[1];
    size_t request_cursor = 2u;
    size_t response_cursor = 2u;

    if (sequence_count == 0u) {
        return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
    }
    if (capacity < 2u) {
        return -EMSGSIZE;
    }
    response[0] = command;
    response[1] = DAP_STATUS_OK;

    for (uint32_t sequence = 0u; sequence < sequence_count; sequence++) {
        uint8_t info;

        if (request_cursor >= request_length) {
            ret = -EINVAL;
            break;
        }
        info = request[request_cursor++];
        if ((info & 0x40u) != 0u) {
            ret = -EINVAL;
            break;
        }
        bits = info & 0x3fu;
        bits = bits == 0u ? 64u : bits;
        bytes = (bits + 7u) / 8u;
        if ((info & 0x80u) != 0u) {
            if (response_cursor + bytes > capacity) {
                return -EMSGSIZE;
            }
            ret = mclw_hal_swd_input_sequence(dap->swd, bits, &response[response_cursor]);
            response_cursor += bytes;
        } else {
            if (request_cursor + bytes > request_length) {
                ret = -EINVAL;
                break;
            }
            ret = mclw_hal_swd_output_sequence(dap->swd, bits, &request[request_cursor]);
            request_cursor += bytes;
        }
        if (ret != 0) {
            note_error(dap, command, (uint16_t)sequence, (int16_t)ret);
            break;
        }
    }

    response[1] = ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR;
    *length = response_cursor;
    return 0;
}

static int process_command(struct mclw_dap *dap, const uint8_t *request, size_t request_length,
    uint8_t *response, size_t capacity, size_t *length) {
    uint8_t command = request[0];
    int ret;

    switch (command) {
        case DAP_ID_INFO:
            return response_info(request, request_length, response, capacity, length);

        case DAP_ID_HOST_STATUS:
            return response_status(response, capacity, command, DAP_STATUS_OK, length);

        case DAP_ID_CONNECT:
            if (request_length < 2u || (request[1] != DAP_CONNECT_DEFAULT &&
                request[1] != DAP_CONNECT_SWD) || !wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_CONNECT_FAILED, length);
            }
            dap->connected = true;
            return response_status(response, capacity, command, DAP_CONNECT_SWD, length);

        case DAP_ID_DISCONNECT:
            dap->connected = false;
            return response_status(response, capacity, command, DAP_STATUS_OK, length);

        case DAP_ID_TRANSFER_CONFIGURE:
            if (request_length < 6u) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            dap->idle_cycles = request[1];
            dap->retry_count = (uint16_t)((uint16_t)request[2] | ((uint16_t)request[3] << 8));
            return response_status(response, capacity, command, DAP_STATUS_OK, length);

        case DAP_ID_TRANSFER:
        case DAP_ID_TRANSFER_BLOCK:
            if (!wire_ready(dap) || !dap->connected) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            return process_transfer(dap, request, request_length, response, capacity, length,
                command == DAP_ID_TRANSFER_BLOCK);

        case DAP_ID_TRANSFER_ABORT:
            *length = 0u;
            return 0;

        case DAP_ID_WRITE_ABORT:
            if (request_length < 6u || !wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            ret = write_abort(dap, load_word(&request[2]));
            if (ret != 0) {
                note_error(dap, DAP_REQUEST_DP_ABORT, 0u, (int16_t)ret);
            }
            return response_status(response, capacity, command,
                ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR, length);

        case DAP_ID_DELAY:
            if (request_length < 3u) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            mclw_hal_delay_us((uint32_t)request[1] | ((uint32_t)request[2] << 8));
            return response_status(response, capacity, command, DAP_STATUS_OK, length);

        case DAP_ID_RESET_TARGET:
            if (!wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            ret = mclw_hal_swd_set_pins(dap->swd, MCLW_SWD_PIN_NRESET, 0u);
            mclw_hal_delay_us(DAP_RESET_TARGET_US);
            if (ret == 0) {
                ret = mclw_hal_swd_set_pins(dap->swd, MCLW_SWD_PIN_NRESET,
                    MCLW_SWD_PIN_NRESET);
            }
            if (response_need(capacity, 3u, length) != 0) {
                return -EMSGSIZE;
            }
            response[0] = command;
            response[1] = ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR;
            response[2] = ret == 0 ? 1u : 0u;
            return 0;

        case DAP_ID_SWJ_PINS: {
            if (request_length < 7u || !wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            uint32_t wait_us = load_word(&request[3]);
            ret = mclw_hal_swd_set_pins(dap->swd, request[2], request[1]);
            mclw_hal_delay_us(wait_us > DAP_SWJ_PINS_WAIT_MAX_US ?
                DAP_SWJ_PINS_WAIT_MAX_US : wait_us);
            if (ret != 0) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            if (response_need(capacity, 2u, length) != 0) {
                return -EMSGSIZE;
            }
            response[0] = command;
            if (mclw_hal_swd_get_pins(dap->swd, &response[1]) != 0) {
                response[1] = DAP_STATUS_ERROR;
            }
            return 0;
        }

        case DAP_ID_SWJ_CLOCK: {
            if (request_length < 5u || !wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            ret = mclw_dap_set_clock(dap, load_word(&request[1]));
            return response_status(response, capacity, command,
                ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR, length);
        }

        case DAP_ID_SWJ_SEQUENCE:
            if (!wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            return process_sequence(dap, request, request_length, response, capacity, length, false);

        case DAP_ID_SWD_CONFIGURE:
            if (request_length < 2u || (request[1] & 0xf8u) != 0u || !wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            dap->turnaround = (uint8_t)((request[1] & 0x03u) + 1u);
            dap->data_phase = (request[1] & 0x04u) != 0u;
            ret = mclw_hal_swd_configure(dap->swd, dap->turnaround, dap->data_phase);
            return response_status(response, capacity, command,
                ret == 0 ? (uint8_t)DAP_STATUS_OK : (uint8_t)DAP_STATUS_ERROR, length);

        case DAP_ID_SWD_SEQUENCE:
            if (!wire_ready(dap)) {
                return response_status(response, capacity, command, DAP_STATUS_ERROR, length);
            }
            return process_sequence(dap, request, request_length, response, capacity, length, true);

        default:
            if (capacity < 1u) {
                return -EMSGSIZE;
            }
            response[0] = DAP_ID_INVALID;
            *length = 1u;
            return 0;
    }
}

void mclw_dap_init(struct mclw_dap *dap, struct mclw_instance *inst,
    struct mclw_swd *swd) {
    memset(dap, 0, sizeof(*dap));
    dap->inst = inst;
    dap->swd = swd;
    dap->clock_hz = MCLW_DAP_CLOCK_MIN_HZ;
    dap->turnaround = 1u;
}

int mclw_dap_enable(struct mclw_dap *dap, bool enable) {
    int ret;

    if (dap->swd == NULL) {
        return -ENODEV;
    }
    if (enable == dap->enabled) {
        return 0;
    }
    if (!enable) {
        dap->enabled = false;
        dap->connected = false;
        return mclw_hal_swd_port_off(dap->swd);
    }

    ret = mclw_hal_swd_port_on(dap->swd);
    if (ret != 0) {
        return ret;
    }
    ret = mclw_hal_swd_set_clock(dap->swd, dap->clock_hz);
    if (ret == 0) {
        ret = mclw_hal_swd_configure(dap->swd, dap->turnaround, dap->data_phase);
    }
    if (ret != 0) {
        (void)mclw_hal_swd_port_off(dap->swd);
        return ret;
    }
    dap->enabled = true;
    return 0;
}

int mclw_dap_set_clock(struct mclw_dap *dap, uint32_t hz) {
    int ret;

    if (hz == 0u || dap->swd == NULL) {
        return -EINVAL;
    }
    ret = mclw_hal_swd_set_clock(dap->swd, hz);
    if (ret == 0) {
        dap->clock_hz = hz;
    }
    return ret;
}

int mclw_dap_reset(struct mclw_dap *dap, bool factory) {
    int ret;

    dap->connected = false;
    dap->idle_cycles = 0u;
    dap->retry_count = 0u;
    dap->turnaround = 1u;
    dap->data_phase = false;
    dap->transfers = 0u;
    dap->errors = 0u;
    if (factory) {
        dap->clock_hz = MCLW_DAP_CLOCK_MIN_HZ;
    }
    if (!wire_ready(dap)) {
        return 0;
    }
    ret = mclw_hal_swd_configure(dap->swd, dap->turnaround, dap->data_phase);
    if (ret == 0 && factory) {
        ret = mclw_hal_swd_set_clock(dap->swd, dap->clock_hz);
    }
    return ret;
}

size_t mclw_dap_process(struct mclw_dap *dap, const uint8_t *request,
    size_t request_length) {
    size_t length = 0u;

    if (dap == NULL || request == NULL || request_length == 0u) {
        return 0u;
    }
    if (process_command(dap, request, request_length, dap->response, sizeof(dap->response),
        &length) != 0) {
        return 0u;
    }
    return length;
}
