// SPDX-License-Identifier: Apache-2.0 OR MIT
// Links between two pins, each one PIO state machine with no CPU or DMA
// once it runs.
//
// follow  `mov pins, pins` on every cycle: the output takes the input's
//         level two synchronizer cycles and one instruction later, about
//         25 ns at 125 MHz, with one cycle of jitter.
// join    an open-drain line in two halves, A and B. Nothing above the
//         halves tells a low that something on a side drives from one the
//         link drives itself, so the program remembers which side it
//         drives: a low found on an undriven side is forwarded to the other
//         side until it ends, and once the link lets go of the other side
//         it waits out SETTLE for that side to rise before it looks again;
//         a side still low then is held by its own parts, and is forwarded
//         the other way. A side's release can therefore show the other side
//         high for SETTLE before a part that held it is seen: harmless on
//         SDA while SCL is low, and for SCL it lands only on a target that
//         is still stretching the clock when the controller lets it go.
// clock   a join for an I2C clock line, kept as one wire through a
//         target's clock stretching. The join lets go of the side it drove
//         and waits out SETTLE with neither side held, which shows the
//         first side the line high meanwhile: on SCL, a controller letting
//         go while the target still stretches sees a clock edge, and the
//         target's reply comes a bit late. The clock link instead keeps the
//         first side low until it sees the released side up; a side still
//         low after SETTLE is held by its own parts, and that low is
//         forwarded without the line ever showing high. A side the link
//         lets go is looked at again only once it has risen, or SETTLE has
//         passed. The handover shows a side high for two PIO cycles at
//         most, under an I2C input's 50 ns spike filter (UM10204 table 10,
//         tSP). 31 instructions: a PIO block of its own. Not for SDA: a controller
//         letting SDA go for a STOP would find it held low and take that
//         for lost arbitration.
//
// The A of a join or clock link is its side-set pin (driven as a pin direction) and its
// JMP pin; B is its SET pin and its IN pin.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "metalclaw/hal/gpio.h"
#include "gpio_internal.h"
#include "pio_window.h"

#define LINK_MAX 4u

#define FOLLOW_LENGTH 1u
#define JOIN_LENGTH 12u
#define JOIN_TEST_B 0u
#define JOIN_A_WAIT 5u
#define JOIN_RELEASE_B 7u
#define JOIN_SETTLE 8u
#define JOIN_SETTLE_LOOP 9u
#define JOIN_B_DRIVEN 10u
// (SETTLE_COUNT + 1) x (SETTLE_DELAY + 1) cycles: 256, 2.0 us at 125 MHz
// and 1.7 us at 150 MHz. That outlasts the rise of a line held up only by
// the pad pull-ups of both chips (50-80 kOhm on the RP2040) over a few
// tens of pF, and stays under the 4.0 us minimum SCL high time of a
// 100 kHz bus (UM10204 table 10).
#define JOIN_SETTLE_COUNT 15u
#define JOIN_SETTLE_DELAY 15u
#define CLOCK_LENGTH 31u
#define CLOCK_TEST_B 0u
#define CLOCK_A_LOW 4u
#define CLOCK_A_WAIT 5u
#define CLOCK_A_RELEASED 7u
#define CLOCK_B_RISE 9u
#define CLOCK_A_SETTLE 13u
#define CLOCK_B_RISE_NEXT 16u
#define CLOCK_B_LOW 17u
#define CLOCK_A_RISE 20u
#define CLOCK_A_ROSE 23u
#define CLOCK_B_SETTLE 25u
#define CLOCK_B_SETTLE_NEXT 29u

enum link_kind {
    LINK_FOLLOW,
    LINK_JOIN,
    LINK_CLOCK,
    LINK_KINDS,
};

struct gpio_link {
    bool used;
    enum link_kind kind;
    // follow: to, from. join: A, B.
    struct mclw_gpio *pins[2];
    PIO pio;
    uint8_t pio_index;
    uint8_t sm;
};

struct link_program {
    uint8_t users;
    uint8_t offset;
};

static struct gpio_link links[LINK_MAX];
static struct link_program programs[LINK_KINDS][NUM_PIOS];
static uint16_t follow_words[FOLLOW_LENGTH];
static uint16_t join_words[JOIN_LENGTH];
static uint16_t clock_words[CLOCK_LENGTH];

static const pio_program_t follow_program = {
    .instructions = follow_words,
    .length = FOLLOW_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static const pio_program_t join_program = {
    .instructions = join_words,
    .length = JOIN_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static const pio_program_t clock_program = {
    .instructions = clock_words,
    .length = CLOCK_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static uint16_t side(uint value) {
    return (uint16_t)pio_encode_sideset(1u, value);
}

static void encode_programs(void) {
    uint16_t *p = join_words;

    follow_words[0] = (uint16_t)pio_encode_mov(pio_pins, pio_pins);

    // B low with the link not driving it: forward it to A.
    p[0] = (uint16_t)pio_encode_mov(pio_osr, pio_pins) | side(0);
    p[1] = (uint16_t)pio_encode_out(pio_x, 1u) | side(0);
    p[2] = (uint16_t)pio_encode_jmp_not_x(JOIN_B_DRIVEN) | side(0);
    p[3] = (uint16_t)pio_encode_jmp_pin(JOIN_TEST_B) | side(0);
    // A low with the link not driving it: drive B until A is let go.
    p[4] = (uint16_t)pio_encode_set(pio_pindirs, 1u) | side(0);
    p[5] = (uint16_t)pio_encode_jmp_pin(JOIN_RELEASE_B) | side(0);
    p[6] = (uint16_t)pio_encode_jmp(JOIN_A_WAIT) | side(0);
    p[7] = (uint16_t)pio_encode_set(pio_pindirs, 0u) | side(0);
    p[8] = (uint16_t)pio_encode_set(pio_y, JOIN_SETTLE_COUNT) | side(0);
    // The wrap's top: the settle runs out into the test of B.
    p[9] = (uint16_t)(pio_encode_jmp_y_dec(JOIN_SETTLE_LOOP) | side(0) |
        pio_encode_delay(JOIN_SETTLE_DELAY));
    p[10] = (uint16_t)pio_encode_wait_pin(true, 0u) | side(1);
    p[11] = (uint16_t)pio_encode_jmp(JOIN_SETTLE) | side(0);
    p = clock_words;
    // Neither side driven by the link: B low is forwarded to A (B_LOW);
    // A low (the JMP pin), to B.
    p[0] = (uint16_t)pio_encode_mov(pio_osr, pio_pins) | side(0);
    p[1] = (uint16_t)pio_encode_out(pio_x, 1u) | side(0);
    p[2] = (uint16_t)pio_encode_jmp_not_x(CLOCK_B_LOW) | side(0);
    p[3] = (uint16_t)pio_encode_jmp_pin(CLOCK_TEST_B) | side(0);
    // A held low by its own parts: drive B until A is let go.
    p[4] = (uint16_t)pio_encode_set(pio_pindirs, 1u) | side(0);
    p[5] = (uint16_t)pio_encode_jmp_pin(CLOCK_A_RELEASED) | side(0);
    p[6] = (uint16_t)pio_encode_jmp(CLOCK_A_WAIT) | side(0);
    // A let go: let go of B and hold A low until B is seen up; B still
    // low after SETTLE is held by its own parts (B_LOW).
    p[7] = (uint16_t)pio_encode_set(pio_pindirs, 0u) | side(1);
    p[8] = (uint16_t)pio_encode_set(pio_y, JOIN_SETTLE_COUNT) | side(1);
    p[9] = (uint16_t)pio_encode_mov(pio_osr, pio_pins) | side(1);
    p[10] = (uint16_t)pio_encode_out(pio_x, 1u) | side(1);
    p[11] = (uint16_t)pio_encode_jmp_not_x(CLOCK_B_RISE_NEXT) | side(1);
    // B up: let go of A, and wait for it to rise before looking at it; A
    // still low after SETTLE is held by its own parts (A_LOW).
    p[12] = (uint16_t)pio_encode_set(pio_y, JOIN_SETTLE_COUNT) | side(0);
    p[13] = (uint16_t)pio_encode_jmp_pin(CLOCK_TEST_B) | side(0);
    p[14] = (uint16_t)(pio_encode_jmp_y_dec(CLOCK_A_SETTLE) | side(0) | pio_encode_delay(JOIN_SETTLE_DELAY));
    p[15] = (uint16_t)pio_encode_jmp(CLOCK_A_LOW) | side(0);
    p[16] = (uint16_t)(pio_encode_jmp_y_dec(CLOCK_B_RISE) | side(1) | pio_encode_delay(JOIN_SETTLE_DELAY));
    // B held low by its own parts: drive A until B is let go.
    p[17] = (uint16_t)pio_encode_wait_pin(true, 0u) | side(1);
    // B let go: let go of A and hold B low until A is seen up; A still
    // low after SETTLE is held by its own parts (A_WAIT, B still driven).
    p[18] = (uint16_t)pio_encode_set(pio_pindirs, 1u) | side(0);
    p[19] = (uint16_t)pio_encode_set(pio_y, JOIN_SETTLE_COUNT) | side(0);
    p[20] = (uint16_t)pio_encode_jmp_pin(CLOCK_A_ROSE) | side(0);
    p[21] = (uint16_t)(pio_encode_jmp_y_dec(CLOCK_A_RISE) | side(0) | pio_encode_delay(JOIN_SETTLE_DELAY));
    p[22] = (uint16_t)pio_encode_jmp(CLOCK_A_WAIT) | side(0);
    // A up: let go of B, and wait for it to rise before looking at it; B
    // still low after SETTLE is held by its own parts (B_LOW).
    p[23] = (uint16_t)pio_encode_set(pio_pindirs, 0u) | side(0);
    p[24] = (uint16_t)pio_encode_set(pio_y, JOIN_SETTLE_COUNT) | side(0);
    p[25] = (uint16_t)pio_encode_mov(pio_osr, pio_pins) | side(0);
    p[26] = (uint16_t)pio_encode_out(pio_x, 1u) | side(0);
    p[27] = (uint16_t)pio_encode_jmp_not_x(CLOCK_B_SETTLE_NEXT) | side(0);
    p[28] = (uint16_t)pio_encode_jmp(CLOCK_TEST_B) | side(0);
    p[29] = (uint16_t)(pio_encode_jmp_y_dec(CLOCK_B_SETTLE) | side(0) | pio_encode_delay(JOIN_SETTLE_DELAY));
    p[30] = (uint16_t)pio_encode_jmp(CLOCK_B_LOW) | side(0);
}

static const pio_program_t *program_of(enum link_kind kind) {
    return kind == LINK_FOLLOW ? &follow_program : kind == LINK_JOIN ? &join_program : &clock_program;
}

static int acquire(struct gpio_link *l, uint64_t pins) {
    const pio_program_t *program = program_of(l->kind);
    int err = -ENOSPC;

    encode_programs();
    for (size_t n = 0; n < MCLW_PIO_TRIES; n++) {
        PIO pio = mclw_pio_try(n);
        if (pio == NULL) {
            continue;
        }
        uint block = pio_get_index(pio);
        struct link_program *shared = &programs[l->kind][block];

        if ((shared->users == 0u && !pio_can_add_program(pio, program)) || !mclw_pio_window(pio, pins)) {
            continue;
        }
        int sm = pio_claim_unused_sm(pio, false);
        if (sm < 0) {
            continue;
        }
        if (shared->users == 0u) {
            shared->offset = (uint8_t)pio_add_program(pio, program);
        }
        shared->users++;
        l->pio = pio;
        l->pio_index = (uint8_t)block;
        l->sm = (uint8_t)sm;
        return 0;
    }
    return err;
}

static void start_follow(struct gpio_link *l) {
    uint to = l->pins[0]->pin;
    uint from = l->pins[1]->pin;
    uint offset = programs[LINK_FOLLOW][l->pio_index].offset;
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_out_pins(&c, to, 1u);
    sm_config_set_in_pins(&c, from);
    pio_sm_set_pins_with_mask64(l->pio, l->sm, gpio_get(from) ? MCLW_PIO_PIN(to) : 0u, MCLW_PIO_PIN(to));
    pio_sm_set_pindirs_with_mask64(l->pio, l->sm, MCLW_PIO_PIN(to), MCLW_PIO_PIN(to));
    pio_gpio_init(l->pio, to);
    pio_sm_init(l->pio, l->sm, offset, &c);
    pio_sm_set_enabled(l->pio, l->sm, true);
}

static void start_join(struct gpio_link *l) {
    uint a = l->pins[0]->pin;
    uint b = l->pins[1]->pin;
    uint64_t both = MCLW_PIO_PIN(a) | MCLW_PIO_PIN(b);
    uint offset = programs[l->kind][l->pio_index].offset;
    pio_sm_config c = pio_get_default_sm_config();

    if (l->kind == LINK_JOIN) {
        sm_config_set_wrap(&c, offset + JOIN_TEST_B, offset + JOIN_SETTLE_LOOP);
    } else {
        // Every path of the clock program ends in a jump.
        sm_config_set_wrap(&c, offset, offset + CLOCK_LENGTH - 1u);
    }
    sm_config_set_sideset(&c, 1u, false, true);
    sm_config_set_sideset_pins(&c, a);
    sm_config_set_set_pins(&c, b, 1u);
    sm_config_set_in_pins(&c, b);
    sm_config_set_jmp_pin(&c, a);
    sm_config_set_out_shift(&c, true, false, 32u);
    gpio_pull_up(a);
    gpio_pull_up(b);
    pio_sm_set_pins_with_mask64(l->pio, l->sm, 0u, both);
    pio_sm_set_pindirs_with_mask64(l->pio, l->sm, 0u, both);
    pio_gpio_init(l->pio, a);
    pio_gpio_init(l->pio, b);
    pio_sm_init(l->pio, l->sm, offset, &c);
    pio_sm_set_enabled(l->pio, l->sm, true);
}

static int link_pins(enum link_kind kind, struct mclw_gpio *first, struct mclw_gpio *second) {
    struct gpio_link *l = NULL;
    int err;

    if (first == NULL || second == NULL || first == second || first->pin == second->pin ||
        first->link != NULL || second->link != NULL || first->square || second->square ||
        first->held || second->held ||
        (kind == LINK_FOLLOW && !first->output)) {
        return -EINVAL;
    }
    for (size_t i = 0; i < LINK_MAX; i++) {
        if (!links[i].used) {
            l = &links[i];
            break;
        }
    }
    if (l == NULL) {
        return -ENOSPC;
    }
    l->kind = kind;
    l->pins[0] = first;
    l->pins[1] = second;
    err = acquire(l, MCLW_PIO_PIN(first->pin) | MCLW_PIO_PIN(second->pin));
    if (err != 0) {
        return err;
    }
    l->used = true;
    first->link = l;
    second->link = l;
    if (kind == LINK_FOLLOW) {
        start_follow(l);
    } else {
        start_join(l);
    }
    return 0;
}

int mclw_hal_gpio_follow(struct mclw_gpio *to, struct mclw_gpio *from) {
    return link_pins(LINK_FOLLOW, to, from);
}

int mclw_hal_gpio_join(struct mclw_gpio *a, struct mclw_gpio *b) {
    return link_pins(LINK_JOIN, a, b);
}

int mclw_hal_gpio_join_clock(struct mclw_gpio *a, struct mclw_gpio *b) {
    return link_pins(LINK_CLOCK, a, b);
}

void mclw_hal_gpio_unlink(struct mclw_gpio *g) {
    struct gpio_link *l = g == NULL ? NULL : g->link;
    struct link_program *shared;

    if (l == NULL) {
        return;
    }
    shared = &programs[l->kind][l->pio_index];
    pio_sm_set_enabled(l->pio, l->sm, false);
    pio_sm_unclaim(l->pio, l->sm);
    if (--shared->users == 0u) {
        pio_remove_program(l->pio, program_of(l->kind), shared->offset);
    }
    for (size_t i = 0; i < 2u; i++) {
        l->pins[i]->link = NULL;
        mclw_rp2_gpio_restore(l->pins[i]);
    }
    l->used = false;
}
