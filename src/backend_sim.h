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
 * backend_sim.h -- the built-in loopback simulator (device = sim).
 *
 * Every output is wired to the input of the same number: analog output aoN to
 * analog input aiN, with a little noise on top.  An input with no output of its
 * number hears only noise.  It tests everything that is not the cell and not the
 * board: scheduling, rest values, recording, the archive.
 *
 * It behaves like a board where that matters to a test of LCG2:
 *   - an input has a range (0.2, 1, 5 or 10 V): a voltage beyond it is clamped
 *     and counted, never passed on as if it were fine;
 *   - the noise is deterministic, so the same command gives the same "recording";
 *   - starting a trial can take time (latency_ns), which shows up as a delay;
 *   - it remembers what every output last did, and in what order the operations
 *     came, so that a test can check that every exit path ended at rest.
 */
#ifndef LCG2_BACKEND_SIM_H
#define LCG2_BACKEND_SIM_H

#include "backend.h"

typedef struct {
    int64_t latency_ns;       /* time from play() until the outputs start     */
    double  noise_sd;         /* volts, Gaussian, on every input              */
    uint64_t seed;            /* the noise of trial t is derived from this    */
} SimOpts;

/* What the simulator saw, for the tests. */
typedef struct {
    int           nplay, nrest, ntrigger;
    char          events[512];            /* O open, R rest, T trigger, P play, p play stopped early, C close */
    double        ao_state[4];            /* volts last driven on ao0..ao3    */
    int           ao_known[4];
    unsigned char dig_state[8];           /* last 0/1 on port0/line0..7       */
    int           dig_known[8];
    int           open, closed;
} SimStats;

/* A new simulator (free it with sim_backend_free).  opts may be NULL: no
 * latency, 2 mV of noise, seed 1. */
Backend *sim_backend_new(const SimOpts *opts);
void     sim_backend_free(Backend *be);
const SimStats *sim_backend_stats(const Backend *be);

#endif /* LCG2_BACKEND_SIM_H */
