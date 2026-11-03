// SPDX-License-Identifier: Apache-2.0 OR MIT
// 3GPP TS 27.010 basic option, the responder side, as Zephyr's
// subsys/modem/modem_cmux.c drives it: a byte-wise frame parser, the frame
// encoder, and the DLCI 0 control channel (SABM, DISC, MSC, CLD, FCon,
// FCoff, Test, NSC). It knows no AT and no PPP: what it opens, closes and
// carries goes to a sink.
//
// C/R follows 27.010 table 1 with the DTE as initiator: the model's
// responses carry C/R 1 in the address, its own commands and its UIH data
// C/R 0. Zephyr drops a DLCI 0 frame whose C/R equals its own once
// connected, and a DLCI UA whose C/R is 0.

#ifndef MCLW_DEVICES_CELLULAR_CMUX_H
#define MCLW_DEVICES_CELLULAR_CMUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMUX_FLAG 0xf9u
#define CMUX_EA 0x01u
#define CMUX_CR 0x02u
#define CMUX_PF 0x10u

#define CMUX_SABM 0x2fu
#define CMUX_UA 0x63u
#define CMUX_DM 0x0fu
#define CMUX_DISC 0x43u
#define CMUX_UIH 0xefu
#define CMUX_UI 0x03u

// Control channel message types, the value bits of the type octet.
#define CMUX_MSG_NSC 0x04u
#define CMUX_MSG_TEST 0x08u
#define CMUX_MSG_PSC 0x10u
#define CMUX_MSG_FCOFF 0x18u
#define CMUX_MSG_FCON 0x28u
#define CMUX_MSG_CLD 0x30u
#define CMUX_MSG_MSC 0x38u

// MSC V.24 signals octet.
#define CMUX_MSC_FC 0x02u
#define CMUX_MSC_RTC 0x04u
#define CMUX_MSC_RTR 0x08u
#define CMUX_MSC_DV 0x80u
#define CMUX_MSC_READY (CMUX_EA | CMUX_MSC_RTC | CMUX_MSC_RTR | CMUX_MSC_DV)

// Zephyr's CONFIG_MODEM_CMUX_MTU for every modem_cellular compatible: it
// drops a longer frame, and AT+CMUX N1 is clamped to it.
#define CMUX_MTU 127u
// Flag, address, control, one length octet, FCS, flag.
#define CMUX_OVERHEAD 6u
#define CMUX_FRAME_MAX (CMUX_MTU + CMUX_OVERHEAD)
// DLCIs 1 .. CMUX_DLCI_MAX may open; 0 is control.
#define CMUX_DLCI_MAX 4u

struct cmux_sink {
    // A DLCI asks to open: true answers UA, false DM.
    bool (*open)(void *ctx, uint8_t dlci);
    void (*close)(void *ctx, uint8_t dlci);
    void (*data)(void *ctx, uint8_t dlci, const uint8_t *bytes, size_t len);
    // A control reply, a whole frame.
    void (*send)(void *ctx, const uint8_t *frame, size_t len);
    // The DTE closed the multiplexer (CLD or DISC on DLCI 0) and the reply
    // has been sent; what follows on the wire is no longer CMUX.
    void (*closedown)(void *ctx);
};

struct cmux_stats {
    uint32_t bad_fcs;
};

struct cmux {
    uint8_t state;
    uint8_t addr;
    uint8_t ctrl;
    uint8_t len;
    uint8_t pos;
    uint8_t fcs;
    // Bit n: DLCI n is open.
    uint8_t open;
    // Bit n: the DTE asked for DLCI n to stop (MSC FC set or RTR clear).
    uint8_t stopped;
    // FCoff: the DTE stopped every DLCI.
    bool all_stopped;
    uint8_t n1;
    uint8_t buf[CMUX_MTU];
    struct cmux_stats stats;
};

// The 27.010 FCS over `len` octets, already complemented.
uint8_t cmux_fcs(const uint8_t *p, size_t len);
// Writes one frame (at most CMUX_FRAME_MAX octets) into `out`; `len` <=
// CMUX_MTU. Returns its length.
size_t cmux_encode(uint8_t *out, uint8_t dlci, bool cr, uint8_t ctrl, const uint8_t *data, size_t len);

// Starts the multiplexer: every DLCI closed, the parser waiting for a flag.
void cmux_start(struct cmux *c, uint8_t n1);
// Parses `len` bytes. Returns how many it took: all of them, or up to and
// including the frame that closed the multiplexer.
size_t cmux_input(struct cmux *c, const uint8_t *bytes, size_t len, const struct cmux_sink *sink, void *ctx);
bool cmux_is_open(const struct cmux *c, uint8_t dlci);
// The DTE has not flow-stopped this DLCI.
bool cmux_can_send(const struct cmux *c, uint8_t dlci);

#endif
