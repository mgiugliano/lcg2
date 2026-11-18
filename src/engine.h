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
 * engine.h -- the trial engine (milestone M3).
 *
 * The engine plays a list of trials through a backend, one after the other, on
 * a clock.  It decides WHEN each trial starts (the timing rules of a StimGen
 * protocol), keeps the outputs at their rest values in between, hands the
 * recorded inputs to a Sink in physical units, and makes sure that however the
 * run ends -- normally, by an error, or by Ctrl-C -- the outputs end at rest.
 *
 * THE TIMING RULES (StimGen's own, protocol layer, "Timing between trials"):
 *   period T   each trial starts T after the START of the previous one.  If a
 *              trial is longer than T, the next starts as soon as it ends; the
 *              delay is recorded.
 *   gap T      each trial starts T after the END of the previous one.
 *   start on trigger   each trial waits for a trigger, after the period or the
 *              gap has elapsed.
 * The default, when a protocol says nothing, is "gap 0s" and "start immediately".
 *
 * Times are nanoseconds on the engine's Clock, counted from the start of the
 * run.  The "delay" of a trial is when it really started minus when it was
 * meant to start; it includes the start latency of the backend.  On the virtual
 * clock all times are simulated, and the progress line says so.
 *
 * THE ORDER OF THINGS (what makes it safe):
 *   1  the first trial is prepared (rendered, checked) before any output is touched;
 *   2  the backend is opened and every output is set to its rest value;
 *   3  for every trial: prepare it, wait for its time (and its trigger), play it,
 *      set the outputs at rest again;
 *   4  whatever happens, the last thing before closing is: outputs to rest.
 * "Prepare" re-renders the trial, compares its digest with the one remembered by
 * the check pass, checks every sample again, and builds its drive.  A buffer that
 * is not exactly the one that was checked is never played.
 */
#ifndef LCG2_ENGINE_H
#define LCG2_ENGINE_H

#include <stdint.h>
#include "backend.h"
#include "cli.h"
#include "config.h"
#include "platform.h"
#include "stim.h"

/* The timing between trials, from the "timing" and "start" strings StimGen gives. */
typedef struct {
    int     is_period;           /* 1: period (start to start), 0: gap (end to start) */
    int64_t ns;                  /* the time, in nanoseconds                          */
    int     on_trigger;          /* each trial waits for a trigger                    */
} Timing;

/* Read "period 5s", "gap 0s", "gap 500ms" and "immediately" / "on trigger".  Empty
 * strings mean the defaults.  Zero is allowed; a bare number is seconds, as in
 * StimGen.  Returns 0 or -1. */
int engine_parse_timing(const char *timing, const char *start, Timing *t, Err *err);

/* What the sink is told about a trial before it starts. */
typedef struct {
    int         index, count;    /* trial index (0-based) and number of trials          */
    const char *name;            /* "0003", "stimulus", "record"                        */
    const Trial *trial;          /* the rendered stimulus; NULL for a record-only trial */
    const char *variables;       /* "amp=-300pA", or ""                                 */
    int64_t     n;               /* samples per input in this trial                     */
    int64_t     rate_p, rate_q;
    int64_t     scheduled_ns;    /* when it was meant to start                          */
    int         nin;             /* active inputs, in the order of the blocks           */
    struct { int in; const char *name; const char *unit; } ai[CFG_MAX_IN];
    int         is_virtual;      /* the times are simulated                             */
} TrialInfo;

/* What the sink is told when a trial is over (or was stopped). */
typedef struct {
    int64_t start_ns, end_ns, delay_ns;   /* delay = start - scheduled (>= 0)           */
    char    start_utc[32];                /* wall clock at the start, UTC               */
    int64_t latency_ns;                   /* the backend's start latency (part of delay)*/
    int64_t recorded;                     /* samples per input actually delivered       */
    int     incomplete;                   /* stopped before the end                     */
    int64_t overrange[CFG_MAX_IN];        /* per input: samples clamped at its range    */
} TrialRecord;

/* Where the recorded inputs go.  Blocks carry physical units (pA, mV, ...), one
 * array per active input (TrialInfo.nin of them), and the list ends with a NULL.
 * M4 provides the sink that writes the HDF5 file. */
typedef struct {
    int (*begin)(void *user, const TrialInfo *ti, Err *err);
    int (*block)(void *user, int64_t first, int64_t count, const double *const x[], Err *err);
    int (*end)(void *user, const TrialInfo *ti, const TrialRecord *rec, Err *err);
    void *user;
} Sink;

/* How a run ended. */
enum { ENGINE_OK = 0, ENGINE_ABORTED = 2, ENGINE_FAILED = 3 };

typedef struct {
    int     touched;             /* the backend was opened: an exit status of 2, not 1  */
    int     started, completed;  /* trials started and completed                        */
    int64_t max_delay_ns;
    int64_t elapsed_ns;
    int64_t overrange_total;
    int     is_virtual;
} EngineReport;

/* Everything the engine needs. */
typedef struct {
    const Config  *cfg;
    Backend       *backend;
    Clock         *clock;
    Sink          *sink;
    Timing         timing;
    int            quiet;        /* no progress lines */
    /* What to play: a stimulus (st, opt) or, if st is NULL, idle_n samples of rest. */
    const Stim    *st;
    const Options *opt;
    int64_t        idle_n, idle_rate_p, idle_rate_q;
} EngineJob;

/* Run the job.  Returns ENGINE_OK, ENGINE_ABORTED (Ctrl-C or kill) or
 * ENGINE_FAILED (message in err).  In every case the outputs have been put at rest
 * if the backend had been touched (report->touched). */
int engine_run(const EngineJob *job, EngineReport *rep, Err *err);

/* Block size of the streaming: about this many seconds of samples, at least 1. */
#define ENGINE_BLOCK_SECONDS 0.1

#endif /* LCG2_ENGINE_H */
