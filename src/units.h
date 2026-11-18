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
 * units.h -- numbers with units: rates, durations, current and voltage units.
 *
 * The spellings accepted here are the ones StimGen 2 accepts (see StimGen's
 * parse.c, parse_rate() and unit_ps_of(), and its documentation chapter on
 * conventions), because the flags -r and -t "mean what they mean in sg".
 * LCG2 may only call StimGen through sg_api_*, so these few lines are
 * written again here; the milestone M2 test compares them with StimGen.
 */
#ifndef LCG2_UNITS_H
#define LCG2_UNITS_H

#include <stdint.h>
#include "lcg2.h"

/* The two physical dimensions an amplifier channel can carry. */
typedef enum { DIM_NONE = 0, DIM_CURRENT, DIM_VOLTAGE } Dim;

/* Look a unit name up ("pA", "mV", ...).  On success return 0, set *dim, and
 * *scale to the number of base units (A or V) in one of that unit, so that
 * "nA" -> 1e-9.  Return -1 for a unit that is neither a current nor a
 * voltage.  Names are case sensitive, as in StimGen ("mV" is not "MV"). */
int units_lookup(const char *name, Dim *dim, double *scale);

/* Sampling rate: a number optionally followed by Hz, kHz or MHz ("20kHz",
 * "10000", "2.5Hz", "1e4").  Without a unit the number is in Hz.  No sign.
 * Malformed exponents such as "1e" or "1ee3" are refused (StimGen's own
 * parser lets some of them through; LCG2 is stricter on purpose).  The result
 * is the exact fraction p/q Hz in lowest terms, q > 0.  A zero, negative or
 * inexact (more than 6 decimals) rate is refused.  Return 0 or -1. */
int units_parse_rate(const char *text, int64_t *p, int64_t *q);

/* Duration: a number followed by ns, us, ms, s or min ("30s", "2min",
 * "250us").  The unit is required.  The
 * result is in picoseconds (exact, like StimGen), and must be > 0.  A value
 * that is not a whole number of picoseconds is refused.  Return 0 or -1. */
int units_parse_duration(const char *text, int64_t *ps);

/* Format p/q Hz for people: "20 kHz", "10 Hz", "2.5 Hz".  Result is in buf. */
void units_format_rate(char *buf, size_t n, int64_t p, int64_t q);

#endif /* LCG2_UNITS_H */
