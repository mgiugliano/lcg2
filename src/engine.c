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
 * engine.c -- the trial engine.  Read engine.h first for the rules and the order
 * of things; this file follows that order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "json.h"
#include "units.h"

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

int engine_parse_timing(const char *timing, const char *start, Timing *t, Err *err)
{
    int64_t ps;
    memset(t, 0, sizeof *t);                      /* gap 0s, immediately */

    if (timing && timing[0]) {
        const char *sp = strchr(timing, ' ');
        size_t kl = sp ? (size_t)(sp - timing) : strlen(timing);
        const char *val = sp ? sp + strspn(sp, " \t") : "";
        if (kl == 6 && strncmp(timing, "period", 6) == 0) t->is_period = 1;
        else if (!(kl == 3 && strncmp(timing, "gap", 3) == 0))
            return err_set(err, "unknown timing '%s': expected 'period T' or 'gap T'", timing);
        if (units_parse_duration_ex(val, &ps, 1, 1) < 0)
            return err_set(err, "cannot read the time in '%s'", timing);
        t->ns = ps / 1000;                         /* picoseconds to nanoseconds */
    }
    if (start && start[0]) {
        if (strcmp(start, "on trigger") == 0) t->on_trigger = 1;
        else if (strcmp(start, "immediately") != 0)
            return err_set(err, "the protocol's start rule is '%s'; expected 'immediately' or 'on trigger'", start);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* One prepared trial                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    Trial   trial;               /* a stimulus trial (nch == 0 for an idle one)  */
    int     has_trial;
    Drive   drive;
    int     has_drive;
    char    name[64];
    char    variables[160];
    int64_t n, rate_p, rate_q;
} Prep;

static void prep_free(Prep *p)
{
    if (p->has_drive) drive_free(&p->drive);
    if (p->has_trial) trial_free(&p->trial);
    memset(p, 0, sizeof *p);
}

/* "amp=-300pA, freq=5Hz" from the "variables" object of a protocol trial's record. */
static void variables_of(const char *info, char *out, size_t n)
{
    Json *j;
    const Json *v;
    size_t k, len = 0;
    out[0] = 0;
    if (!info) return;
    j = json_parse(info, strlen(info), &(Err){ {0} });
    if (!j) return;
    v = json_get(j, "variables");
    for (k = 0; v && v->type == J_OBJ && k < v->n; k++) {
        const char *val = json_str(v->item[k]);
        int w = snprintf(out + len, n - len, "%s%s=%s", k ? ", " : "", v->key[k], val ? val : "?");
        if (w < 0 || (size_t)w >= n - len) break;
        len += (size_t)w;
    }
    json_free(j);
}

/* Render trial i again, and refuse anything that is not exactly what was checked. */
static int prepare_stim(const EngineJob *job, int i, Prep *p, Err *err)
{
    Map map[CFG_MAX_OUT];
    Err inner;
    memset(p, 0, sizeof *p);
    if (stim_render(job->st, i, &p->trial, err) < 0) return -1;
    p->has_trial = 1;
    if (!stim_digest_matches(job->st, i, p->trial.samples_sha256)) {
        return err_set(err, "trial %s is not the one that was checked (its samples changed between the "
                       "check and the run): not played", p->trial.name);
    }
    if (stim_map(job->st, &p->trial, job->opt, job->cfg, map, &inner) < 0 ||
        stim_check(&p->trial, map, job->cfg, NULL, &inner) < 0)
        return err_set(err, "trial %s failed its check again before playing: %s", p->trial.name, inner.msg);
    if (stim_drive(&p->trial, map, job->cfg, &p->drive, err) < 0) return -1;
    p->has_drive = 1;
    snprintf(p->name, sizeof p->name, "%s", p->trial.name);
    variables_of(p->trial.info, p->variables, sizeof p->variables);
    p->n = p->trial.n;
    p->rate_p = p->trial.rate_p;
    p->rate_q = p->trial.rate_q;
    return 0;
}

static int prepare_idle(const EngineJob *job, Prep *p, Err *err)
{
    memset(p, 0, sizeof *p);
    if (stim_drive_idle(job->cfg, job->idle_n, &p->drive, err) < 0) return -1;
    p->has_drive = 1;
    snprintf(p->name, sizeof p->name, "record");
    p->n = job->idle_n;
    p->rate_p = job->idle_rate_p;
    p->rate_q = job->idle_rate_q;
    return 0;
}

static int prepare(const EngineJob *job, int i, Prep *p, Err *err)
{
    return job->st ? prepare_stim(job, i, p, err) : prepare_idle(job, p, err);
}

/* ------------------------------------------------------------------ */
/* Inputs: volts from the backend to physical units for the sink        */
/* ------------------------------------------------------------------ */

typedef struct {
    const EngineJob *job;
    const TrialInfo *ti;
    int      act[CFG_MAX_IN], nact;
    double  *phys[CFG_MAX_IN];
    int64_t  cap;
    Err     *err;
} Relay;

static int relay_block(void *user, int64_t first, int64_t count, const double *const volts[], Err *err)
{
    Relay *r = (Relay *)user;
    const Config *cfg = r->job->cfg;
    int m = MODE_INDEX(cfg->mode), k;
    int64_t i;
    const double *x[CFG_MAX_IN + 1];

    if (count > r->cap) return err_set(err, "internal error: a block is larger than announced");
    for (k = 0; k < r->nact; k++) {
        const InCh *in = &cfg->in[r->act[k]];
        for (i = 0; i < count; i++)
            r->phys[k][i] = in->gain[m] * (in->cal_scale * volts[k][i] + in->cal_offset);
        x[k] = r->phys[k];
    }
    x[r->nact] = NULL;                       /* the sink may rely on the end of the list */
    return r->job->sink->block(r->job->sink->user, first, count, x, err);
}

/* ------------------------------------------------------------------ */
/* The run                                                             */
/* ------------------------------------------------------------------ */

static void progress(const EngineJob *job, const TrialInfo *ti, const TrialRecord *rec)
{
    if (job->quiet) return;
    fprintf(stderr, "lcg2: trial %d/%d  %s%s%s  start %.3f s  delay %.3f s%s%s\n", ti->index + 1,
            ti->count, ti->name, ti->variables[0] ? "  " : "", ti->variables,
            (double)rec->start_ns / 1e9, (double)rec->delay_ns / 1e9,
            ti->is_virtual ? "  (simulated time)" : "", rec->incomplete ? "  INCOMPLETE" : "");
}

/*
 * Two slots hold the trials: trial i lives in slot[i & 1], so while trial i is
 * handled, trial i-1 is still alive in the other slot.  rest_drive always points
 * at the drive of the most recent trial that was prepared; its rest samples are
 * the state the outputs rest at, whatever stops the run.
 */
int engine_run(const EngineJob *job, EngineReport *rep, Err *err)
{
    const Config *cfg = job->cfg;
    Backend *be = job->backend;
    Clock *clk = job->clock;
    Prep slot[2];
    const Drive *rest_drive;
    int count = job->st ? job->st->n : 1;
    int64_t prev_start = 0, prev_end = 0, t0;
    int status = ENGINE_OK, i;
    Err fail = { {0} };

    memset(rep, 0, sizeof *rep);
    memset(slot, 0, sizeof slot);
    rep->is_virtual = clk->is_virtual;

    /* 1. Prepare the first trial before any output is touched. */
    if (prepare(job, 0, &slot[0], err) < 0) { prep_free(&slot[0]); return ENGINE_FAILED; }
    rest_drive = &slot[0].drive;

    /* 2. Open the backend and set every output to rest. */
    if (be->open(be, cfg, err) < 0) { prep_free(&slot[0]); return ENGINE_FAILED; }
    rep->touched = 1;
    if (be->set_rest(be, cfg, rest_drive, err) < 0) { status = ENGINE_FAILED; goto finish; }

    /* 3. The trials. */
    t0 = clock_now(clk);
    for (i = 0; i < count; i++) {
        Prep *p = &slot[i & 1];
        TrialInfo ti;
        TrialRecord rec;
        PlayResult pr;
        Relay relay;
        int64_t scheduled, call_ns, block_samples;
        int r, k;

        if (i > 0) {                                  /* (trial 0 was prepared above) */
            prep_free(p);                             /* trial i-2 is no longer needed */
            if (prepare(job, i, p, &fail) < 0) { *err = fail; status = ENGINE_FAILED; break; }
            rest_drive = &p->drive;
        }

        /* When was it meant to start? */
        if (i == 0) scheduled = t0;
        else if (job->timing.is_period) scheduled = prev_start + job->timing.ns;
        else scheduled = prev_end + job->timing.ns;

        memset(&ti, 0, sizeof ti);
        memset(&rec, 0, sizeof rec);
        memset(&pr, 0, sizeof pr);
        ti.index = i; ti.count = count; ti.name = p->name;
        ti.trial = p->has_trial ? &p->trial : NULL;
        ti.variables = p->variables;
        ti.n = p->n; ti.rate_p = p->rate_p; ti.rate_q = p->rate_q;
        ti.scheduled_ns = scheduled - t0;
        ti.is_virtual = clk->is_virtual;
        relay.nact = backend_active_inputs(cfg, relay.act);
        ti.nin = relay.nact;
        for (k = 0; k < relay.nact; k++) {
            ti.ai[k].in = relay.act[k];
            ti.ai[k].name = cfg->in[relay.act[k]].name;
            ti.ai[k].unit = cfg->in[relay.act[k]].unit[MODE_INDEX(cfg->mode)];
        }

        /* Wait for its time, and for its trigger. */
        if (!clock_wait_until(clk, scheduled)) { status = ENGINE_ABORTED; break; }
        if (job->timing.on_trigger) {
            int w = be->wait_trigger(be, cfg, clk, &fail);
            if (w < 0) { *err = fail; status = ENGINE_FAILED; break; }
            if (w == 1) { status = ENGINE_ABORTED; break; }
        }

        /* Play it, handing the inputs to the sink as they arrive. */
        block_samples = (int64_t)(((double)p->rate_p / (double)p->rate_q) * ENGINE_BLOCK_SECONDS);
        if (block_samples < 1) block_samples = 1;
        relay.job = job; relay.ti = &ti; relay.err = &fail; relay.cap = block_samples;
        for (k = 0; k < CFG_MAX_IN; k++) relay.phys[k] = NULL;
        for (k = 0; k < relay.nact; k++) {
            relay.phys[k] = (double *)malloc((size_t)block_samples * sizeof(double));
            if (!relay.phys[k]) { err_set(err, "out of memory"); status = ENGINE_FAILED; break; }
        }
        if (status != ENGINE_OK) { for (k = 0; k < relay.nact; k++) free(relay.phys[k]); break; }
        if (job->sink->begin(job->sink->user, &ti, &fail) < 0) {
            *err = fail; status = ENGINE_FAILED;
            for (k = 0; k < relay.nact; k++) free(relay.phys[k]);
            break;
        }
        rep->started++;
        call_ns = clock_now(clk);
        platform_utc_now(rec.start_utc);
        r = be->play(be, clk, cfg, &p->drive, p->rate_p, p->rate_q, block_samples,
                     relay_block, &relay, &pr, &fail);
        for (k = 0; k < relay.nact; k++) free(relay.phys[k]);

        rec.start_ns = call_ns - t0 + pr.latency_ns;
        rec.end_ns = clock_now(clk) - t0;
        rec.delay_ns = rec.start_ns - ti.scheduled_ns;
        if (rec.delay_ns < 0) rec.delay_ns = 0;
        rec.latency_ns = pr.latency_ns;
        rec.recorded = pr.recorded;
        rec.incomplete = pr.incomplete || r != 0;
        memcpy(rec.overrange, pr.overrange, sizeof rec.overrange);
        if (rec.delay_ns > rep->max_delay_ns) rep->max_delay_ns = rec.delay_ns;
        for (k = 0; k < relay.nact; k++) rep->overrange_total += pr.overrange[k];

        /* The sink hears about every trial that began, finished or not (SPEC 3.15). */
        if (job->sink->end(job->sink->user, &ti, &rec, &fail) < 0 && r >= 0) {
            *err = fail; status = ENGINE_FAILED; break;
        }
        progress(job, &ti, &rec);
        if (!job->quiet)
            for (k = 0; k < relay.nact; k++)
                if (pr.overrange[k])
                    fprintf(stderr, "lcg2: warning: trial %s: input %s: %lld samples beyond the range of "
                            "%g V (clamped)\n", p->name, cfg->in[relay.act[k]].name,
                            (long long)pr.overrange[k], cfg->in[relay.act[k]].range);

        if (r < 0) { *err = fail; status = ENGINE_FAILED; break; }
        if (r == 1) { status = ENGINE_ABORTED; break; }
        rep->completed++;
        prev_start = call_ns + pr.latency_ns;
        prev_end = clock_now(clk);

        /* Between trials the outputs hold their rest value. */
        if (be->set_rest(be, cfg, &p->drive, &fail) < 0) { *err = fail; status = ENGINE_FAILED; break; }
        if (platform_abort_requested()) { status = ENGINE_ABORTED; break; }
    }

    /* 4. Whatever happened: the outputs to rest, then close. */
finish:
    if (rep->touched) {
        Err ignore;
        be->set_rest(be, cfg, rest_drive, &ignore);
        be->close(be);
    }
    rep->elapsed_ns = clock_now(clk);
    prep_free(&slot[0]);
    prep_free(&slot[1]);
    if (status == ENGINE_ABORTED && !job->quiet)
        fprintf(stderr, "lcg2: aborted after %d of %d trial(s): outputs at rest\n", rep->completed, count);
    return status;
}
