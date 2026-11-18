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
 * lcg2.h -- declarations shared by all the modules of LCG2.
 *
 * LCG2 plays a StimGen 2 stimulus on the outputs of a data-acquisition board
 * and records the inputs at the same time.  This build (milestones M0 and M1)
 * reads and checks the command line and the configuration file; nothing is
 * played yet.
 *
 * Module map (one file each):
 *
 *   units.c    numbers with units: sampling rates, durations, current/voltage
 *   config.c   the INI file: reading, rig profiles, validation
 *   cli.c      the command line
 *   main.c     glue: read, check, print the plan
 *
 * Conventions used everywhere:
 *   - Functions that can fail return 0 on success and -1 on failure, and put a
 *     human-readable message in an Err.  Nothing below main() prints errors or
 *     calls exit(): main() decides.  (This matters later: when the outputs of
 *     the board are live, an error must return to a place that can put them
 *     back to rest, never exit from deep inside.)
 *   - Exit status of the program: 0 success, 1 error before the run starts,
 *     2 aborted or failed during the run.
 *   - Text is plain ASCII; sizes are fixed (no malloc) where a fixed size is
 *     enough, to keep the code short and easy to audit.
 */
#ifndef LCG2_H
#define LCG2_H

#include <stdint.h>
#include <stdio.h>

#define LCG2_VERSION "0.2.1"

/* ------------------------------------------------------------------ */
/* Error messages                                                      */
/* ------------------------------------------------------------------ */

typedef struct { char msg[512]; } Err;

/* Format a message into e (printf style) and return -1, so that a failing
 * function can simply say "return err_set(e, ...);". */
int err_set(Err *e, const char *fmt, ...);

#endif /* LCG2_H */
