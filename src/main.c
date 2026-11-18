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
 * This build is milestones M0 to M3: it reads the command line and the
 * configuration file, applies the rig profile, reads the stimulus through
 * StimGen, checks everything (units, limits, the rate), and either prints the
 * plan (-n) or runs the trials on the loopback simulator.  A run writes no data
 * file yet: recording is milestone M4.  Other devices (NEURON, the board) are
 * refused with a message.
 *
 * main() is the only place that prints errors and chooses the exit status.
 * The modules below it return -1 and a message instead.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "backend_sim.h"
#include "cli.h"
#include "config.h"
#include "engine.h"
#include "platform.h"
#include "stim.h"
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

static Stim   stim;
static Report report;

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
static int check_stimulus(const Options *o, const Config *c, Err *err)
{
    char *text;
    const char *source;
    int i, k;

    if (load_text(o, &text, &source, err) < 0) return -1;
    if (stim_open(&stim, text, source, o, c, err) < 0) return -1;

    for (i = 0; i < stim.n; i++) {
        Trial t;
        Map map[CFG_MAX_OUT];
        double peak[CFG_MAX_OUT] = { 0 };

        if (stim_render(&stim, i, &t, err) < 0) return -1;
        if (i == 0) {
            report.rate_p = t.rate_p;
            report.rate_q = t.rate_q;
        } else if (t.rate_p != report.rate_p || t.rate_q != report.rate_q) {
            err_set(err, "%s: trial %s has another sampling rate than the first trial",
                    stim.source, t.name);                 /* before the name is freed */
            trial_free(&t);
            return -1;
        }
        if (stim_map(&stim, &t, o, c, map, err) < 0 ||
            stim_check(&t, map, c, peak, err) < 0) {
            trial_free(&t);
            return -1;
        }
        for (k = 0; k < t.nwarn; k++) {
            report.nwarn++;
            if (!o->quiet)
                fprintf(stderr, "lcg2: warning: %s%s%s: %s\n", stim.source,
                        t.info ? " trial " : "", t.info ? t.name : "", t.warn[k]);
        }
        if (o->verbose)
            fprintf(stderr, "lcg2: trial %s: %lld samples, seed %s\n", t.name,
                    (long long)t.n, t.seed);
        if (stim_remember_digest(&stim, i, t.samples_sha256) < 0) {
            trial_free(&t);
            return err_set(err, "out of memory");
        }
        report.total_samples += t.n;
        for (k = 0; k < t.nch; k++)
            if (peak[k] > report.peak[k]) report.peak[k] = peak[k];
        if (i == 0) {
            report.first = t;
            memcpy(report.map, map, sizeof map);
        } else {
            trial_free(&t);
        }
    }
    report.ntrials = stim.n;

    /* The rate of the stimulus has to be possible on this board too. */
    return config_check_rate(c, report.rate_p, report.rate_q, err);
}

/* Print what would happen, in words (the answer to -n, on stdout). */
static void print_plan(const Options *o, const Table *t, const Config *c,
                       const Report *rep)
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

    if (rep && rep->ntrials) {                      /* the stimulus has been read */
        units_format_rate(r, sizeof r, rep->rate_p, rep->rate_q);
        printf("  rate           %s (%s)\n", r,
               o->has_rate ? "from -r" :
               stim.rate_arg[0] ? "from the configuration; the stimulus states none"
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
        const char *how = stim.seed_source == SEED_GIVEN ? "from -s" :
            stim.seed_source == SEED_STATED ? "stated by the stimulus" :
            "drawn for this check; a run draws its own unless you give -s";
        if (stim.is_protocol)
            printf("  seed           protocol seed %s (%s)\n", stim.protocol_seed, how);
        else
            printf("  seed           %s (%s)\n", stim.seed_arg, how);
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

        if (stim.is_protocol)
            printf("  stimulus       a protocol of %d trials: %s, start %s; each trial %lld "
                   "samples (%.6g s)\n", rep->ntrials, stim.timing, stim.start,
                   (long long)f->n, (double)f->n / f->rate);
        else
            printf("  stimulus       %s with %d channel(s), %lld samples (%.6g s)\n",
                   stim.waveform ? "a plain waveform" : "a stimulus", f->nch,
                   (long long)f->n, (double)f->n / f->rate);
        for (k = 0; k < f->nch; k++) {
            const OutCh *oc = &c->out[rep->map[k].out];
            printf("  %-14s %s (%s)  ->  [out %s] on %s", k ? "" : "channels",
                   stim.waveform ? "waveform" : f->ch[k].name,
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
    if (c->device == DEV_NEURON)
        printf("\n  virtual preparation: %d cell(s), %d synapse(s) (checked later)\n",
               c->n_cells, c->n_synapses);
    printf("\nnothing was played.\n");
}

/* Where the recorded inputs go until M4 writes them to a file: counted and dropped. */
typedef struct { int64_t samples; int trials; } Counter;

static int count_begin(void *u, const TrialInfo *ti, Err *e) { (void)u; (void)ti; (void)e; return 0; }
static int count_block(void *u, int64_t first, int64_t count, const double *const x[], Err *e)
{
    (void)first; (void)x; (void)e;
    ((Counter *)u)->samples += count;
    return 0;
}
static int count_end(void *u, const TrialInfo *ti, const TrialRecord *rec, Err *e)
{
    (void)ti; (void)rec; (void)e;
    ((Counter *)u)->trials++;
    return 0;
}

/*
 * Run the trials.  Returns the exit status: 0 success, 1 an error before the
 * outputs were touched, 2 aborted or failed after.
 */
static int do_run(const Options *o, const Config *c, Err *err)
{
    Clock clock;
    Backend *be;
    Counter counter = { 0, 0 };
    Sink sink;
    EngineJob job;
    EngineReport rep;
    Timing timing;
    int status;

    if (c->device == DEV_NEURON) {
        err_set(err, "device = neuron is not implemented yet (milestone M3b); use device = sim");
        return 1;
    }
    if (c->device == DEV_DAQMX) {
        err_set(err, "device '%s': there is no hardware backend yet (milestone M6, and the machine "
                "that will have the board is not decided); use device = sim", c->device_name);
        return 1;
    }

    memset(&job, 0, sizeof job);
    if (o->has_duration) {                         /* -t: one trial with every output at rest */
        int64_t p, q, n;
        int exact;
        if (o->has_rate) { p = o->rate_p; q = o->rate_q; }
        else if (c->has_rate) { p = c->rate_p; q = c->rate_q; }
        else {
            err_set(err, "no sampling rate: give -r RATE or write rate = in [rig]");
            return 1;
        }
        if (config_check_rate(c, p, q, err) < 0) return 1;
        if (!units_ps_to_samples(o->duration_ps, p, q, &n, &exact) || n < 1) {
            err_set(err, "-t: the duration is shorter than one sample, or too long");
            return 1;
        }
        job.idle_n = n; job.idle_rate_p = p; job.idle_rate_q = q;
        memset(&timing, 0, sizeof timing);
    } else {
        job.st = &stim;
        job.opt = o;
        if (engine_parse_timing(stim.timing, stim.start, &timing, err) < 0) return 1;
    }

    {
        SimOpts so;
        memset(&so, 0, sizeof so);
        so.noise_sd = 0.002; so.seed = 1;
        so.report_state = o->verbose;              /* -v: say where the outputs were left */
        be = sim_backend_new(&so);
    }
    if (!be) { err_set(err, "out of memory"); return 1; }
    platform_install_abort_handlers();
    if (o->realtime) clock_init_real(&clock); else clock_init_virtual(&clock);

    sink.begin = count_begin; sink.block = count_block; sink.end = count_end; sink.user = &counter;
    job.cfg = c; job.backend = be; job.clock = &clock; job.sink = &sink;
    job.timing = timing; job.quiet = o->quiet;

    if (!o->quiet)
        fprintf(stderr, "lcg2: running %d trial(s) on the %s, %s\n", job.st ? stim.n : 1, be->name,
                o->realtime ? "in real time" : "in simulated time (-R follows the clock)");
    status = engine_run(&job, &rep, err);
    sim_backend_free(be);

    if (rep.rest_failed) return 2;                 /* the outputs may not be at rest */
    if (status == ENGINE_OK) {
        if (!o->quiet)
            fprintf(stderr, "lcg2: done: %d trial(s) in %.3f s%s, largest delay %.3f s; no data file was "
                    "written: recording is milestone M4\n", rep.completed, (double)rep.elapsed_ns / 1e9,
                    rep.is_virtual ? " (simulated)" : "", (double)rep.max_delay_ns / 1e9);
        return 0;
    }
    if (status == ENGINE_ABORTED) return 2;
    /* a failure: before the backend was touched it is an error of the run's setup (1) */
    return rep.touched ? 2 : 1;
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
    if (!opt.has_duration && check_stimulus(&opt, &config, &err) < 0) return fail(&err);

    if (opt.dry) {
        print_plan(&opt, &table, &config, &report);
        if (opt.verbose) {
            printf("\nresolved configuration:\n\n");
            config_dump(&table, stdout);
        }
        return 0;
    }

    {
        int status;
        err.msg[0] = 0;                            /* so that only a real message is printed */
        status = do_run(&opt, &config, &err);
        if (status != 0 && err.msg[0]) fprintf(stderr, "lcg2: %s\n", err.msg);
        return status;
    }
}
