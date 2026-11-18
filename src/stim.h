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
 * stim.h -- from StimGen text to checked drive signals (milestone M2).
 *
 * The path a stimulus takes through LCG2:
 *
 *   text (file or -e)
 *     |  stim_open()      what is it (waveform, stimulus, protocol)?  which
 *     v                   rate and seed?  for a protocol, the list of trials
 *   Stim
 *     |  stim_render()    one trial, as StimGen renders it, decoded into
 *     v                   arrays of numbers in the stimulus's own units
 *   Trial
 *     |  stim_map()       which [out] carries each channel, and the unit
 *     v                   factor (checked by dimension: pA against mV is an
 *   Map                   error, nA against pA is fine)
 *     |  stim_check()     every sample against both limits, never silently
 *     v                   clipped
 *     |  stim_drive()     volts for every analog output (calibration applied)
 *   Drive                 and 0/1 for every digital line, rest samples added
 *
 * Only the sg_api_* functions of StimGen are called (see stimgen_api.h), and
 * all of them are called from stim.c.
 *
 * Determinism.  A trial is rendered twice in a real run: once to check it
 * before anything is played and once to play it.  Both renders give the same
 * samples, because stim_open() fixes every seed: the master seed of a plain
 * stimulus (read back from the first render), and the protocol seed of a
 * protocol (the trial seeds are in the trial list).
 */
#ifndef LCG2_STIM_H
#define LCG2_STIM_H

#include <stdint.h>
#include "cli.h"
#include "config.h"
#include "lcg2.h"

/* Number of rest samples appended to the end of every output buffer, so that a
 * clocked task that holds its last sample holds the rest value.  Provisional:
 * the hardware spike (S1) decides how many are needed. */
#define STIM_REST_PAD 2

/* One channel of a rendered trial, in the unit the stimulus states. */
typedef struct {
    char    name[CFG_NAME];
    char    unit[16];            /* "pA", "mV", "1" for a digital line       */
    double  rest;                /* the stimulus's rest value (its unit)     */
    int     digital;
    double *x;                   /* n samples (owned)                        */
} SChan;

typedef struct { char name[64]; int64_t sample; } Marker;

/* One rendered trial. */
typedef struct {
    int      index;              /* 0-based position in the run              */
    char     name[64];           /* "0003", or "stimulus"                    */
    char     seed[24];           /* the master seed of this trial, decimal   */
    char    *info;               /* the protocol's record of this trial (JSON),
                                    NULL for a plain stimulus                */
    double   rate;               /* Hz, as StimGen reports it                */
    int64_t  rate_p, rate_q;     /* the same, as an exact fraction           */
    int64_t  n;                  /* samples per channel                      */
    int      nch;
    SChan    ch[CFG_MAX_OUT];
    int      nmark;
    Marker  *mark;
    char     samples_sha256[72]; /* StimGen's digest of the samples          */
    int      nwarn;
    char   **warn;               /* StimGen's warnings                       */
    int      nfiles;
    char   **file_path, **file_sha;   /* files the stimulus read             */
} Trial;

void trial_free(Trial *t);

typedef struct {
    char *name, *seed, *text, *info;  /* one trial of a protocol (owned)     */
} TrialDesc;

/* Where the seed of a stimulus (or the protocol seed) came from. */
enum { SEED_DRAWN = 0,           /* drawn at random for this check            */
       SEED_GIVEN = 1,           /* -s                                       */
       SEED_STATED = 2 };        /* written in the stimulus or protocol      */

/* An opened stimulus. */
typedef struct {
    int        is_protocol;
    int        waveform;         /* plain waveform: one channel, sent to -T  */
    char      *text;             /* owned                                    */
    char       source[300];      /* "examples/pair.sg" or "the -e text"      */
    char       unit[16];         /* waveform: the unit handed to StimGen     */
    char       rate_arg[48];     /* the rate text handed to StimGen          */
    char       seed_arg[24];     /* a plain stimulus: its fixed seed         */
    int        seed_source;      /* SEED_DRAWN, SEED_GIVEN or SEED_STATED    */
    int        n;                /* trials                                   */
    TrialDesc *td;               /* protocols: the trial list                */
    char       protocol_seed[24];
    char       timing[80];       /* "period 5s", "gap 2s", ...               */
    char       start[80];        /* "immediately" or "on trigger"            */
    char      *protocol_json;    /* the expanded list, kept for the archive  */
    char      *digest;           /* n digests of 72 bytes, from the check pass */
    int        have_digest;
} Stim;

/* The check pass remembers the digest StimGen gives for the samples of each
 * trial; the engine compares it with the digest of the trial it re-renders, so
 * that a buffer that differs from the one that was checked is never played. */
int  stim_remember_digest(Stim *st, int i, const char *sha);
int  stim_digest_matches(const Stim *st, int i, const char *sha);

/* Read a text file whole.  A NUL byte, a file over 64 MiB, or a missing file
 * is an error.  *text is malloc'ed. */
int stim_read_file(const char *path, char **text, Err *err);

/* Open a stimulus.  Takes ownership of text (also on failure).  `source` names
 * it in messages.  Needs: opt (rate, seed, -T), cfg (mode, outputs, rate).
 * Checks the kind, the rules about -T, and renders the first trial once, to
 * learn which rate applies (see SPEC 4.1) and to fix the seed. */
int stim_open(Stim *st, char *text, const char *source, const Options *opt,
              const Config *cfg, Err *err);

void stim_free(Stim *st);

/* Render trial i (0 <= i < st->n) into t (free it with trial_free). */
int stim_render(const Stim *st, int i, Trial *t, Err *err);

/* Which output carries each channel of a trial, and the factor that turns the
 * stimulus's unit into the unit of that output. */
typedef struct { int out; double scale; } Map;

int stim_map(const Stim *st, const Trial *t, const Options *opt, const Config *cfg,
             Map *map, Err *err);

/* Check every sample of every named analog channel, and its rest value,
 * against the limits of its output (out_check_value in config.c).  Refuses
 * with the trial, the channel, the first sample and how many would clip. */
int stim_check(const Trial *t, const Map *map, const Config *cfg, double *peak_volts,
               Err *err);
/* peak_volts, if not NULL, has one entry per channel of the trial; each is
 * raised to the largest |volts| that channel asks of the amplifier command
 * (before calibration), so that the caller can report how close to its limit
 * a stimulus comes. */

/* The signals for the backend: one array per output in use, each of the
 * trial's n samples followed by STIM_REST_PAD rest samples.  Named channels
 * carry the stimulus (analog: volts, with the calibration applied; digital:
 * 0 or 1); outputs the stimulus does not name hold their rest value. */
typedef struct {
    int64_t n;                   /* samples of the trial, without the rest   */
    int     pad;                 /* STIM_REST_PAD                            */
    int     nao;
    struct { int out; double *v; } ao[4];
    int     ndig;
    struct { int out; unsigned char *b; } dig[8];
    /* The state every output ends on, which is also the state it rests at
     * between trials and after the run: the value of the rest samples. */
    double        ao_rest[4];            /* volts, for ao[k]   */
    unsigned char dig_rest[8];           /* 0 or 1, for dig[k] */
} Drive;

int stim_drive(const Trial *t, const Map *map, const Config *cfg, Drive *d, Err *err);

/* A drive with every output at its rest value from the configuration, for n
 * samples (and the rest padding): what a record-only run (-t) plays. */
int stim_drive_idle(const Config *cfg, int64_t n, Drive *d, Err *err);

void drive_free(Drive *d);

/* The exact fraction p/q of a rate that StimGen reports as a double.  StimGen's
 * rates are decimal (q a power of ten up to 10^6); returns -1 for anything
 * else.  Exposed for the tests. */
int stim_rate_fraction(double hz, int64_t *p, int64_t *q);

#endif /* LCG2_STIM_H */
