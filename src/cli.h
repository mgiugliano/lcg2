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
 * cli.h -- the command line.
 *
 *   lcg2 [options] STIMULUS.sg          play a stimulus or protocol and record
 *   lcg2 [options] -e 'TEXT'            the same, the text given inline
 *   lcg2 [options] -t DURATION          record only, outputs at rest
 *
 * Options are written with their value as a separate word ("-r 20kHz", not
 * "-r20kHz"); short options cannot be combined ("-n -v", not "-nv").  This
 * is deliberately simpler than getopt(), which is POSIX and not ISO C.
 */
#ifndef LCG2_CLI_H
#define LCG2_CLI_H

#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "lcg2.h"

typedef struct {
    int         help;            /* -h: print the help and stop            */
    const char *config;          /* -c FILE (NULL: lcg2.ini)               */
    const char *profile;         /* -P NAME (NULL: none)                   */
    Mode        mode;            /* -m cc|vc (MODE_NONE: not given)        */
    int         has_rate;        /* -r given                               */
    int64_t     rate_p, rate_q;  /*   its exact value p/q Hz               */
    const char *seed;            /* -s SEED, checked: a non-negative integer */
    const char *target;          /* -T NAME: pipette for a plain waveform  */
    int         has_duration;    /* -t given                               */
    int64_t     duration_ps;     /*   its value in picoseconds             */
    const char *text;            /* -e TEXT                                */
    const char *prefix;          /* -p PREFIX                              */
    const char *stimulus;        /* the stimulus file                      */
    int         dry, quiet, verbose;
} Options;

/* Read argv into o.  Returns 0 (also when -h was seen: then o->help is set
 * and nothing else is checked) or -1 with a message. */
int cli_parse(int argc, char **argv, Options *o, Err *err);

/* The help text. */
void cli_usage(FILE *f);

#endif /* LCG2_CLI_H */
