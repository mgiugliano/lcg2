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
 * mtest.c -- the membrane test (see mtest.h for what is computed and why).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mtest.h"

static double mean(const double *x, int64_t a, int64_t b)
{
    double s = 0;
    int64_t i;
    for (i = a; i < b; i++) s += x[i];
    return s / (double)(b - a);
}

int mtest_analyse(const double *cmd, const double *cur, int64_t n, double rate_hz, MTest *m)
{
    int64_t s, e, len, i, first, last, nb;
    double ib, iend, di, d0, sign, sx = 0, sy = 0, sxx = 0, sxy = 0, slope, icpt, i0;
    int np;

    memset(m, 0, sizeof *m);
    /* the step: the first change of the command, and how long it is held */
    for (s = 1; s < n && cmd[s] == cmd[s - 1]; s++) {}
    if (s >= n) {
        snprintf(m->why, sizeof m->why, "no step in the command");
        return -1;
    }
    for (e = s + 1; e < n && cmd[e] == cmd[s]; e++) {}
    len = e - s;
    if (s < 10 || len < 20) {
        snprintf(m->why, sizeof m->why, "the step must come after 10 samples at least and last 20 (here %lld and %lld)",
                 (long long)s, (long long)len);
        return -1;
    }
    m->dv_mv = cmd[s] - cmd[s - 1];
    nb = len / 4 < s ? len / 4 : s;
    ib = mean(cur, s - nb, s);                                     /* before the step  */
    iend = mean(cur, e - len / 4, e);                              /* its last quarter */
    m->ihold_pa = ib;
    di = iend - ib;
    m->r_mohm = di != 0 ? m->dv_mv / di * 1000.0 : INFINITY;       /* mV / pA is a GOhm */

    /* the transient: from the second sample after the step, while it stays above 5 % of its start */
    first = s + 1;
    d0 = cur[first] - iend;
    sign = d0 > 0 ? 1.0 : -1.0;
    if (d0 == 0 || sign * m->dv_mv < 0) {
        snprintf(m->why, sizeof m->why, "no capacitive transient: no cell behind the pipette (bath, seal or cell-attached)");
        return 0;
    }
    for (last = first; last < s + len / 2 && sign * (cur[last] - iend) > 0.05 * fabs(d0); last++) {}
    np = (int)(last - first);
    if (last >= s + len / 2) {                                     /* still changing half-way through the step */
        snprintf(m->why, sizeof m->why, "the current is still changing half-way through the step: not the transient "
                 "of a cell (the seal is forming, or the step is too short)");
        return 0;
    }
    if (np < 3) {
        snprintf(m->why, sizeof m->why, "no capacitive transient: no cell behind the pipette (bath, seal or cell-attached)");
        return 0;
    }
    for (i = first; i < last; i++) {
        double t = (double)(i - s + 1) / rate_hz * 1000.0;         /* ms from the step */
        double y = log(sign * (cur[i] - iend));
        sx += t; sy += y; sxx += t * t; sxy += t * y;
    }
    slope = (np * sxy - sx * sy) / (np * sxx - sx * sx);
    icpt = (sy - slope * sx) / np;
    if (!(slope < 0)) {
        snprintf(m->why, sizeof m->why, "the current after the step does not decay");
        return 0;
    }
    m->tau_ms = -1.0 / slope;
    if (m->tau_ms * rate_hz / 1000.0 < MTEST_MIN_SAMPLES) {
        snprintf(m->why, sizeof m->why, "the transient lasts %.1f samples per time constant (tau %.0f us at %.0f kHz); "
                 "at least %.0f are needed: sample faster", m->tau_ms * rate_hz / 1000.0, m->tau_ms * 1000.0,
                 rate_hz / 1000.0, MTEST_MIN_SAMPLES);
        return 0;
    }
    i0 = sign * exp(icpt);
    /* at t = 0 the capacitance is a short: the whole step falls on Ra, and the current is the transient plus
     * the steady change, dV / Ra = I0 + dI */
    m->ra_mohm = m->dv_mv / (i0 + di) * 1000.0;
    m->rm_mohm = m->r_mohm - m->ra_mohm;
    if (!(m->ra_mohm > 0) || !(m->rm_mohm > m->ra_mohm) || !isfinite(m->rm_mohm)) {   /* in whole-cell Ra << Rm */
        snprintf(m->why, sizeof m->why, "the transient does not fit a cell clamped through an access resistance");
        return 0;
    }
    m->cm_pf = m->tau_ms * (m->ra_mohm + m->rm_mohm) / (m->ra_mohm * m->rm_mohm) * 1000.0;   /* ms / MOhm is a nF */
    if (m->cm_pf < 1.0) {
        snprintf(m->why, sizeof m->why, "the transient is too small for a cell (%.2f pF)", m->cm_pf);
        return 0;
    }
    m->cell = 1;
    return 0;
}

void mtest_format_r(double r_mohm, char *buf, int size)
{
    if (!isfinite(r_mohm) || r_mohm > 1e6) snprintf(buf, (size_t)size, "> 1000 GOhm");
    else if (fabs(r_mohm) >= 1000.0) snprintf(buf, (size_t)size, "%.2f GOhm", r_mohm / 1000.0);
    else snprintf(buf, (size_t)size, "%.2f MOhm", r_mohm);
}
