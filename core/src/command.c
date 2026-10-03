// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <string.h>

#include "metalclaw/command.h"

void mclw_command_init(struct mclw_command_image *image, uint8_t read_register_opcode,
    uint8_t read_buffer_opcode) {
    memset(image, 0, sizeof(*image));
    image->read_register_opcode = read_register_opcode;
    image->read_buffer_opcode = read_buffer_opcode;
    image->dirty = MCLW_COMMAND_DIRTY_STATUS | MCLW_COMMAND_DIRTY_RESPONSES |
        MCLW_COMMAND_DIRTY_PAGES | MCLW_COMMAND_DIRTY_BUFFER;
}

void mclw_command_set_on_window(struct mclw_command_image *image, mclw_command_on_window_t fn,
    void *ctx) {
    image->on_window = fn;
    image->on_window_ctx = ctx;
}

int mclw_command_add_page(struct mclw_command_image *image, uint8_t page) {
    for (uint8_t i = 0; i < image->page_count; i++) {
        if (image->page_number[i] == page) {
            return 0;
        }
    }
    if (image->page_count >= MCLW_COMMAND_PAGES_MAX) {
        return -ENOSPC;
    }
    image->page_number[image->page_count++] = page;
    image->dirty |= MCLW_COMMAND_DIRTY_PAGES;
    return 0;
}

static struct mclw_command_response *find_response(struct mclw_command_image *image, uint8_t opcode) {
    for (uint8_t i = 0; i < image->response_count; i++) {
        if (image->responses[i].opcode == opcode) {
            return &image->responses[i];
        }
    }
    return NULL;
}

int mclw_command_add_response(struct mclw_command_image *image, uint8_t opcode) {
    if (find_response(image, opcode) != NULL) {
        return 0;
    }
    if (image->response_count >= MCLW_COMMAND_RESPONSES_MAX) {
        return -ENOSPC;
    }
    image->responses[image->response_count++].opcode = opcode;
    image->dirty |= MCLW_COMMAND_DIRTY_RESPONSES;
    return 0;
}

void mclw_command_set_status(struct mclw_command_image *image, uint8_t status) {
    if (image->status != status) {
        image->status = status;
        image->dirty |= MCLW_COMMAND_DIRTY_STATUS;
    }
}

int mclw_command_set_response(struct mclw_command_image *image, uint8_t opcode, const uint8_t *bytes,
    size_t len) {
    struct mclw_command_response *r = find_response(image, opcode);

    if (r == NULL) {
        return -ENOENT;
    }
    if (len > MCLW_COMMAND_RESPONSE_MAX) {
        len = MCLW_COMMAND_RESPONSE_MAX;
    }
    memcpy(r->bytes, bytes, len);
    memset(r->bytes + len, 0, MCLW_COMMAND_RESPONSE_MAX - len);
    image->dirty |= MCLW_COMMAND_DIRTY_RESPONSES;
    return 0;
}

static uint8_t *page_of(const struct mclw_command_image *image, uint8_t page) {
    for (uint8_t i = 0; i < image->page_count; i++) {
        if (image->page_number[i] == page) {
            return (uint8_t *)image->pages[i];
        }
    }
    return NULL;
}

uint8_t *mclw_command_register(struct mclw_command_image *image, uint16_t address) {
    uint8_t *page = page_of(image, (uint8_t)(address >> 8));

    if (page == NULL) {
        return NULL;
    }
    image->dirty |= MCLW_COMMAND_DIRTY_PAGES;
    return &page[address & 0xffu];
}

uint8_t mclw_command_get_register(const struct mclw_command_image *image, uint16_t address) {
    const uint8_t *page = page_of(image, (uint8_t)(address >> 8));

    return page == NULL ? 0u : page[address & 0xffu];
}

size_t mclw_command_set_registers(struct mclw_command_image *image, uint16_t address,
    const uint8_t *bytes, size_t len) {
    size_t written = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t *reg = mclw_command_register(image, (uint16_t)(address + i));

        if (reg != NULL) {
            *reg = bytes[i];
            written++;
        }
    }
    return written;
}

void mclw_command_write_buffer(struct mclw_command_image *image, uint8_t offset, const uint8_t *bytes,
    size_t len) {
    for (size_t i = 0; i < len; i++) {
        image->buffer[(uint8_t)(offset + i)] = bytes[i];
    }
    image->dirty |= MCLW_COMMAND_DIRTY_BUFFER;
}

void mclw_command_read_buffer(const struct mclw_command_image *image, uint8_t offset, uint8_t *bytes,
    size_t len) {
    for (size_t i = 0; i < len; i++) {
        bytes[i] = image->buffer[(uint8_t)(offset + i)];
    }
}

void mclw_command_publish(struct mclw_command_image *image) {
    if (image->notify != NULL) {
        image->notify(image, image->notify_ctx);
    }
}
