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
 * run.h -- run the trials of a checked Session and keep the record.
 *
 * run_execute() is what lcg2 does after all the checks: it makes the run folder
 * and the data file, plays the trials on the loopback simulator through the engine,
 * writes every input into the data file as it arrives, and finishes the record
 * however the run ends.  It lives here, and not in main.c, so that the test drivers
 * run exactly the same code with their own simulator settings (no noise, a start
 * latency, a failing rest) instead of the program growing hidden switches.
 *
 * THE ORDER, and why it matters for the exit status:
 *   1  everything that can fail without touching an output: the run folder, the
 *      files in it, the data file.  A failure here ends with status 1;
 *   2  the engine plays the trials (it opens the backend: from here a failure is 2);
 *   3  whatever happened, the data file is closed, its digest taken, the digests
 *      written.  A failure to write the integrity record is a failure of the run
 *      (status 2, said loudly): the record is part of the result.
 */
#ifndef LCG2_RUN_H
#define LCG2_RUN_H

#include "backend_sim.h"
#include "lcg2.h"
#include "session.h"

typedef struct {
    const Session *session;
    SimOpts        sim;              /* the simulator's behaviour                         */
} RunParams;

/* The settings of the real program: 2 mV of noise, a fixed noise seed, no latency,
 * the state of the outputs reported with -v. */
void run_params_default(RunParams *rp, const Session *s);

/* Returns the exit status: 0 success, 1 an error before any output was touched, 2
 * aborted, or failed after, or the record could not be finished.  A message for
 * status 1 or 2 is left in err when there is one to say. */
int run_execute(const RunParams *rp, Err *err);

#endif /* LCG2_RUN_H */
