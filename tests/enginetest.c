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
 * enginetest.c -- a test driver for the trial engine (milestone M3).
 *
 *     enginetest [driver options] -- [lcg2 options] [stimulus]
 *
 * Runs the real engine on the loopback simulator, on the VIRTUAL clock (so every
 * time is exact), and prints what happened in a form a test can read:
 *
 *   T index name scheduled start end delay latency recorded incomplete    (ns)
 *   I index input n min max mean overrange fnv                            (physical units)
 *   EVENTS  the simulator's operations in order: O open, R rest, T trigger,
 *           P play, p play stopped early, C close
 *   STATE   what every output was last driven to
 *   STATUS  how the run ended
 *
 * Driver options:
 *   --latency NS      the simulator takes NS nanoseconds to start a trial
 *   --noise SD        volts of noise on every input (default 0.002)
 *   --seed N          the noise seed
 *   --abort-after K   request an abort when trial K has just finished
 *   --abort-block K:B request an abort while block B of trial K is arriving
 *   --tamper K        remember a wrong digest for trial K (it must be refused)
 *   --fail-block K    make the sink fail on the first block of trial K
 *   --abort-now       request an abort before the run starts
 *
 * Not part of lcg2; built by `make tools`.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend_sim.h"
#include "cli.h"
#include "config.h"
#include "engine.h"
#include "platform.h"
#include "stim.h"
#include "units.h"

static Table  table;
static Config config;
static Stim   stim;

typedef struct {
    int      abort_after, abort_k, abort_b, fail_k;
    int      block_no;
    double   mn[CFG_MAX_IN], mx[CFG_MAX_IN], sum[CFG_MAX_IN];
    uint64_t fnv[CFG_MAX_IN];
    int64_t  n[CFG_MAX_IN];
    int      cur, nin;
} Probe;

static int p_begin(void *u, const TrialInfo *ti, Err *e)
{
    Probe *p = (Probe *)u;
    int k;
    (void)e;
    p->block_no = 0;
    p->cur = ti->index;
    p->nin = ti->nin;
    for (k = 0; k < CFG_MAX_IN; k++) {
        p->mn[k] = INFINITY; p->mx[k] = -INFINITY; p->sum[k] = 0;
        p->fnv[k] = 1469598103934665603ull; p->n[k] = 0;
    }
    return 0;
}

static int p_block(void *u, int64_t first, int64_t count, const double *const x[], Err *e)
{
    Probe *p = (Probe *)u;
    int k;
    int64_t i;
    (void)first;
    if (p->fail_k == p->cur) return err_set(e, "the sink was told to fail");
    for (k = 0; k < p->nin; k++) {
        if (!x[k]) return err_set(e, "the engine gave the sink too few input arrays");
        for (i = 0; i < count; i++) {
            unsigned char b[8];
            int j;
            double v = x[k][i];
            memcpy(b, &v, 8);
            for (j = 0; j < 8; j++) p->fnv[k] = (p->fnv[k] ^ b[j]) * 1099511628211ull;
            if (v < p->mn[k]) p->mn[k] = v;
            if (v > p->mx[k]) p->mx[k] = v;
            p->sum[k] += v;
        }
        p->n[k] += count;
    }
    if (x[p->nin] != NULL) return err_set(e, "the list of input arrays is not ended by NULL");
    if (p->abort_k == p->cur && p->abort_b == p->block_no) platform_request_abort();
    p->block_no++;
    return 0;
}

static int p_end(void *u, const TrialInfo *ti, const TrialRecord *r, Err *e)
{
    Probe *p = (Probe *)u;
    int k;
    (void)e;
    printf("T %d %s %lld %lld %lld %lld %lld %lld %d\n", ti->index, ti->name,
           (long long)ti->scheduled_ns, (long long)r->start_ns, (long long)r->end_ns,
           (long long)r->delay_ns, (long long)r->latency_ns, (long long)r->recorded, r->incomplete);
    for (k = 0; k < ti->nin; k++)
        printf("I %d %s %lld %.17g %.17g %.17g %lld %llx\n", ti->index, ti->ai[k].name,
               (long long)p->n[k], p->n[k] ? p->mn[k] : 0.0, p->n[k] ? p->mx[k] : 0.0,
               p->n[k] ? p->sum[k] / (double)p->n[k] : 0.0, (long long)r->overrange[k],
               (unsigned long long)p->fnv[k]);
    if (p->abort_after == ti->index) platform_request_abort();
    return 0;
}

int main(int argc, char **argv)
{
    SimOpts so = { 0, 0.002, 1 };
    Probe probe;
    Options opt;
    Err err;
    Clock clk;
    Sink sink;
    EngineJob job;
    EngineReport rep;
    Timing timing;
    Backend *be;
    char *text;
    const char *source;
    int tamper = -1, real = 0, abort_now = 0, status, i, k, dd = 0;

    memset(&probe, 0, sizeof probe);
    err.msg[0] = 0;                                   /* an abort leaves it unset */
    probe.abort_after = probe.abort_k = probe.abort_b = probe.fail_k = -1;
    for (dd = 1; dd < argc && strcmp(argv[dd], "--") != 0; dd++) {
        if (!strcmp(argv[dd], "--latency") && dd + 1 < argc) so.latency_ns = atoll(argv[++dd]);
        else if (!strcmp(argv[dd], "--noise") && dd + 1 < argc) so.noise_sd = atof(argv[++dd]);
        else if (!strcmp(argv[dd], "--seed") && dd + 1 < argc) so.seed = (uint64_t)atoll(argv[++dd]);
        else if (!strcmp(argv[dd], "--abort-after") && dd + 1 < argc) probe.abort_after = atoi(argv[++dd]);
        else if (!strcmp(argv[dd], "--abort-block") && dd + 1 < argc)
            sscanf(argv[++dd], "%d:%d", &probe.abort_k, &probe.abort_b);
        else if (!strcmp(argv[dd], "--tamper") && dd + 1 < argc) tamper = atoi(argv[++dd]);
        else if (!strcmp(argv[dd], "--fail-block") && dd + 1 < argc) probe.fail_k = atoi(argv[++dd]);
        else if (!strcmp(argv[dd], "--real")) real = 1;
        else if (!strcmp(argv[dd], "--abort-now")) abort_now = 1;
        else { fprintf(stderr, "enginetest: unknown driver option %s\n", argv[dd]); return 2; }
    }
    if (dd >= argc) { fprintf(stderr, "usage: enginetest [options] -- [lcg2 options]\n"); return 2; }

    if (cli_parse(argc - dd, argv + dd, &opt, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }
    if (config_load(&table, opt.config ? opt.config : "lcg2.ini", &err) < 0 ||
        (opt.profile && config_apply_profile(&table, opt.profile, &err) < 0) ||
        config_build(&table, opt.mode, &config, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }

    memset(&job, 0, sizeof job);
    memset(&timing, 0, sizeof timing);
    if (opt.has_duration) {
        int64_t p, q, n;
        int exact;
        if (opt.has_rate) { p = opt.rate_p; q = opt.rate_q; } else { p = config.rate_p; q = config.rate_q; }
        if (!units_ps_to_samples(opt.duration_ps, p, q, &n, &exact)) return 1;
        job.idle_n = n; job.idle_rate_p = p; job.idle_rate_q = q;
    } else {
        if (opt.text) { text = (char *)malloc(strlen(opt.text) + 1); strcpy(text, opt.text); source = "the -e text"; }
        else { source = opt.stimulus; if (stim_read_file(opt.stimulus, &text, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; } }
        if (stim_open(&stim, text, source, &opt, &config, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }
        for (i = 0; i < stim.n; i++) {                    /* the check pass */
            Trial t;
            if (stim_render(&stim, i, &t, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }
            stim_remember_digest(&stim, i, i == tamper ? "0000" : t.samples_sha256);
            trial_free(&t);
        }
        if (engine_parse_timing(stim.timing, stim.start, &timing, &err) < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }
        job.st = &stim; job.opt = &opt;
    }

    be = sim_backend_new(&so);
    if (real) clock_init_real(&clk); else clock_init_virtual(&clk);
    sink.begin = p_begin; sink.block = p_block; sink.end = p_end; sink.user = &probe;
    job.cfg = &config; job.backend = be; job.clock = &clk; job.sink = &sink;
    job.timing = timing; job.quiet = 1;

    if (abort_now) platform_request_abort();
    status = engine_run(&job, &rep, &err);

    {
        const SimStats *st = sim_backend_stats(be);
        printf("EVENTS %s\n", st->events);
        printf("STATE");
        for (k = 0; k < 4; k++) if (st->ao_known[k]) printf(" ao%d=%.17g", k, st->ao_state[k]);
        for (k = 0; k < 8; k++) if (st->dig_known[k]) printf(" line%d=%d", k, st->dig_state[k]);
        printf("\n");
    }
    printf("STATUS %d touched=%d started=%d completed=%d max_delay=%lld elapsed=%lld\n", status, rep.touched,
           rep.started, rep.completed, (long long)rep.max_delay_ns, (long long)rep.elapsed_ns);
    if (status != ENGINE_OK && err.msg[0]) printf("ERROR %s\n", err.msg);
    sim_backend_free(be);
    return 0;
}
