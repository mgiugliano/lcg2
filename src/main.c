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
 * main.c -- LCG2: play a StimGen 2 stimulus and record, from the command line.
 *
 * This build is milestones M0 and M1: it reads the command line and the
 * configuration file, applies the rig profile, checks everything, and prints
 * the plan (-n).  Playing and recording arrive in the next milestones; until
 * then a run without -n stops with an explanation and exit status 1.
 *
 * main() is the only place that prints errors and chooses the exit status.
 * The modules below it return -1 and a message instead.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "config.h"
#include "units.h"

/* Static, not on the stack: the table is a few hundred kilobytes. */
static Table  table;
static Config config;

static int fail(const Err *e)
{
    fprintf(stderr, "lcg2: %s\n", e->msg);
    return 1;                                 /* error before the run */
}

/* Checks that need both the command line and the configuration. */
static int check_plan(const Options *o, const Config *c, Err *err)
{
    int i, active_out = 0;

    for (i = 0; i < c->n_out; i++) active_out += !c->out[i].skip;

    /* A stimulus file must exist and be readable.  (Its content is read and
     * checked by StimGen from milestone M2 on.) */
    if (o->stimulus) {
        FILE *f = fopen(o->stimulus, "r");
        if (!f)
            return err_set(err, "cannot open stimulus file '%s': %s", o->stimulus,
                           strerror(errno));
        fclose(f);
    }

    /* Playing needs somewhere to play. */
    if (!o->has_duration && active_out == 0)
        return err_set(err, "no output to play on: the configuration has no "
                       "[out NAME] section that is in use (use -t to record only)");

    /* -T names the pipette that receives a plain waveform. */
    if (o->target) {
        const OutCh *hit = NULL;
        for (i = 0; i < c->n_out; i++)
            if (strcmp(c->out[i].name, o->target) == 0) hit = &c->out[i];
        if (!hit) {
            char list[200] = "";
            for (i = 0; i < c->n_out; i++)
                if (!c->out[i].skip && !c->out[i].digital) {
                    if (list[0]) strncat(list, ", ", sizeof list - strlen(list) - 1);
                    strncat(list, c->out[i].name, sizeof list - strlen(list) - 1);
                }
            return err_set(err, "-T: there is no output named '%s' (analog outputs "
                           "in use: %s)", o->target, list[0] ? list : "none");
        }
        if (hit->skip)
            return err_set(err, "-T: output '%s' is skipped in this configuration",
                           o->target);
        if (hit->digital)
            return err_set(err, "-T: output '%s' is a digital line; a plain waveform "
                           "needs an analog output", o->target);
    }

    /* The rate that the command line or the file asks for must be possible
     * on this board.  (A rate written in the stimulus is checked in M2.) */
    if (o->has_rate && config_check_rate(c, o->rate_p, o->rate_q, err) < 0)
        return -1;
    if (!o->has_rate && c->has_rate &&
        config_check_rate(c, c->rate_p, c->rate_q, err) < 0)
        return -1;
    return 0;
}

/* Print what would happen, in words (the answer to -n, on stdout). */
static void print_plan(const Options *o, const Table *t, const Config *c)
{
    int i, m = MODE_INDEX(c->mode);
    char r[48];

    printf("LCG2 %s, dry run: the plan\n\n", LCG2_VERSION);
    printf("  configuration  %s (profile: %s)\n", t->path,
           t->profile[0] ? t->profile : "none");
    printf("  device         %s%s\n", c->device_name,
           c->device == DEV_SIM ? "  (built-in loopback simulator)" :
           c->device == DEV_NEURON ? "  (NEURON simulator)" : "");
    printf("  mode           %s\n", config_mode_name(c->mode));

    if (o->has_rate) {
        units_format_rate(r, sizeof r, o->rate_p, o->rate_q);
        printf("  rate           %s (from -r)\n", r);
    } else if (c->has_rate) {
        units_format_rate(r, sizeof r, c->rate_p, c->rate_q);
        printf("  rate           %s (from the configuration, unless the stimulus "
               "states one)\n", r);
    } else {
        printf("  rate           from the stimulus (none in -r or the "
               "configuration)\n");
    }
    printf("  seed           %s\n", o->seed ? o->seed : "drawn at the start and recorded");

    if (o->has_duration)
        printf("  work           record only, %.6g s\n",
               (double)o->duration_ps / 1e12);
    else if (o->stimulus)
        printf("  work           play the stimulus in %s (read and checked from M2 on)\n",
               o->stimulus);
    else
        printf("  work           play the stimulus text given with -e\n");
    if (o->target) printf("  target         plain waveform to %s (-T)\n", o->target);
    if (o->prefix) printf("  file prefix    %s\n", o->prefix);

    printf("\n  outputs\n");
    for (i = 0; i < c->n_out; i++) {
        const OutCh *oc = &c->out[i];
        if (oc->skip)
            printf("    %-8s skipped\n", oc->name);
        else if (oc->digital)
            printf("    %-8s -> %s (digital, clocked)\n", oc->name, oc->line);
        else {
            printf("    %-8s -> %s  %g %s per volt", oc->name, oc->line,
                   oc->gain[m], oc->unit[m]);
            if (oc->has_limit) printf(", limit %g V", oc->limit);
            printf(", rest %g %s\n", oc->rest, oc->unit[m]);
        }
    }
    if (c->n_out == 0) printf("    (none)\n");

    printf("\n  inputs (all are recorded)\n");
    for (i = 0; i < c->n_in; i++) {
        const InCh *ic = &c->in[i];
        if (ic->skip)
            printf("    %-8s skipped\n", ic->name);
        else
            printf("    %-8s <- %s  %g %s per volt, range %g V, %s\n", ic->name,
                   ic->line, ic->gain[m], ic->unit[m], ic->range, ic->terminal);
    }
    if (c->device == DEV_NEURON)
        printf("\n  virtual preparation: %d cell(s), %d synapse(s) (checked later)\n",
               c->n_cells, c->n_synapses);
    printf("\nnothing was played.\n");
}

int main(int argc, char **argv)
{
    Options opt;
    Err err;

    if (cli_parse(argc, argv, &opt, &err) < 0) return fail(&err);
    if (opt.help) {
        cli_usage(stdout);
        return 0;
    }

    if (config_load(&table, opt.config ? opt.config : "lcg2.ini", &err) < 0)
        return fail(&err);
    if (opt.profile && config_apply_profile(&table, opt.profile, &err) < 0)
        return fail(&err);
    if (opt.mode != MODE_NONE &&
        config_override(&table, "rig", "mode", config_mode_name(opt.mode), &err) < 0)
        return fail(&err);
    if (config_build(&table, opt.mode, &config, &err) < 0) return fail(&err);
    if (check_plan(&opt, &config, &err) < 0) return fail(&err);

    if (opt.dry) {
        print_plan(&opt, &table, &config);
        if (opt.verbose) {
            printf("\nresolved configuration:\n\n");
            config_dump(&table, stdout);
        }
        return 0;
    }

    fprintf(stderr, "lcg2: playing and recording are not implemented yet in this "
            "build (milestones M0 and M1); use -n to check a configuration\n");
    return 1;
}
