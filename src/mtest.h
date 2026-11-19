/*
 * LCG2 -- SPDX-License-Identifier: MIT
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
 * mtest.h -- the membrane test: what an amplifier's seal-test window shows,
 * computed from a recorded voltage-clamp test pulse.
 *
 * The trial must hold a potential, step by dV, and hold the step for a while:
 *
 *     command  ____|‾‾‾‾‾‾‾‾‾‾‾‾‾‾|____        current  ____|\_____________|/___
 *                  s              e                          ^ transient
 *
 *   R      = dV / (I at the end of the step - I before it)          the total resistance:
 *            the pipette in the bath, the seal, or Ra + Rm in whole-cell
 *   I hold = the current before the step
 *
 * If the current after the step decays as an exponential (a cell's capacitance charging through the
 * access resistance), it is fitted, from the second sample after the step (the first holds the
 * pipette's own transient) while it stays above 5 % of where it started:
 *
 *   I(t) - I_end = I0 exp(-t / tau)    (t from the step)
 *   Ra = dV / (I0 + dI),  Rm = R - Ra,  Cm = tau (Ra + Rm) / (Ra Rm)
 *
 * (at the step the capacitance is a short, so the whole step falls on Ra: I0 + dI = dV / Ra, where dI is the
 * steady change of the current)
 *
 * which is the usual estimate for a single passive compartment clamped through Ra (a cell with active
 * channels, or a leaky seal, biases it, as on a real amplifier).  It is refused (and why is said) when the
 * transient spans fewer than MTEST_MIN_SAMPLES samples per time constant (then sample a faster rate), when it
 * has not decayed half-way through the step (a seal that is still forming), or when it gives Ra >= Rm (not a
 * whole-cell recording: a patch in cell-attached charges the cell through gigaohms).  Sample i is the value at the end of sample interval i (SPEC 4e), so t = (i - s + 1)/rate.
 *
 * Needs nothing but the C library: unit-tested on synthetic exponentials (tests/unit.c).
 */
#ifndef LCG2_MTEST_H
#define LCG2_MTEST_H

#include <stdint.h>

#define MTEST_MIN_SAMPLES 5.0       /* samples per time constant needed to fit the transient */

typedef struct {
    double dv_mv;                   /* the step                                         */
    double r_mohm;                  /* total resistance (INFINITY if the current did not change) */
    double ihold_pa;                /* the current before the step                      */
    int    cell;                    /* 1: a whole-cell transient was fitted             */
    double ra_mohm, rm_mohm, cm_pf, tau_ms;
    char   why[200];                /* why there is no fit (when cell is 0)             */
} MTest;

/* cmd in mV, cur in pA, n samples at rate_hz.  Returns 0 and fills m, or -1 with m->why set when the
 * trial has no usable step (then nothing else in m is valid). */
int mtest_analyse(const double *cmd, const double *cur, int64_t n, double rate_hz, MTest *m);

/* "5.02 MOhm", "1.84 GOhm", "> 1000 GOhm": a resistance for a person, in buf. */
void mtest_format_r(double r_mohm, char *buf, int size);

#endif
