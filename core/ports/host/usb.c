// SPDX-License-Identifier: Apache-2.0 OR MIT
// USB functions for the host port: a serial interface is a pseudo-terminal,
// and with METALCLAW_USB_DIR set, a link there named after its interface
// string points at it, as the rig finds a harness's CDC port by that
// string; a second interface with the same string gets "<string> 2", a
// third "<string> 3", in the order they open. Bytes written to the terminal reach the interface's rx on the
// worker, under the runtime lock. The interface is connected, DTR and RTS
// up, while something has the terminal open (the master reads EIO while
// nothing does); what it sends meanwhile is dropped, as a host that has
// not opened a CDC port never takes it. The line coding is 115200 8N1. A
// vendor interface (the SWD probe) has no place here.

#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/usb.h"

#define INTERFACES_MAX 4u
#define PENDING_MAX 1024u

struct mclw_usb_interface {
    const void *owner;
    const struct mclw_usb_interface_desc *desc;
    mclw_usb_rx_t rx;
    mclw_usb_control_t control;
    void *ctx;
    int master;
    // Whether something has the terminal open.
    bool connected;
    char link[PATH_MAX];
    // Bytes read from the terminal that rx has yet to take.
    uint8_t pending[PENDING_MAX];
    size_t pending_len;
};

static struct mclw_usb_interface interfaces[INTERFACES_MAX];
static bool poll_registered;

static void usb_poll(void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < INTERFACES_MAX; i++) {
        struct mclw_usb_interface *itf = &interfaces[i];

        if (itf->owner == NULL) {
            continue;
        }
        bool was = itf->connected;

        if (itf->pending_len < PENDING_MAX) {
            ssize_t n = read(itf->master, itf->pending + itf->pending_len, PENDING_MAX - itf->pending_len);

            if (n > 0) {
                itf->pending_len += (size_t)n;
                itf->connected = true;
            } else {
                // EIO: no one has the terminal open; EAGAIN: someone does.
                itf->connected = n < 0 && errno == EAGAIN;
            }
        }
        if (itf->connected != was && itf->control != NULL && mclw_hal_try_lock()) {
            itf->control(itf, itf->ctx);
            mclw_hal_unlock();
        }
        if (itf->pending_len == 0u || itf->rx == NULL || !mclw_hal_try_lock()) {
            continue;
        }
        size_t took = itf->rx(itf, itf->pending, itf->pending_len, itf->ctx);

        mclw_hal_unlock();
        if (took > itf->pending_len) {
            took = itf->pending_len;
        }
        memmove(itf->pending, itf->pending + took, itf->pending_len - took);
        itf->pending_len -= took;
    }
}

static bool link_held(const struct mclw_usb_interface *self, const char *link) {
    for (size_t i = 0; i < INTERFACES_MAX; i++) {
        const struct mclw_usb_interface *other = &interfaces[i];

        if (other != self && other->owner != NULL && strcmp(other->link, link) == 0) {
            return true;
        }
    }
    return false;
}

static int open_terminal(struct mclw_usb_interface *itf) {
    struct termios raw;
    const char *name;

    itf->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (itf->master < 0 || grantpt(itf->master) != 0 || unlockpt(itf->master) != 0 ||
        (name = ptsname(itf->master)) == NULL) {
        return -EIO;
    }
    // Raw from the start; the settings outlast this open.
    int slave = open(name, O_RDWR | O_NOCTTY);

    if (slave < 0 || tcgetattr(slave, &raw) != 0) {
        if (slave >= 0) {
            close(slave);
        }
        return -EIO;
    }
    cfmakeraw(&raw);
    (void)tcsetattr(slave, TCSANOW, &raw);
    close(slave);
    (void)fcntl(itf->master, F_SETFL, fcntl(itf->master, F_GETFL) | O_NONBLOCK);
    const char *dir = getenv("METALCLAW_USB_DIR");

    if (dir != NULL && dir[0] != '\0') {
        // A link no open interface holds is left from an earlier run.
        for (unsigned n = 1u; n <= INTERFACES_MAX; n++) {
            if (n == 1u) {
                (void)snprintf(itf->link, sizeof(itf->link), "%s/%s", dir, itf->desc->name);
            } else {
                (void)snprintf(itf->link, sizeof(itf->link), "%s/%s %u", dir, itf->desc->name, n);
            }
            if (!link_held(itf, itf->link)) {
                break;
            }
        }
        (void)unlink(itf->link);
        if (symlink(name, itf->link) != 0) {
            itf->link[0] = '\0';
            return -errno;
        }
    } else {
        fprintf(stderr, "metalclaw: USB interface %s at %s\n", itf->desc->name, name);
    }
    return 0;
}

static void close_terminal(struct mclw_usb_interface *itf) {
    if (itf->link[0] != '\0') {
        (void)unlink(itf->link);
        itf->link[0] = '\0';
    }
    if (itf->master >= 0) {
        close(itf->master);
    }
    itf->master = -1;
    itf->connected = false;
}

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, mclw_usb_control_t control, void *ctx, const void *owner, int *err) {
    struct mclw_usb_interface *itf = NULL;

    if (desc == NULL || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    if (desc->function != MCLW_USB_SERIAL) {
        *err = -ENOTSUP;
        return NULL;
    }
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < INTERFACES_MAX && itf == NULL; i++) {
        if (interfaces[i].owner == NULL && interfaces[i].desc == NULL) {
            itf = &interfaces[i];
            itf->desc = desc;
        }
    }
    mclw_hal_critical_exit(state);
    if (itf == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    itf->rx = rx;
    itf->control = control;
    itf->ctx = ctx;
    itf->pending_len = 0;
    itf->connected = false;
    itf->master = -1;
    itf->link[0] = '\0';
    *err = open_terminal(itf);
    if (*err == 0 && !poll_registered) {
        *err = mclw_hal_poll_register(usb_poll, NULL);
        poll_registered = *err == 0;
    }
    if (*err != 0) {
        close_terminal(itf);
        itf->desc = NULL;
        return NULL;
    }
    state = mclw_hal_critical_enter();
    itf->owner = owner;
    mclw_hal_critical_exit(state);
    return itf;
}

void mclw_hal_usb_close(struct mclw_usb_interface *itf) {
    if (itf == NULL || itf->owner == NULL) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();

    itf->owner = NULL;
    itf->rx = NULL;
    mclw_hal_critical_exit(state);
    close_terminal(itf);
    itf->desc = NULL;
}

size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len) {
    ssize_t n = itf->connected && itf->master >= 0 ? write(itf->master, data, len) : -1;

    return n > 0 ? (size_t)n : 0u;
}

bool mclw_hal_usb_connected(const struct mclw_usb_interface *itf) {
    return itf->owner != NULL && itf->connected;
}

int mclw_hal_usb_line_coding(const struct mclw_usb_interface *itf, struct mclw_usb_line_coding *out) {
    (void)itf;
    out->baud = 115200u;
    out->format = (struct mclw_uart_format){ .data_bits = 8u, .stop_bits = 1u, .parity = MCLW_UART_PARITY_NONE };
    return 0;
}

uint8_t mclw_hal_usb_control_lines(const struct mclw_usb_interface *itf) {
    return itf->connected ? (uint8_t)(MCLW_USB_LINE_DTR | MCLW_USB_LINE_RTS) : 0u;
}

size_t mclw_hal_usb_serial_functions(size_t *free) {
    if (free != NULL) {
        *free = 0;
        for (size_t i = 0; i < INTERFACES_MAX; i++) {
            *free += interfaces[i].owner == NULL && interfaces[i].desc == NULL ? 1u : 0u;
        }
    }
    return INTERFACES_MAX;
}

const char *mclw_hal_usb_serial(void) {
    return "host";
}
