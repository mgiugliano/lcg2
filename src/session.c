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
 * session.c -- reading and checking everything, before anything is played.
 * See session.h.
 *
 * The checks live here (they were in main.c up to milestone M3): those that need
 * the command line and the configuration together (check_plan), the reading of the
 * stimulus through StimGen and the check of every trial (check_stimulus), and the
 * plan that -n prints.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend_neuron.h"
#include "platform.h"
#include "session.h"
#include "units.h"

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

/* What reading and checking the stimulus found (for the plan). */
/* The text of the stimulus: the file or the -e text.  Owned by the caller of
 * stim_open, which takes it over. */
static int load_text(const Options *o, char **text, const char **source, Err *err)
{
    if (o->text) {
        size_t n = strlen(o->text) + 1;
        *text = (char *)malloc(n);
        if (!*text) return err_set(err, "out of memory");
        memcpy(*text, o->text, n);
        *source = "the -e text";
        return 0;
    }
    *source = o->stimulus;
    return stim_read_file(o->stimulus, text, err);
}

/*
 * Read the stimulus and check all of it, before anything could be played:
 * render every trial, find the output of every channel, check units and every
 * sample against the limits.  The first problem stops everything.  (The same
 * trials are rendered again when they are played; the seeds are fixed, so the
 * samples are the same.)
 */
static int check_stimulus(Session *s, Err *err)
{
    const Options *o = &s->opt;
    const Config *c = &s->config;
    Stim *stim = &s->stim;
    Report *report = &s->report;
    char *text;
    const char *source = NULL;
    int i, k;

    if (load_text(o, &text, &source, err) < 0) return -1;
    if (stim_open(stim, text, source, o, c, err) < 0) return -1;

    for (i = 0; i < stim->n; i++) {
        Trial t;
        Map map[CFG_MAX_OUT];
        double peak[CFG_MAX_OUT] = { 0 };

        if (stim_render(stim, i, &t, err) < 0) return -1;
        if (i == 0) {
            report->rate_p = t.rate_p;
            report->rate_q = t.rate_q;
        } else if (t.rate_p != report->rate_p || t.rate_q != report->rate_q) {
            err_set(err, "%s: trial %s has another sampling rate than the first trial",
                    stim->source, t.name);                 /* before the name is freed */
            trial_free(&t);
            return -1;
        }
        if (stim_map(stim, &t, o, c, map, err) < 0 ||
            stim_check(&t, map, c, peak, err) < 0) {
            trial_free(&t);
            return -1;
        }
        for (k = 0; k < t.nwarn; k++) {
            report->nwarn++;
            if (stim_note_warning(stim, &t, t.warn[k]) < 0) { trial_free(&t); return err_set(err, "out of memory"); }
            if (!o->quiet)
                fprintf(stderr, "lcg2: warning: %s%s%s: %s\n", stim->source,
                        t.info ? " trial " : "", t.info ? t.name : "", t.warn[k]);
        }
        if (o->verbose)
            fprintf(stderr, "lcg2: trial %s: %lld samples, seed %s\n", t.name,
                    (long long)t.n, t.seed);
        if (stim_note_files(stim, &t) < 0 || stim_remember_digest(stim, i, t.samples_sha256) < 0) {
            trial_free(&t);
            return err_set(err, "out of memory");
        }
        report->total_samples += t.n;
        for (k = 0; k < t.nch; k++)
            if (peak[k] > report->peak[k]) report->peak[k] = peak[k];
        if (i == 0) {
            report->first = t;
            memcpy(report->map, map, sizeof map);
        } else {
            trial_free(&t);
        }
    }
    report->ntrials = stim->n;

    /* The rate of the stimulus has to be possible on this board too. */
    return config_check_rate(c, report->rate_p, report->rate_q, err);
}

/* Print what would happen, in words (the answer to -n, on stdout). */
int session_print_plan(const Session *s, Err *err)
{
    const Options *o = &s->opt;
    const Table *t = &s->table;
    const Config *c = &s->config;
    const Report *rep = &s->report;
    const Stim *stim = &s->stim;
    int i, m = MODE_INDEX(c->mode);
    char r[48];

    printf("LCG2 %s, dry run: the plan\n\n", LCG2_VERSION);
    printf("  configuration  %s (profile: %s)\n", t->path,
           t->profile[0] ? t->profile : "none");
    printf("  device         %s%s\n", c->device_name,
           c->device == DEV_SIM ? "  (built-in loopback simulator)" :
           c->device == DEV_NEURON ? "  (NEURON simulator)" : "");
    printf("  mode           %s\n", config_mode_name(c->mode));

    if (rep && rep->ntrials) {                      /* the stimulus has been read */
        units_format_rate(r, sizeof r, rep->rate_p, rep->rate_q);
        printf("  rate           %s (%s)\n", r,
               o->has_rate ? "from -r" :
               stim->rate_arg[0] ? "from the configuration; the stimulus states none"
                                : "stated by the stimulus");
    } else if (o->has_rate) {
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
    if (rep && rep->ntrials) {
        const char *how = stim->seed_source == SEED_GIVEN ? "from -s" :
            stim->seed_source == SEED_STATED ? "stated by the stimulus" :
            "drawn for this check; a run draws its own unless you give -s";
        if (stim->is_protocol)
            printf("  seed           protocol seed %s (%s)\n", stim->protocol_seed, how);
        else
            printf("  seed           %s (%s)\n", stim->seed_arg, how);
    } else
        printf("  seed           %s\n", o->seed ? o->seed : "drawn at the start and recorded");

    if (o->has_duration)
        printf("  work           record only, %.6g s\n",
               (double)o->duration_ps / 1e12);
    else if (o->stimulus)
        printf("  work           play the stimulus in %s\n", o->stimulus);
    else
        printf("  work           play the stimulus text given with -e\n");
    if (o->target) printf("  target         plain waveform to %s (-T)\n", o->target);
    if (o->prefix) printf("  file prefix    %s\n", o->prefix);

    if (rep && rep->ntrials) {
        const Trial *f = &rep->first;
        char idle[200] = "";
        int k, j;

        if (stim->is_protocol)
            printf("  stimulus       a protocol of %d trials: %s, start %s; each trial %lld "
                   "samples (%.6g s)\n", rep->ntrials, stim->timing, stim->start,
                   (long long)f->n, (double)f->n / f->rate);
        else
            printf("  stimulus       %s with %d channel(s), %lld samples (%.6g s)\n",
                   stim->waveform ? "a plain waveform" : "a stimulus", f->nch,
                   (long long)f->n, (double)f->n / f->rate);
        for (k = 0; k < f->nch; k++) {
            const OutCh *oc = &c->out[rep->map[k].out];
            printf("  %-14s %s (%s)  ->  [out %s] on %s", k ? "" : "channels",
                   stim->waveform ? "waveform" : f->ch[k].name,
                   f->ch[k].digital ? "digital" : f->ch[k].unit, oc->name, oc->line);
            if (!oc->digital) {
                printf(", peak %.4g V", rep->peak[k]);
                if (oc->has_limit) printf(" of the limit %g V", oc->limit);
            }
            printf("\n");
        }
        for (j = 0; j < c->n_out; j++) {
            int named = 0;
            for (k = 0; k < f->nch; k++) named |= rep->map[k].out == j;
            if (!named && !c->out[j].skip) {
                if (idle[0]) strncat(idle, ", ", sizeof idle - strlen(idle) - 1);
                strncat(idle, c->out[j].name, sizeof idle - strlen(idle) - 1);
            }
        }
        if (idle[0]) printf("  %-14s %s (not named: they hold their rest value)\n", "idle", idle);
        printf("  %-14s %d trial(s) rendered and checked: no sample clips%s\n", "check",
               rep->ntrials, rep->nwarn ? " (StimGen warnings were printed)" : "");
    }

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
    if (c->device == DEV_NEURON) {
        /* Build the cells for real (not play anything): it is the only way to know that the model files
         * are found, run, and give sections where the electrodes are meant to be. */
        NeuronOpts no;
        char script[1200], logpath[300];
        int have_log;
        memset(&no, 0, sizeof no);
        neuron_script_path(c, script, sizeof script);
        have_log = platform_temp_file(logpath, sizeof logpath) == 0;
        if (!have_log) strcpy(logpath, "/dev/null");
        no.python = c->sim_python; no.script = script; no.stderr_path = logpath;
        fflush(stdout);
        if (neuron_check(&no, c, err) < 0) {
            if (have_log) platform_delete_file(logpath);
            return -1;
        }
        if (have_log) platform_delete_file(logpath);
    }
    printf("\nnothing was played.\n");
    return 0;
}


int session_prepare(Session *s, int argc, char **argv, Err *err)
{
    s->argc = argc;
    s->argv = argv;
    if (cli_parse(argc, argv, &s->opt, err) < 0) return -1;
    if (s->opt.help) {
        cli_usage(stdout);
        return 1;
    }
    if (config_load(&s->table, s->opt.config ? s->opt.config : "lcg2.ini", err) < 0) return -1;
    if (s->opt.profile && config_apply_profile(&s->table, s->opt.profile, err) < 0) return -1;
    if (s->opt.mode != MODE_NONE &&
        config_override(&s->table, "rig", "mode", config_mode_name(s->opt.mode), err) < 0)
        return -1;
    if (config_build(&s->table, s->opt.mode, &s->config, err) < 0) return -1;
    if (check_plan(&s->opt, &s->config, err) < 0) return -1;
    if (!s->opt.has_duration && check_stimulus(s, err) < 0) return -1;
    return 0;
}
