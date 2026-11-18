/*
 * LCG2 -- SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Michele Giugliano
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * units.c -- numbers with units: rates, durations, current and voltage units.
 *
 * See units.h for what is accepted.  Rates and durations are kept as exact
 * integers (a fraction p/q Hz, picoseconds), never as floating point, so that
 * "20kHz" is exactly 20000 and a comparison of two rates is exact.  This
 * matches StimGen 2, which also counts time in integer picoseconds.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "units.h"

/* ------------------------------------------------------------------ */
/* Current and voltage units (the same names as StimGen's table).      */
/* ------------------------------------------------------------------ */

static const struct { const char *name; Dim dim; double scale; } unit_table[] = {
    { "fA", DIM_CURRENT, 1e-15 }, { "pA", DIM_CURRENT, 1e-12 },
    { "nA", DIM_CURRENT, 1e-9  }, { "uA", DIM_CURRENT, 1e-6  },
    { "mA", DIM_CURRENT, 1e-3  }, { "A",  DIM_CURRENT, 1.0   },
    { "uV", DIM_VOLTAGE, 1e-6  }, { "mV", DIM_VOLTAGE, 1e-3  },
    { "V",  DIM_VOLTAGE, 1.0   },
    { NULL, DIM_NONE,    0.0   }
};

int units_lookup(const char *name, Dim *dim, double *scale)
{
    int i;
    for (i = 0; unit_table[i].name; i++)
        if (strcmp(name, unit_table[i].name) == 0) {
            *dim = unit_table[i].dim;
            *scale = unit_table[i].scale;
            return 0;
        }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Decimal numbers, exactly                                            */
/* ------------------------------------------------------------------ */

/*
 * Split a decimal literal such as "20", "2.5" or "1e4" into an
 * integer mantissa m and a power of ten e, so that value = m * 10^e, with
 * all trailing zeros of m moved into e ("20000" -> m = 2, e = 4).  Returns 1
 * if the text is a well-formed number, 0 otherwise.  No floating point.
 */
static int split_decimal(const char *s, uint64_t *m, int *e)
{
    uint64_t v = 0;
    int exp10 = 0, seen_dot = 0, any = 0;

    /* No sign: a rate or a duration is positive, and StimGen refuses "+5". */
    for (; *s && *s != 'e' && *s != 'E'; s++) {
        if (*s == '.') {
            if (seen_dot) return 0;
            seen_dot = 1;
            continue;
        }
        if (*s < '0' || *s > '9') return 0;
        any = 1;
        if (v > (UINT64_MAX - 9) / 10) return 0;      /* too many digits */
        v = v * 10 + (uint64_t)(*s - '0');
        if (seen_dot) exp10--;
    }
    if (!any) return 0;

    if (*s == 'e' || *s == 'E') {                      /* exponent part */
        int x = 0, neg = 0;
        s++;
        if (*s == '+') s++;
        else if (*s == '-') { neg = 1; s++; }
        if (!*s) return 0;
        for (; *s; s++) {
            if (*s < '0' || *s > '9') return 0;
            x = x * 10 + (*s - '0');
            if (x > 400) return 0;                     /* absurd exponent */
        }
        exp10 += neg ? -x : x;
    }

    while (v != 0 && v % 10 == 0) { v /= 10; exp10++; }
    *m = v;
    *e = exp10;
    return 1;
}

/* Greatest common divisor, for reducing the fraction p/q. */
static int64_t gcd64(int64_t a, int64_t b)
{
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

/* ------------------------------------------------------------------ */
/* Sampling rate                                                       */
/* ------------------------------------------------------------------ */

int units_parse_rate(const char *text, int64_t *p, int64_t *q)
{
    char num[64];
    const char *u;
    uint64_t m;
    int e, ue;
    int64_t num_p, den_q, g;
    size_t i = strspn(text, "0123456789.eE+-");

    if (i == 0 || i >= sizeof num) return -1;
    memcpy(num, text, i);
    num[i] = 0;
    u = text + i;                                      /* the unit suffix */
    if (!*u || !strcmp(u, "Hz")) ue = 0;
    else if (!strcmp(u, "kHz")) ue = 3;
    else if (!strcmp(u, "MHz")) ue = 6;
    else return -1;
    if (!split_decimal(num, &m, &e) || m == 0) return -1;

    /* value = m * 10^(e + ue) Hz; build the fraction num_p/den_q. */
    den_q = 1;
    e += ue;
    for (; e > 0; e--) {
        if (m > (uint64_t)INT64_MAX / 10) return -1;
        m *= 10;
    }
    for (; e < 0; e++) {
        if (den_q >= 1000000) return -1;               /* > 6 decimals */
        den_q *= 10;
    }
    if (m > (uint64_t)INT64_MAX) return -1;
    num_p = (int64_t)m;
    g = gcd64(num_p, den_q);
    *p = num_p / g;
    *q = den_q / g;
    return 0;
}

void units_format_rate(char *buf, size_t n, int64_t p, int64_t q)
{
    if (q == 1 && p % 1000000 == 0)
        snprintf(buf, n, "%lld MHz", (long long)(p / 1000000));
    else if (q == 1 && p % 1000 == 0)
        snprintf(buf, n, "%lld kHz", (long long)(p / 1000));
    else if (q == 1)
        snprintf(buf, n, "%lld Hz", (long long)p);
    else
        snprintf(buf, n, "%.6g Hz", (double)p / (double)q);
}

/* ------------------------------------------------------------------ */
/* Duration                                                            */
/* ------------------------------------------------------------------ */

/* Picoseconds in one of the time units, 0 if the name is not a time unit.
 * An empty unit is not one: unlike StimGen's internal helper, LCG2 insists on
 * the unit, as StimGen's own documentation recommends, because "-t 30" could
 * be read as seconds or minutes. */
static int64_t unit_ps(const char *u)
{
    if (!strcmp(u, "s"))        return 1000000000000LL;
    if (!strcmp(u, "ms"))       return 1000000000LL;
    if (!strcmp(u, "us"))       return 1000000LL;
    if (!strcmp(u, "ns"))       return 1000LL;
    if (!strcmp(u, "min"))      return 60000000000000LL;
    return 0;
}

int units_parse_duration(const char *text, int64_t *ps)
{
    char num[64];
    uint64_t m;
    int e;
    int64_t unit, x;
    size_t i = strspn(text, "0123456789.eE+-");

    if (i == 0 || i >= sizeof num) return -1;
    memcpy(num, text, i);
    num[i] = 0;
    unit = unit_ps(text + i);
    if (unit == 0) return -1;
    if (!split_decimal(num, &m, &e) || m == 0) return -1;

    /* value_ps = m * unit * 10^e; multiply first, then scale by ten, and
     * for a negative e insist that every division by ten is exact. */
    if (m > (uint64_t)INT64_MAX / (uint64_t)unit) return -1;
    x = (int64_t)m * unit;
    for (; e > 0; e--) {
        if (x > INT64_MAX / 10) return -1;
        x *= 10;
    }
    for (; e < 0; e++) {
        if (x % 10) return -1;                         /* not whole ps */
        x /= 10;
    }
    if (x <= 0) return -1;
    *ps = x;
    return 0;
}
