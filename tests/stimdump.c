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
 * stimdump.c -- a test driver for the stimulus module (milestone M2).
 *
 *     stimdump MODE DIR [lcg2 options ...] [STIMULUS.sg]
 *
 * It runs the same steps as lcg2 (command line, configuration, profile, mode,
 * stim_open, stim_render, stim_map, stim_check) with the same options, and
 * then writes what LCG2 holds, so that a test can compare it with what StimGen's
 * own tool `sg` writes for the same text:
 *
 *   samples   DIR/NAME.txt per trial: the samples as decoded from the .sgb,
 *             in the format of `sg render --text` (time, then one column per
 *             channel, %.17g so that the text carries every bit)
 *   volts     DIR/NAME.txt per trial: the volts (or 0/1) that stim_drive()
 *             makes for every output in use, rest samples included
 *   twice     renders every trial twice, with no seed given, and says whether
 *             the two renders are bit for bit equal (they must be: stim_open
 *             fixes the seeds)
 *   expand    the trial list of a protocol, as JSON, exactly as StimGen gave it
 *   list      one line per trial on standard output: name, seed, samples,
 *             rate, and the timing of a protocol
 *
 * Exit status 0, or 1 with the message on standard error (a refusal is a
 * result the tests look at).  Not part of lcg2; built by `make tools`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "config.h"
#include "stim.h"

static Table  table;
static Config config;
static Stim   stim;

static int die(const Err *e)
{
    fprintf(stderr, "lcg2: %s\n", e->msg);
    return 1;
}

int main(int argc, char **argv)
{
    Options opt;
    Err err;
    char *text;
    const char *source, *mode;
    char path[600];
    int i, k;

    if (argc < 3) {
        fprintf(stderr, "usage: stimdump samples|volts|list DIR [lcg2 options] [file]\n");
        return 2;
    }
    mode = argv[1];
    if (cli_parse(argc - 2, argv + 2, &opt, &err) < 0) return die(&err);
    if (config_load(&table, opt.config ? opt.config : "lcg2.ini", &err) < 0) return die(&err);
    if (opt.profile && config_apply_profile(&table, opt.profile, &err) < 0) return die(&err);
    if (config_build(&table, opt.mode, &config, &err) < 0) return die(&err);

    if (opt.text) {
        text = (char *)malloc(strlen(opt.text) + 1);
        strcpy(text, opt.text);
        source = "the -e text";
    } else {
        source = opt.stimulus;
        if (stim_read_file(opt.stimulus, &text, &err) < 0) return die(&err);
    }
    if (stim_open(&stim, text, source, &opt, &config, &err) < 0) return die(&err);
    if (strcmp(mode, "expand") == 0) {
        printf("%s\n", stim.protocol_json ? stim.protocol_json : "");
        stim_free(&stim);
        return 0;
    }

    for (i = 0; i < stim.n; i++) {
        Trial t;
        Map map[CFG_MAX_OUT];
        FILE *f;
        int64_t s;

        if (stim_render(&stim, i, &t, &err) < 0) return die(&err);
        if (stim_map(&stim, &t, &opt, &config, map, &err) < 0 ||
            stim_check(&t, map, &config, NULL, &err) < 0)
            return die(&err);

        if (strcmp(mode, "twice") == 0) {
            Trial u;
            int same;
            if (stim_render(&stim, i, &u, &err) < 0) return die(&err);
            same = u.n == t.n && u.nch == t.nch && strcmp(u.seed, t.seed) == 0;
            for (k = 0; same && k < t.nch; k++)
                same = memcmp(u.ch[k].x, t.ch[k].x, (size_t)t.n * sizeof(double)) == 0;
            printf("%s %s\n", t.name, same ? "same" : "DIFFERENT");
            trial_free(&u);
        } else if (strcmp(mode, "list") == 0) {
            printf("%s %s %lld %.17g %s | %s\n", t.name, t.seed, (long long)t.n, t.rate,
                   stim.timing, stim.start);
        } else if (strcmp(mode, "samples") == 0) {
            snprintf(path, sizeof path, "%s/%s.txt", argv[2], t.name);
            f = fopen(path, "w");
            if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            fprintf(f, "# t[s]");
            for (k = 0; k < t.nch; k++) fprintf(f, "\t%s[%s]", t.ch[k].name, t.ch[k].unit);
            fprintf(f, "\n");
            for (s = 0; s < t.n; s++) {
                fprintf(f, "%.9g", (double)s / t.rate);
                for (k = 0; k < t.nch; k++) fprintf(f, "\t%.17g", t.ch[k].x[s]);
                fprintf(f, "\n");
            }
            fclose(f);
        } else if (strcmp(mode, "volts") == 0) {
            Drive d;
            if (stim_drive(&t, map, &config, &d, &err) < 0) return die(&err);
            snprintf(path, sizeof path, "%s/%s.txt", argv[2], t.name);
            f = fopen(path, "w");
            if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            fprintf(f, "# n=%lld pad=%d", (long long)d.n, d.pad);
            for (k = 0; k < d.nao; k++) fprintf(f, "\t%s(V)", config.out[d.ao[k].out].name);
            for (k = 0; k < d.ndig; k++) fprintf(f, "\t%s(0/1)", config.out[d.dig[k].out].name);
            fprintf(f, "\n");
            for (s = 0; s < d.n + d.pad; s++) {
                fprintf(f, "%lld", (long long)s);
                for (k = 0; k < d.nao; k++) fprintf(f, "\t%.17g", drive_ao(&d, k, s));
                for (k = 0; k < d.ndig; k++) fprintf(f, "\t%d", drive_dig(&d, k, s));
                fprintf(f, "\n");
            }
            fclose(f);
            drive_free(&d);
        } else {
            fprintf(stderr, "unknown mode '%s'\n", mode);
            return 2;
        }
        trial_free(&t);
    }
    stim_free(&stim);
    return 0;
}
