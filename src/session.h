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
 * session.h -- reading and checking everything, before anything is played.
 *
 * A Session is what a run of LCG2 knows once it has read its command line, its
 * configuration (with the profile applied) and its stimulus, and checked all of
 * it: the options, the typed configuration, the opened stimulus, and a report of
 * what the check found.  The real program and the test drivers both start here, so
 * they check exactly the same things and differ only in what they do next.
 */
#ifndef LCG2_SESSION_H
#define LCG2_SESSION_H

#include <stdint.h>
#include "cli.h"
#include "config.h"
#include "lcg2.h"
#include "stim.h"

/* What reading and checking the stimulus found (for the plan). */
typedef struct {
    int     ntrials;
    int64_t total_samples;        /* over all trials, per channel            */
    int64_t rate_p, rate_q;       /* the rate of the run                     */
    Trial   first;                /* the first trial (kept to describe it)   */
    Map     map[CFG_MAX_OUT];     /* its channel map                         */
    double  peak[CFG_MAX_OUT];    /* largest |volts| asked, per channel      */
    int     nwarn;
} Report;

typedef struct {
    Options opt;
    Table   table;                /* the configuration, resolved             */
    Config  config;
    Stim    stim;                 /* opened and checked (not for -t)         */
    Report  report;
    int     argc;                 /* the command line, kept for the archive  */
    char  **argv;
} Session;

/* Read the command line and everything it names, and check it all.  Returns 0 on
 * success, 1 if the help was printed (nothing more to do), -1 with a message.
 * The Session is large (the table): keep it static. */
int session_prepare(Session *s, int argc, char **argv, Err *err);

/* The answer to -n: what would happen, in words, on stdout. */
void session_print_plan(const Session *s);

#endif /* LCG2_SESSION_H */
