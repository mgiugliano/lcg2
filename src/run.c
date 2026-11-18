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
 * run.c -- run the trials of a checked Session and keep the record.  See run.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "engine.h"
#include "platform.h"
#include "record.h"
#include "run.h"
#include "stimgen_api.h"
#include "units.h"

/* The StimGen version LCG2 was built against, baked in by the Makefile from STIMGEN_PIN. */
#ifndef LCG2_STIMGEN_COMMIT
#define LCG2_STIMGEN_COMMIT "unknown"
#endif
#ifndef LCG2_STIMGEN_SHA256
#define LCG2_STIMGEN_SHA256 "unknown"
#endif

void run_params_default(RunParams *rp, const Session *s)
{
    memset(rp, 0, sizeof *rp);
    rp->session = s;
    rp->sim.noise_sd = 0.002;
    rp->sim.seed = 1;
    rp->sim.report_state = s->opt.verbose;    /* -v: say where the outputs were left */
}

/* ------------------------------------------------------------------ */
/* Reading a whole file                                                */
/* ------------------------------------------------------------------ */

static char *slurp(const char *path, size_t *len, Err *err)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) { err_set(err, "cannot read '%s': %s", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); err_set(err, "cannot read '%s'", path); return NULL; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); err_set(err, "out of memory"); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

/* The resolved configuration as text (config_dump writes to a FILE). */
static char *config_text(const Table *t, Err *err)
{
    FILE *f = tmpfile();
    char *buf;
    long n;
    if (!f) { err_set(err, "cannot make a temporary file: %s", strerror(errno)); return NULL; }
    config_dump(t, f);
    fflush(f);
    n = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); err_set(err, "out of memory"); return NULL; }
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* The sink that records and logs                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    Sink       rec;                   /* the recorder's sink                            */
    Archive   *a;
    int64_t    samples;
    int        trials;
} Tee;

static int tee_begin(void *u, const TrialInfo *ti, Err *err)
{
    Tee *t = (Tee *)u;
    return t->rec.begin(t->rec.user, ti, err);
}

static int tee_block(void *u, int64_t first, int64_t count, const double *const x[], Err *err)
{
    Tee *t = (Tee *)u;
    t->samples += count;
    return t->rec.block(t->rec.user, first, count, x, err);
}

static int tee_end(void *u, const TrialInfo *ti, const TrialRecord *rec, Err *err)
{
    Tee *t = (Tee *)u;
    int rc = t->rec.end(t->rec.user, ti, rec, err);
    int k;
    t->trials++;
    archive_log(t->a, "trial %d/%d  %s%s%s  start %.3f s  delay %.3f s  utc %s  recorded %lld of %lld samples%s%s",
                ti->index + 1, ti->count, ti->name, ti->variables[0] ? "  " : "", ti->variables,
                (double)rec->start_ns / 1e9, (double)rec->delay_ns / 1e9, rec->start_utc,
                (long long)rec->recorded, (long long)ti->n, ti->is_virtual ? "  (simulated time)" : "",
                rec->incomplete ? "  INCOMPLETE" : "");
    for (k = 0; k < ti->nin; k++)
        if (rec->overrange[k])
            archive_log(t->a, "warning  trial %s: input %s: %lld samples beyond the range (clamped)", ti->name,
                        ti->ai[k].name, (long long)rec->overrange[k]);
    return rc;
}

/* ------------------------------------------------------------------ */
/* The provenance record                                               */
/* ------------------------------------------------------------------ */

static void file_entry(StrBuf *b, int first, const char *path, const char *sha, const char *how)
{
    sb_add(b, first ? "\n    {" : ",\n    {");
    sb_add(b, "\"path\": "); sb_json(b, path);
    sb_add(b, ", \"sha256\": "); sb_json(b, sha);
    sb_add(b, ", \"how\": "); sb_json(b, how);
    sb_add(b, "}");
}

static char *provenance(const RunParams *rp, const Archive *a, const char *cmd, const char *cwd,
                        const char *started_utc, int64_t rate_p, int64_t rate_q, const char *rate_source,
                        int ntrials, int virtual_time)
{
    const Session *s = rp->session;
    const Stim *st = &s->stim;
    StrBuf b;
    const char *sha;
    int k;

    memset(&b, 0, sizeof b);
    sb_add(&b, "{\n");
    sb_addf(&b, "  \"lcg2_version\": \"%s\",\n", LCG2_VERSION);
    sb_addf(&b, "  \"stimgen\": {\"pinned_commit\": \"%s\", \"pinned_sources_sha256\": \"%s\", \"library_version\": ",
            LCG2_STIMGEN_COMMIT, LCG2_STIMGEN_SHA256);
    sb_json(&b, sg_api_version());
    sb_add(&b, "},\n");
    sb_add(&b, "  \"command_line\": "); sb_json(&b, cmd); sb_add(&b, ",\n");
    sb_add(&b, "  \"working_folder\": "); sb_json(&b, cwd); sb_add(&b, ",\n");
    sb_addf(&b, "  \"started_utc\": \"%s\",\n", started_utc);
    sb_addf(&b, "  \"mode\": \"%s\",\n  \"device\": ", config_mode_name(s->config.mode));
    sb_json(&b, s->config.device_name);
    sb_add(&b, ",\n");
    sb_add(&b, "  \"profile\": "); sb_json(&b, s->table.profile[0] ? s->table.profile : ""); sb_add(&b, ",\n");
    sb_addf(&b, "  \"virtual_time\": %s,\n", virtual_time ? "true" : "false");
    sb_addf(&b, "  \"rate\": {\"hz\": %.17g, \"p\": %lld, \"q\": %lld, \"source\": ", (double)rate_p / (double)rate_q,
            (long long)rate_p, (long long)rate_q);
    sb_json(&b, rate_source);
    sb_add(&b, ", \"obtained\": \"the simulator has no clock to coerce it: the rate obtained is the rate asked\"},\n");
    if (s->opt.has_duration) {
        sb_add(&b, "  \"seed\": null,\n");
    } else if (st->is_protocol) {
        sb_addf(&b, "  \"seed\": {\"kind\": \"protocol\", \"value\": \"%s\", \"source\": \"%s\"},\n", st->protocol_seed,
                st->seed_source == SEED_GIVEN ? "-s" : st->seed_source == SEED_STATED ? "stated in the protocol" : "drawn");
    } else {
        sb_addf(&b, "  \"seed\": {\"kind\": \"stimulus\", \"value\": \"%s\", \"source\": \"%s\"},\n", st->seed_arg,
                st->seed_source == SEED_GIVEN ? "-s" : st->seed_source == SEED_STATED ? "stated in the stimulus" : "drawn");
    }
    sb_addf(&b, "  \"trials\": %d,\n", ntrials);
    sb_addf(&b, "  \"clipping\": {\"samples_clipped\": 0, \"trials_checked\": %d, \"note\": \"every sample of every trial was checked against the limits before anything was played; a run with a sample that would clip is refused\"},\n",
            s->opt.has_duration ? 0 : s->report.ntrials);
    sb_add(&b, "  \"warnings\": [");
    for (k = 0; k < st->nwarn && !s->opt.has_duration; k++) {
        sb_add(&b, k ? ",\n    {" : "\n    {");
        sb_add(&b, "\"text\": "); sb_json(&b, st->warn_text[k]);
        sb_add(&b, ", \"first_trial\": "); sb_json(&b, st->warn_first[k]);
        sb_addf(&b, ", \"trials\": %d}", st->warn_count[k]);
    }
    sb_add(&b, st->nwarn && !s->opt.has_duration ? "\n  ],\n" : "],\n");
    sb_add(&b, "  \"convert_rate\": null,\n  \"convert_rate_note\": \"the loopback simulator has no multiplexed converter, so there is no convert rate\",\n");
    sha = archive_sha(a, "config.ini");
    sb_addf(&b, "  \"config_sha256\": \"%s\",\n", sha ? sha : "");
    sha = archive_sha(a, "config_original.ini");
    sb_add(&b, "  \"config_file\": {\"path\": "); sb_json(&b, s->table.path);
    sb_addf(&b, ", \"sha256\": \"%s\"},\n", sha ? sha : "");
    sha = archive_sha(a, "stimulus.sg");
    sb_addf(&b, "  \"stimulus_sha256\": %s%s%s,\n", sha ? "\"" : "", sha ? sha : "null", sha ? "\"" : "");
    sha = archive_sha(a, "canonical.sg");
    sb_addf(&b, "  \"canonical_sha256\": %s%s%s,\n", sha ? "\"" : "", sha ? sha : "null", sha ? "\"" : "");
    /* what StimGen says file(...) read, with its digests; then every copy kept in files/ */
    sb_add(&b, "  \"files_read_by_file_function\": [");
    for (k = 0; k < st->nused && !s->opt.has_duration; k++)
        file_entry(&b, k == 0, st->used_path[k], st->used_sha[k], "listed by StimGen");
    sb_add(&b, st->nused && !s->opt.has_duration ? "\n  ],\n" : "],\n");
    sb_add(&b, "  \"files_archived\": [");
    {
        int first = 1;
        for (k = 0; k < a->nitem; k++)
            if (strstr(a->item[k].path, "/files/")) {
                file_entry(&b, first, a->item[k].path, a->item[k].sha, "copy kept in the run folder");
                first = 0;
            }
        sb_add(&b, first ? "]\n}\n" : "\n  ]\n}\n");
    }
    if (b.failed) { sb_free(&b); return NULL; }
    return b.s;
}

/* ------------------------------------------------------------------ */
/* The run                                                             */
/* ------------------------------------------------------------------ */

int run_execute(const RunParams *rp, Err *err)
{
    const Session *s = rp->session;
    const Options *o = &s->opt;
    const Config *c = &s->config;
    const Stim *st = &s->stim;
    Clock clock;
    Backend *be;
    Archive ar;
    Recorder *rec = NULL;
    Tee tee;
    Sink sink;
    EngineJob job;
    EngineReport rep;
    Timing timing;
    int status, exit_status = 0, ntrials, have_ar = 0;
    int64_t rate_p, rate_q;
    const char *rate_source;
    char *cmd = NULL, *cfgtext = NULL, *orig = NULL, *prov = NULL, cwd[512] = "", started[40], *canon = NULL;
    size_t origlen = 0;
    char cmdfile[1200];
    int64_t estimated_bytes = 0, free_bytes = -1;
    Err e2;

    if (c->device == DEV_NEURON) {
        err_set(err, "device = neuron is not implemented yet (milestone M3b); use device = sim");
        return 1;
    }
    if (c->device == DEV_DAQMX) {
        err_set(err, "device '%s': there is no hardware backend yet (milestone M6, and the machine "
                "that will have the board is not decided); use device = sim", c->device_name);
        return 1;
    }

    /* What is to be played, and its timing. */
    memset(&job, 0, sizeof job);
    memset(&timing, 0, sizeof timing);
    if (o->has_duration) {                         /* -t: one trial with every output at rest */
        int64_t n;
        int exact;
        if (o->has_rate) { rate_p = o->rate_p; rate_q = o->rate_q; rate_source = "-r"; }
        else if (c->has_rate) { rate_p = c->rate_p; rate_q = c->rate_q; rate_source = "the configuration"; }
        else {
            err_set(err, "no sampling rate: give -r RATE or write rate = in [rig]");
            return 1;
        }
        if (config_check_rate(c, rate_p, rate_q, err) < 0) return 1;
        if (!units_ps_to_samples(o->duration_ps, rate_p, rate_q, &n, &exact) || n < 1) {
            err_set(err, "-t: the duration is shorter than one sample, or too long");
            return 1;
        }
        job.idle_n = n; job.idle_rate_p = rate_p; job.idle_rate_q = rate_q;
        ntrials = 1;
    } else {
        job.st = st;
        job.opt = o;
        if (engine_parse_timing(st->timing, st->start, &timing, err) < 0) return 1;
        rate_p = s->report.rate_p; rate_q = s->report.rate_q;
        rate_source = o->has_rate ? "-r" : st->rate_arg[0] ? "the configuration" : "stated by the stimulus";
        ntrials = st->n;
    }

    /* 0. Is there room?  The size of the data is known in advance: every input, and every command
     * the stimulus names, for every sample of every trial, as 8-byte numbers, plus the structure
     * of the file.  A disk that fills in the middle of a run can leave a file that cannot be
     * opened (measured: docs/13-data-format.md), so refuse to start rather than hope. */
    {
        int act[CFG_MAX_IN], nin = 0, k, ncmd = 0;
        int64_t samples = 0;
        double need, margin;
        int64_t freeb = platform_free_bytes(".");
        for (k = 0; k < c->n_in; k++) nin += !c->in[k].skip;
        if (o->has_duration) samples = job.idle_n;
        else { samples = s->report.total_samples; ncmd = s->report.first.nch; }
        (void)act;
        need = (double)samples * (double)(nin + ncmd) * 8.0 + 131072.0 + 20480.0 * (double)ntrials;
        margin = need * 1.10 + 1048576.0;
        estimated_bytes = (int64_t)need;
        if (freeb >= 0 && (double)freeb < margin) {
            err_set(err, "not enough free disk space for the data in the current folder: this run needs about "
                    "%.1f MB and %.1f MB are free (nothing was written)", need / 1048576.0, (double)freeb / 1048576.0);
            return 1;
        }
        free_bytes = freeb;
    }

    /* 1. Everything that can fail without touching an output: the folder, its files, the data file. */
    platform_getcwd(cwd, sizeof cwd);
    platform_format_utc(platform_wall_ns(), started);
    if (archive_create(&ar, o->prefix, err) < 0) return 1;
    have_ar = 1;
    cmd = archive_quote_args(s->argc, s->argv);
    cfgtext = config_text(&s->table, err);
    orig = cfgtext ? slurp(s->table.path, &origlen, err) : NULL;
    if (!cmd || !cfgtext || !orig) { if (!err->msg[0]) err_set(err, "out of memory"); goto setup_failed; }

    snprintf(cmdfile, sizeof cmdfile, "%s\nfolder: %s\n", cmd, cwd);
    if (archive_put(&ar, "config.ini", cfgtext, strlen(cfgtext), err) < 0 ||
        archive_put(&ar, "config_original.ini", orig, origlen, err) < 0 ||
        archive_put(&ar, "command.txt", cmdfile, strlen(cmdfile), err) < 0) goto setup_failed;
    archive_log(&ar, "LCG2 %s, run %s, started %s", LCG2_VERSION, ar.stem, started);
    archive_log(&ar, "StimGen pinned at %s (sources sha256 %s), library %s", LCG2_STIMGEN_COMMIT,
                LCG2_STIMGEN_SHA256, sg_api_version());
    archive_log(&ar, "command: %s", cmd);
    archive_log(&ar, "folder: %s", cwd);
    archive_log(&ar, "device %s, mode %s, %d trial(s), %s%s", c->device_name, config_mode_name(c->mode), ntrials,
                o->realtime ? "real time" : "simulated time", o->has_duration ? ", record only" : "");
    archive_log(&ar, "disk  the data is expected to take about %lld bytes; %lld bytes free", (long long)estimated_bytes,
                (long long)free_bytes);

    if (!o->has_duration) {
        int k;
        for (k = 0; k < st->nwarn; k++) {                  /* StimGen's warnings from the check of every trial */
            if (st->warn_first[k][0])
                archive_log(&ar, "warning  %s  (%d trial%s, first in trial %s)", st->warn_text[k], st->warn_count[k],
                            st->warn_count[k] == 1 ? "" : "s", st->warn_first[k]);
            else
                archive_log(&ar, "warning  %s", st->warn_text[k]);
        }
        if (archive_put(&ar, "stimulus.sg", st->text, strlen(st->text), err) < 0) goto setup_failed;
        canon = stim_canonical(st);
        if (canon) { if (archive_put(&ar, "canonical.sg", canon, strlen(canon), err) < 0) goto setup_failed; }
        else archive_log(&ar, "note  no canonical form (a protocol has none; its trials are in trials.json)");
        if (st->is_protocol) {
            if (archive_put(&ar, "trials.json", st->protocol_json, strlen(st->protocol_json), err) < 0) goto setup_failed;
        } else {
            StrBuf tj;
            memset(&tj, 0, sizeof tj);
            sb_addf(&tj, "{\"kind\": \"%s\", \"seed\": \"%s\", \"trials\": 1}\n", st->waveform ? "waveform" : "stimulus", st->seed_arg);
            if (tj.failed || archive_put(&ar, "trials.json", tj.s, tj.n, err) < 0) { sb_free(&tj); goto setup_failed; }
            sb_free(&tj);
        }
        for (k = 0; k < st->nused; k++)               /* the files file() read, checked against StimGen's digest */
            if (archive_copy(&ar, st->used_path[k], st->used_sha[k], err) < 0) goto setup_failed;
        if (archive_collect_uses(&ar, st->text, err) < 0) goto setup_failed;
    }
    prov = provenance(rp, &ar, cmd, cwd, started, rate_p, rate_q, rate_source, ntrials, !o->realtime);
    if (!prov) { err_set(err, "out of memory"); goto setup_failed; }
    if (archive_put(&ar, "provenance.json", prov, strlen(prov), err) < 0) goto setup_failed;

    {
        RecorderInit ri;
        memset(&ri, 0, sizeof ri);
        ri.path = ar.h5; ri.cfg = c; ri.run_name = ar.stem; ri.config_text = cfgtext;
        ri.stimulus_text = o->has_duration ? NULL : st->text;
        ri.command_line = cmd; ri.provenance_json = prov; ri.lcg2_version = LCG2_VERSION;
        ri.stimgen_commit = LCG2_STIMGEN_COMMIT; ri.stimgen_sources_sha256 = LCG2_STIMGEN_SHA256;
        ri.stimgen_library_version = sg_api_version(); ri.started_utc = started;
        if (record_open(&rec, &ri, err) < 0) goto setup_failed;
    }

    /* 2. Play. */
    be = sim_backend_new(&rp->sim);
    if (!be) { err_set(err, "out of memory"); record_close(rec, &e2); goto setup_failed; }
    platform_install_abort_handlers();
    if (o->realtime) clock_init_real(&clock); else clock_init_virtual(&clock);
    memset(&tee, 0, sizeof tee);
    record_sink(rec, &tee.rec);
    tee.a = &ar;
    sink.begin = tee_begin; sink.block = tee_block; sink.end = tee_end; sink.user = &tee;
    job.cfg = c; job.backend = be; job.clock = &clock; job.sink = &sink;
    job.timing = timing; job.quiet = o->quiet;

    if (!o->quiet)
        fprintf(stderr, "lcg2: running %d trial(s) on the %s, %s; writing %s\n", ntrials, be->name,
                o->realtime ? "in real time" : "in simulated time (-R follows the clock)", ar.h5);
    status = engine_run(&job, &rep, err);
    sim_backend_free(be);

    /* 3. Whatever happened: close the data file, take its digest, write the record. */
    archive_log(&ar, "end  %s: %d of %d trial(s) completed, %d started, largest delay %.3f s, %lld input samples beyond range",
                status == ENGINE_OK ? "completed" : status == ENGINE_ABORTED ? "ABORTED" : "FAILED",
                rep.completed, ntrials, rep.started, (double)rep.max_delay_ns / 1e9, (long long)rep.overrange_total);
    if (status == ENGINE_FAILED && err->msg[0]) archive_log(&ar, "error  %s", err->msg);
    if (rep.rest_failed) archive_log(&ar, "ERROR  the outputs may NOT be at rest");
    if (record_close(rec, &e2) < 0) {
        fprintf(stderr, "lcg2: ERROR: the data file could not be closed properly: %s\n", e2.msg);
        archive_log(&ar, "ERROR  the data file could not be closed properly: %s", e2.msg);
        exit_status = 2;
    }
    rec = NULL;
    if (archive_finish(&ar, &e2) < 0) {
        fprintf(stderr, "lcg2: ERROR: the integrity record (digests) could not be written: %s\n", e2.msg);
        exit_status = 2;
    }
    have_ar = 0;

    free(cmd); free(cfgtext); free(orig); free(prov); free(canon);
    if (rep.rest_failed) return 2;                 /* the outputs may not be at rest */
    if (status == ENGINE_OK) {
        if (!o->quiet)
            fprintf(stderr, "lcg2: done: %d trial(s) in %.3f s%s, largest delay %.3f s; wrote %s and %s/\n",
                    rep.completed, (double)rep.elapsed_ns / 1e9, rep.is_virtual ? " (simulated)" : "",
                    (double)rep.max_delay_ns / 1e9, ar.h5, ar.dir);
        return exit_status;
    }
    if (!o->quiet) fprintf(stderr, "lcg2: the data so far is in %s and %s/\n", ar.h5, ar.dir);
    if (status == ENGINE_ABORTED) return 2;
    return rep.touched ? 2 : 1;                    /* a failure before the backend was touched is a setup error */

setup_failed:
    /* Nothing was touched. The run folder was made; say so, so that it is not a mystery. */
    if (have_ar && err->msg[0]) {
        size_t n = strlen(err->msg);
        snprintf(err->msg + n, sizeof err->msg - n, " (the run folder %s was already made and may be incomplete)", ar.dir);
    }
    if (have_ar) { archive_log(&ar, "setup failed: %s", err->msg); if (ar.log) { fclose(ar.log); ar.log = NULL; } }
    free(cmd); free(cfgtext); free(orig); free(prov); free(canon);
    return 1;
}
