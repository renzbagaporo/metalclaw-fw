// SPDX-License-Identifier: Apache-2.0 OR MIT
// Formatting and matching without printf, for the AT engine and the fake
// network alike.

#ifndef MCLW_DEVICES_CELLULAR_TEXT_H
#define MCLW_DEVICES_CELLULAR_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEXT_MAX 128

// A line being formatted on the stack. Appends past the end are dropped.
struct text {
    size_t len;
    char buf[TEXT_MAX];
};

void text_init(struct text *t);
void text_char(struct text *t, char c);
void text_mem(struct text *t, const char *p, size_t n);
void text_str(struct text *t, const char *str);
void text_u32(struct text *t, uint32_t v);
void text_i32(struct text *t, int32_t v);
// `v` in units of 10^-decimals, printed with exactly that many decimals.
void text_fixed(struct text *t, int32_t v, uint8_t decimals);
void text_d2(struct text *t, uint32_t v);
void text_ip(struct text *t, const uint8_t ip[4]);
// "yy/MM/dd,hh:mm:ss" of a {year, month, day, hour, minute, second} array.
void text_date_time(struct text *t, const int32_t time[6]);

char text_upper(char c);
bool text_equal_nocase(const char *a, const char *b);
size_t text_strnlen(const char *s, size_t max);

#endif
