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
 * backend_sim.c -- the built-in loopback simulator.  See backend_sim.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend_sim.h"
#include "units.h"

typedef struct {
    SimOpts  o;
    SimStats st;
} Sim;

static void event(Sim *s, char c)
{
    size_t n = strlen(s->st.events);
    if (n + 1 < sizeof s->st.events) { s->st.events[n] = c; s->st.events[n + 1] = 0; }
}

/* The noise: a splitmix64 stream, turned into Gaussian numbers by Box-Muller.
 * The stream of an input in a trial depends only on (seed, trial, input), so a
 * run can be repeated exactly. */
typedef struct { uint64_t s; } Rng;

static uint64_t next64(Rng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double uniform(Rng *r)                  /* in (0, 1) */
{
    return ((double)(next64(r) >> 11) + 0.5) / 9007199254740992.0;
}

static double gaussian(Rng *r)
{
    double u = uniform(r), v = uniform(r);
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}

static int sim_open(Backend *be, const Config *cfg, Err *err)
{
    Sim *s = (Sim *)be->impl;
    if (cfg->device != DEV_SIM)
        return err_set(err, "the loopback simulator was asked to run device '%s'", cfg->device_name);
    event(s, 'O');
    s->st.open = 1;
    return 0;
}

static void note_state(Sim *s, const Config *cfg, const Drive *d, int64_t sample)
{
    int k;
    for (k = 0; k < d->nao; k++) {
        int line = cfg->out[d->ao[k].out].index;
        s->st.ao_state[line] = drive_ao(d, k, sample);
        s->st.ao_known[line] = 1;
    }
    for (k = 0; k < d->ndig; k++) {
        int line = cfg->out[d->dig[k].out].index;
        s->st.dig_state[line] = drive_dig(d, k, sample);
        s->st.dig_known[line] = 1;
    }
}

static int sim_set_rest(Backend *be, const Config *cfg, const Drive *d, Err *err)
{
    Sim *s = (Sim *)be->impl;
    int k;
    s->st.nrest_calls++;
    if (s->o.fail_rest_from > 0 && s->st.nrest_calls >= s->o.fail_rest_from)
        return err_set(err, "the simulator was told to fail setting the rest values (call %d)", s->st.nrest_calls);
    for (k = 0; k < d->nao; k++) {
        int line = cfg->out[d->ao[k].out].index;
        s->st.ao_state[line] = d->ao_rest[k];
        s->st.ao_known[line] = 1;
    }
    for (k = 0; k < d->ndig; k++) {
        int line = cfg->out[d->dig[k].out].index;
        s->st.dig_state[line] = d->dig_rest[k];
        s->st.dig_known[line] = 1;
    }
    event(s, 'R');
    s->st.nrest++;
    return 0;
}

static int sim_wait_trigger(Backend *be, const Config *cfg, Clock *clk, Err *err)
{
    Sim *s = (Sim *)be->impl;
    int r;
    (void)clk; (void)err;
    event(s, 'T');
    s->st.ntrigger++;
    if (cfg->has_trigger) return 0;     /* the simulator has no PFI line: the edge is simulated, at once */
    fprintf(stderr, "lcg2: start on trigger: no trigger line is configured; press Enter to start\n");
    r = platform_wait_enter();
    return r == 1 ? 0 : 1;              /* end of input and an abort both end the wait */
}

static int sim_play(Backend *be, Clock *clk, const Config *cfg, const Drive *d,
                    int64_t rate_p, int64_t rate_q, int64_t block_samples,
                    BlockFn block, void *user, PlayResult *res, Err *err)
{
    Sim *s = (Sim *)be->impl;
    int act[CFG_MAX_IN], nin = backend_active_inputs(cfg, act);
    int64_t first = 0, t_start, nb = block_samples > 0 ? block_samples : 1;
    double *buf[CFG_MAX_IN];
    const double *view[CFG_MAX_IN];
    Rng rng[CFG_MAX_IN];
    int k, rc = 0;
    uint64_t trial = (uint64_t)s->st.nplay;

    memset(res, 0, sizeof *res);
    memset(buf, 0, sizeof buf);
    event(s, 'P');
    s->st.nplay++;

    for (k = 0; k < nin; k++) {
        buf[k] = (double *)malloc((size_t)nb * sizeof(double));
        if (!buf[k]) { rc = err_set(err, "out of memory"); goto done; }
        rng[k].s = s->o.seed * 0x2545F4914F6CDD1Dull + trial * 0x9E3779B97F4A7C15ull
                   + (uint64_t)(k + 1) * 0xD1B54A32D192ED03ull;
    }

    /* Starting the trial takes time on a real board; here it is a number. */
    t_start = clock_now(clk) + s->o.latency_ns;
    if (!clock_wait_until(clk, t_start)) { res->incomplete = 1; rc = 1; goto done; }
    res->latency_ns = s->o.latency_ns;

    while (first < d->n) {
        int64_t count = d->n - first < nb ? d->n - first : nb, end_ns;
        if (!units_samples_to_ns(first + count, rate_p, rate_q, &end_ns)) {
            rc = err_set(err, "the trial is too long to time"); goto done;
        }
        /* The block is measured when its last sample has been taken. */
        if (!clock_wait_until(clk, t_start + end_ns)) { res->incomplete = 1; rc = 1; break; }

        for (k = 0; k < nin; k++) {
            const InCh *in = &cfg->in[act[k]];
            int64_t i;
            int a, src = -1;
            for (a = 0; a < d->nao; a++)                       /* aoN is wired to aiN */
                if (cfg->out[d->ao[a].out].index == in->index) src = a;
            for (i = 0; i < count; i++) {
                double v = (src >= 0 ? drive_ao(d, src, first + i) : 0.0) + s->o.noise_sd * gaussian(&rng[k]);
                if (v > in->range)       { v = in->range;  res->overrange[k]++; }
                else if (v < -in->range) { v = -in->range; res->overrange[k]++; }
                buf[k][i] = v;
            }
            view[k] = buf[k];
        }
        if (block(user, first, count, view, err) < 0) { rc = -1; goto done; }
        res->recorded += count;
        first += count;
        note_state(s, cfg, d, first - 1);                      /* the outputs are where the last sample put them */
    }

    if (rc == 0) note_state(s, cfg, d, d->n + d->pad - 1);     /* the rest samples ended the buffer */
done:
    if (rc == 1) event(s, 'p');
    for (k = 0; k < CFG_MAX_IN; k++) free(buf[k]);
    return rc;
}

static void sim_close(Backend *be)
{
    Sim *s = (Sim *)be->impl;
    event(s, 'C');
    s->st.closed = 1;
    if (s->o.report_state) {
        int k;
        fprintf(stderr, "lcg2: simulator: outputs left at");
        for (k = 0; k < 4; k++) if (s->st.ao_known[k]) fprintf(stderr, " ao%d=%.6g V", k, s->st.ao_state[k]);
        for (k = 0; k < 8; k++) if (s->st.dig_known[k]) fprintf(stderr, " port0/line%d=%d", k, s->st.dig_state[k]);
        fprintf(stderr, "\n");
    }
}

Backend *sim_backend_new(const SimOpts *opts)
{
    Backend *be = (Backend *)calloc(1, sizeof *be);
    Sim *s = (Sim *)calloc(1, sizeof *s);
    if (!be || !s) { free(be); free(s); return NULL; }
    s->o.noise_sd = 0.002;
    s->o.seed = 1;
    if (opts) s->o = *opts;
    be->name = "loopback simulator";
    be->open = sim_open;
    be->set_rest = sim_set_rest;
    be->wait_trigger = sim_wait_trigger;
    be->play = sim_play;
    be->close = sim_close;
    be->impl = s;
    return be;
}

void sim_backend_free(Backend *be)
{
    if (!be) return;
    free(be->impl);
    free(be);
}

const SimStats *sim_backend_stats(const Backend *be)
{
    return &((const Sim *)be->impl)->st;
}
