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
 * cli.c -- the command line.  See cli.h for the shape of a command.
 *
 * cli_parse() only looks at the command line itself: which options are
 * present, whether their values are well formed, and which combinations make
 * no sense.  Everything that needs the configuration file (does the pipette
 * named by -T exist? is the rate possible on this board?) is checked later,
 * in main.c, once the file has been read.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "lcg2.h"
#include "units.h"

void cli_usage(FILE *f)
{
    fputs(
"usage: lcg2 [options] STIMULUS.sg      play a stimulus or protocol, record\n"
"       lcg2 [options] -e 'TEXT'        the same, with the text on the command line\n"
"       lcg2 [options] -t DURATION      record only (outputs stay at rest)\n"
"\n"
"options (give the value as a separate word, e.g. -r 20kHz):\n"
"  -c FILE     configuration file (default: lcg2.ini)\n"
"  -P NAME     rig profile: the [profile NAME] section of the file\n"
"  -m cc|vc    amplifier mode: current clamp or voltage clamp\n"
"  -r RATE     sampling rate, e.g. 20kHz or 10000 (Hz)\n"
"  -s SEED     seed of the random generators, a non-negative integer\n"
"  -T NAME     send a plain waveform to the pipette NAME (an [out NAME])\n"
"  -t DURATION record only, for that long, e.g. 30s or 2min\n"
"  -e TEXT     stimulus text given here instead of in a file\n"
"  -p PREFIX   prefix of the names of the output files\n"
"  -n          dry run: check everything, print the plan, play nothing\n"
"  -R          real time: a simulator waits as a real board would (the default\n"
"              is virtual time: as fast as possible, with exact simulated times)\n"
"  -L          live patching (real time): keys act on a patching pipette while the\n"
"              seal test runs: c touch, s suction, b break in, z zap, h holding\n"
"              0/-70 mV, q stop (docs/16, tutorial L)\n"
"  -q, -v      quieter, more verbose\n"
"  -h          this help\n"
"\n"
"exit status: 0 success, 1 error before the run, 2 aborted or failed during it\n"
"\n"
"A run writes STEM.h5 (the data) and the hidden folder .STEM/ (configuration, command,\n"
"stimulus, log, checksums) in the current folder. Manual: docs/README.md.\n",
          f);
    fprintf(f, "LCG2 %s, https://github.com/mgiugliano/lcg2\n", LCG2_VERSION);
}

/* Digits only, and small enough for 64 bits (StimGen's seed rule). */
static int valid_seed(const char *s)
{
    char *end;
    if (!*s || strspn(s, "0123456789") != strlen(s)) return 0;
    errno = 0;
    (void)strtoull(s, &end, 10);
    return errno != ERANGE;
}

/* The prefix becomes part of file and folder names: no path separators, no
 * leading dot (the run folder adds its own), only plain characters. */
static int valid_prefix(const char *s)
{
    if (!*s || *s == '.') return 0;
    return strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                     "0123456789_.-") == strlen(s);
}

int cli_parse(int argc, char **argv, Options *o, Err *err)
{
    /* seen[c] says option -c was already given, to refuse "-r 1 -r 2". */
    unsigned char seen[256];
    int i, sources;

    memset(o, 0, sizeof *o);
    memset(seen, 0, sizeof seen);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        char opt;
        const char *val;

        if (a[0] != '-' || a[1] == 0) {              /* the stimulus file */
            if (a[0] == '-')
                return err_set(err, "'-' (standard input) is not supported; give a "
                               "file name, -e TEXT or -t DURATION");
            if (o->stimulus)
                return err_set(err, "two stimulus files given ('%s' and '%s'); LCG2 "
                               "plays one", o->stimulus, a);
            o->stimulus = a;
            continue;
        }
        if (a[2] != 0)
            return err_set(err, "unknown option '%s' (options are one letter, with "
                           "their value as a separate word; try -h)", a);
        opt = a[1];
        if (opt == 'h') { o->help = 1; return 0; }
        if (seen[(unsigned char)opt])
            return err_set(err, "option -%c is given twice", opt);

        switch (opt) {
        case 'n': o->dry = 1; break;
        case 'R': o->realtime = 1; break;
        case 'L': o->live = 1; o->realtime = 1; break;
        case 'q': o->quiet = 1; break;
        case 'v': o->verbose = 1; break;
        case 'c': case 'P': case 'm': case 'r': case 's': case 'T':
        case 't': case 'e': case 'p':
            if (i + 1 >= argc)
                return err_set(err, "option -%c needs a value", opt);
            val = argv[++i];
            if (!*val && opt != 'e')
                return err_set(err, "option -%c has an empty value", opt);
            switch (opt) {
            case 'c': o->config = val; break;
            case 'P': o->profile = val; break;
            case 'T': o->target = val; break;
            case 'e': o->text = val; break;
            case 'm':
                if (strcmp(val, "cc") == 0) o->mode = MODE_CC;
                else if (strcmp(val, "vc") == 0) o->mode = MODE_VC;
                else return err_set(err, "-m: expected cc or vc, got '%s'", val);
                break;
            case 'r':
                if (units_parse_rate(val, &o->rate_p, &o->rate_q) < 0)
                    return err_set(err, "-r: '%s' is not a rate (examples: 20kHz, "
                                   "10000, 2.5Hz)", val);
                o->has_rate = 1;
                o->rate_text = val;
                break;
            case 't':
                if (units_parse_duration(val, &o->duration_ps) < 0)
                    return err_set(err, "-t: '%s' is not a duration (examples: 30s, "
                                   "500ms, 2min; it must be above 0)", val);
                o->has_duration = 1;
                break;
            case 's':
                if (!valid_seed(val))
                    return err_set(err, "-s: the seed must be a non-negative "
                                   "integer, got '%s'", val);
                o->seed = val;
                break;
            case 'p':
                if (!valid_prefix(val))
                    return err_set(err, "-p: the prefix may contain only letters, "
                                   "digits, '_', '-' and '.', and must not start "
                                   "with '.' (got '%s')", val);
                o->prefix = val;
                break;
            }
            break;
        default:
            return err_set(err, "unknown option '%s' (try -h)", a);
        }
        seen[(unsigned char)opt] = 1;
    }

    /* Exactly one source of work: a file, -e, or -t. */
    sources = (o->stimulus != NULL) + (o->text != NULL) + o->has_duration;
    if (sources == 0)
        return err_set(err, "nothing to do: give a stimulus file, -e TEXT or "
                       "-t DURATION (try -h)");
    if (sources > 1)
        return err_set(err, "give only one of: a stimulus file, -e TEXT, "
                       "-t DURATION");
    if (o->target && o->has_duration)
        return err_set(err, "-T names the pipette that receives a stimulus, so it "
                       "cannot be used with -t (record only)");
    if (o->quiet && o->verbose)
        return err_set(err, "-q and -v contradict each other");
    return 0;
}
