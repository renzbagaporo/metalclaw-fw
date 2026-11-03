// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "text.h"

char text_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool text_equal_nocase(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (text_upper(*a) != text_upper(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

size_t text_strnlen(const char *s, size_t max) {
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

void text_init(struct text *t) {
    t->len = 0;
}

void text_char(struct text *t, char c) {
    if (t->len < TEXT_MAX) {
        t->buf[t->len++] = c;
    }
}

void text_mem(struct text *t, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        text_char(t, p[i]);
    }
}

void text_str(struct text *t, const char *str) {
    text_mem(t, str, strlen(str));
}

void text_u32(struct text *t, uint32_t v) {
    char digits[10];
    size_t n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        text_char(t, digits[--n]);
    }
}

static uint32_t magnitude(int32_t v) {
    return v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
}

void text_i32(struct text *t, int32_t v) {
    if (v < 0) {
        text_char(t, '-');
    }
    text_u32(t, magnitude(v));
}

void text_fixed(struct text *t, int32_t v, uint8_t decimals) {
    uint32_t scale = 1;
    uint32_t m = magnitude(v);

    for (uint8_t i = 0; i < decimals; i++) {
        scale *= 10u;
    }
    if (v < 0) {
        text_char(t, '-');
    }
    text_u32(t, m / scale);
    if (decimals == 0) {
        return;
    }
    text_char(t, '.');
    uint32_t frac = m % scale;
    for (uint32_t div = scale / 10u; div != 0; div /= 10u) {
        text_char(t, (char)('0' + (frac / div) % 10u));
    }
}

void text_d2(struct text *t, uint32_t v) {
    text_char(t, (char)('0' + (v / 10u) % 10u));
    text_char(t, (char)('0' + v % 10u));
}

void text_ip(struct text *t, const uint8_t ip[4]) {
    for (size_t i = 0; i < 4; i++) {
        if (i != 0) {
            text_char(t, '.');
        }
        text_u32(t, ip[i]);
    }
}

void text_date_time(struct text *t, const int32_t time[6]) {
    text_d2(t, (uint32_t)time[0] % 100u);
    text_char(t, '/');
    text_d2(t, (uint32_t)time[1]);
    text_char(t, '/');
    text_d2(t, (uint32_t)time[2]);
    text_char(t, ',');
    text_d2(t, (uint32_t)time[3]);
    text_char(t, ':');
    text_d2(t, (uint32_t)time[4]);
    text_char(t, ':');
    text_d2(t, (uint32_t)time[5]);
}
