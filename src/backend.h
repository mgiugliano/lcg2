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
 * backend.h -- what stands between LCG2 and the world that produces the signals.
 *
 * A backend plays the outputs of a trial and returns what the inputs measured.
 * It may be the built-in loopback simulator (backend_sim.c), the NEURON
 * simulator (milestone M3b) or the board (milestone M6).  Everything above it
 * (the engine, the recording, the archive) is the same for all of them.
 *
 * THIS INTERFACE IS PROVISIONAL.  It is small on purpose: five operations.  The
 * hardware spike (S1) answers questions that decide how a real board does them
 * (one task or two for the outputs, how the clocks and the start trigger are
 * shared, what the start latency is), and it may change the details.  Keep the
 * callers thin.
 *
 * INPUTS ARE STREAMED.  play() hands the inputs over in blocks as they are
 * measured, not all at once at the end.  This is what lets a recording grow on
 * disk as it is made, keeps the completed part of a trial that is aborted half
 * way (SPEC 3.15), and keeps a long record-only run out of memory.  The blocks
 * carry volts; the engine turns them into physical units.
 *
 * "Active inputs" are the [in] sections that are not skipped, in the order they
 * have in the configuration.  volts[k] of a block belongs to the k-th of them.
 */
#ifndef LCG2_BACKEND_H
#define LCG2_BACKEND_H

#include <stdint.h>
#include "config.h"
#include "lcg2.h"
#include "platform.h"
#include "stim.h"

typedef struct Backend Backend;

/* What play() reports about one trial. */
typedef struct {
    int64_t latency_ns;                  /* from the call until the first output sample    */
    int64_t recorded;                    /* input samples delivered                        */
    int     incomplete;                  /* stopped before the end (an abort)              */
    int64_t overrange[CFG_MAX_IN];       /* per active input: samples beyond its range     */
} PlayResult;

/* Called by play() for every block of inputs: `first` is the index of the first
 * sample of the block in the trial, `count` how many it has.  Return 0, or -1
 * with a message to stop the trial. */
typedef int (*BlockFn)(void *user, int64_t first, int64_t count,
                       const double *const volts[], Err *err);

struct Backend {
    const char *name;

    /* Get ready to drive the board (or simulate it). */
    int  (*open)(Backend *be, const Config *cfg, Err *err);

    /* Hold every output at the rest value that `d` ends on (d->ao_rest, d->dig_rest). */
    int  (*set_rest)(Backend *be, const Config *cfg, const Drive *d, Err *err);

    /* "start on trigger": wait for the trigger.  Returns 0 when it came, 1 if
     * the wait was ended by an abort (or the end of input), -1 on an error. */
    int  (*wait_trigger)(Backend *be, const Config *cfg, Clock *clk, Err *err);

    /* Play the outputs of d and record the inputs, both on the same clock, and
     * return when the trial is over.  Inputs go to `block` as they arrive, in
     * blocks of about block_samples.  Returns 0 when the trial ran to its end, 1
     * if an abort request stopped it early (res->incomplete is then set), -1 on
     * an error.  An abort request is seen between blocks. */
    int  (*play)(Backend *be, Clock *clk, const Config *cfg, const Drive *d,
                 int64_t rate_p, int64_t rate_q, int64_t block_samples,
                 BlockFn block, void *user, PlayResult *res, Err *err);

    void (*close)(Backend *be);

    void *impl;                          /* the backend's own state */
};

/* The [in] sections in use, in configuration order; returns how many. */
static inline int backend_active_inputs(const Config *cfg, int idx[CFG_MAX_IN])
{
    int k, n = 0;
    for (k = 0; k < cfg->n_in; k++)
        if (!cfg->in[k].skip) idx[n++] = k;
    return n;
}

#endif /* LCG2_BACKEND_H */
