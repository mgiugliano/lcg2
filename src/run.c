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
#include "backend_neuron.h"
#include "engine.h"
#include "json.h"
#include "mtest.h"
#include "platform.h"
#include "record.h"
#include "run.h"
#include "sha256.h"
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
    Recorder  *r;
    Archive   *a;
    const Config *c;
    int        quiet;
    int64_t    samples;
    int        trials;
    /* the membrane test (membrane_test = yes in an [in]): per active input, the trial's current and command */
    double    *mt_cur[CFG_MAX_IN], *mt_cmd[CFG_MAX_IN];
    int        mt_j[CFG_MAX_IN];      /* the command (TrialInfo.cmd) of each tested input */
    const TrialInfo *ti;              /* the trial in hand, between begin and end        */
    int64_t    mt_n;
    int        mt_said;               /* the note that it needs voltage clamp was given */
    /* live mode (-L): keys act on the pipette; the readout is one line that is rewritten */
    int        live;
    Backend   *be;
    Clock     *clk;
    int64_t    t0;                    /* the clock when the first trial began            */
    int        quit;                  /* q was pressed: the run ends normally            */
    int        checked;               /* the backend has a pipette to act on             */
    char       line[400];             /* the last readout                                */
    char       cell[48];
    char       seed[40], start[40];   /* the session's patch_seed and patch_start        */
    StrBuf     replay;                /* the events, as the keys of a scripted timeline  */
    char       contact[40], suction[40];
    StrBuf     breaks, zaps, holds;
    double     hold;
} Tee;

/* Live mode: the readout line is rewritten in place; anything else is printed above it. */
static void live_show(Tee *t)
{
    fprintf(stderr, "\r\033[K%s", t->line);
    fflush(stderr);
}

static void live_say(Tee *t, const char *text)
{
    fprintf(stderr, "\r\033[K%s\n", text);
    live_show(t);
}

static void sb_list(StrBuf *b, const char *item)
{
    sb_addf(b, "%s%s", b->n ? ", " : "", item);
}

/* The keys pressed since the last call: each is an event for the backend, said above the readout and kept
 * for the replay. */
static int live_keys(Tee *t, Err *err)
{
    int c;
    while ((c = platform_key()) >= 0) {
        const char *what = c == 'c' ? "contact" : c == 's' ? "suction" : c == 'b' ? "break_in" : c == 'z' ? "zap"
                         : c == 'h' ? "hold" : NULL;
        char said[200], stage[40], cell[48], text[400], when[40];
        double t_ms = 0;
        int r;
        if (c == 'q') {
            t->quit = 1;
            platform_request_abort();
            live_say(t, "lcg2: q: stopping (the trial in progress is kept, marked incomplete)");
            return 0;
        }
        if (!what) {
            if (c != '\n' && c != '\r' && c != ' ')
                live_say(t, "lcg2: keys: c touch the cell, s suction, b suction pulse (break in), z zap, h holding 0/-70 mV, q stop");
            continue;
        }
        t_ms = (double)(clock_now(t->clk) - t->t0) / 1e6;
        r = t->be->event(t->be, what, said, sizeof said, &t_ms, stage, sizeof stage, cell, sizeof cell, err);
        if (r < 0) return -1;
        snprintf(when, sizeof when, "%.6fms", t_ms);
        snprintf(text, sizeof text, "lcg2: %.2f s  %s%s%s", t_ms / 1000.0, r ? said : "nothing: ", r ? "" : said, "");
        live_say(t, text);
        archive_log(t->a, "event  %s  %s  %s%s", when, what, r ? "" : "nothing: ", said);
        if (!r) continue;
        snprintf(t->cell, sizeof t->cell, "%s", cell);
        if (!strcmp(what, "contact")) snprintf(t->contact, sizeof t->contact, "%s", when);
        else if (!strcmp(what, "suction")) snprintf(t->suction, sizeof t->suction, "%s", when);
        else if (!strcmp(what, "break_in")) sb_list(&t->breaks, when);
        else if (!strcmp(what, "zap")) sb_list(&t->zaps, when);
        else {
            char item[80];
            t->hold = t->hold == 0.0 ? -70.0 : 0.0;
            snprintf(item, sizeof item, "%s %g", when, t->hold);
            sb_list(&t->holds, item);
        }
    }
    return 0;
}

/* The replay: a profile that gives a scripted timeline the events of this session, at the same times. */
static void live_replay(Tee *t, Archive *ar)
{
    StrBuf b;
    Err e;
    if (!t->cell[0]) return;
    memset(&b, 0, sizeof b);
    sb_addf(&b, "; The events of the live session %s, as a scripted timeline. To play them again, copy this section into\n"
            "; the configuration file and run without -L, with -P live_replay (the times are those of the session: the trials\n"
            "; of a replay start on time, so a trial that started late in the session lines up a little differently). Use the\n"
            "; same configuration file: this section replaces the timeline, the seed and the start, and nothing else.\n"
            "[profile live_replay]\n", ar->stem);
    if (t->seed[0]) sb_addf(&b, "cell.%s.patch_seed     = %s\ncell.%s.patch_start    = %s\n", t->cell, t->seed, t->cell, t->start);
    /* every timeline key, empty when the session did not use it: the profile replaces whatever the file says */
    sb_addf(&b, "cell.%s.patch_contact  = %s\n", t->cell, t->contact);
    sb_addf(&b, "cell.%s.patch_suction  = %s\n", t->cell, t->suction);
    sb_addf(&b, "cell.%s.patch_break_in = %s\n", t->cell, t->breaks.n ? t->breaks.s : "");
    sb_addf(&b, "cell.%s.patch_zap      = %s\n", t->cell, t->zaps.n ? t->zaps.s : "");
    sb_addf(&b, "cell.%s.patch_holding  = %s\n", t->cell, t->holds.n ? t->holds.s : "");
    if (!b.failed && archive_put(ar, "live_replay.ini", b.s, b.n, &e) < 0)
        archive_log(ar, "warning  live_replay.ini could not be kept: %s", e.msg);
    sb_free(&b);
}

static void mt_free(Tee *t)
{
    int k;
    for (k = 0; k < CFG_MAX_IN; k++) {
        free(t->mt_cur[k]); free(t->mt_cmd[k]);
        t->mt_cur[k] = t->mt_cmd[k] = NULL;
    }
}

static int tee_begin(void *u, const TrialInfo *ti, Err *err)
{
    Tee *t = (Tee *)u;
    int k, j;
    mt_free(t);
    if (t->live && !t->checked) {
        char said[200], stage[40], cell[48];
        double t_ms;
        if (!t->be->event) { err_set(err, "-L (live patching) needs device = neuron and a patching pipette (pipette_resistance in a [cell])"); return -1; }
        t_ms = 0;
        if (t->be->event(t->be, "query", said, sizeof said, &t_ms, stage, sizeof stage, cell, sizeof cell, err) < 0) return -1;
        t->checked = 1;
        snprintf(t->cell, sizeof t->cell, "%s", cell);
        if (sscanf(said, "seed %39s start %39s", t->seed, t->start) != 2) t->seed[0] = t->start[0] = 0;
        t->t0 = clock_now(t->clk);                       /* the cells' time 0 is the start of this trial */
        snprintf(t->line, sizeof t->line, "lcg2: %s  (%s)  waiting for the first test pulse ...", cell, stage);
        live_say(t, "lcg2: live: c touch the cell, s suction, b suction pulse (break in), z zap, h holding 0/-70 mV, q stop");
    }
    if (t->live && live_keys(t, err) < 0) return -1;
    t->mt_n = ti->n;
    t->ti = ti;
    for (k = 0; k < ti->nin; k++) {
        const InCh *in = &t->c->in[ti->ai[k].in];
        if (!in->mtest) continue;
        for (j = 0; j < ti->ncmd; j++)
            if (strcmp(ti->cmd[j].out_name, in->name) == 0) break;
        if (t->c->mode != MODE_VC || j == ti->ncmd || strcmp(ti->ai[k].unit, "pA") != 0 || strcmp(ti->cmd[j].unit, "mV") != 0) {
            if (!t->mt_said) {
                archive_log(t->a, "note  membrane test of %s: it needs voltage clamp, a command in mV on [out %s] from the "
                            "stimulus and a current in pA on [in %s]; not done", in->name, in->name, in->name);
                if (!t->quiet)
                    fprintf(stderr, "lcg2: %s: no membrane test: it needs voltage clamp (-m vc), a command in mV on [out %s] "
                            "and a current in pA on [in %s]\n", in->name, in->name, in->name);
                t->mt_said = 1;
            }
            continue;
        }
        t->mt_cur[k] = (double *)calloc((size_t)ti->n, sizeof(double));
        t->mt_cmd[k] = (double *)calloc((size_t)ti->n, sizeof(double));      /* filled block by block, as played */
        if (!t->mt_cur[k] || !t->mt_cmd[k]) { mt_free(t); err_set(err, "out of memory (membrane test)"); return -1; }
        t->mt_j[k] = j;
    }
    return t->rec.begin(t->rec.user, ti, err);
}

static int tee_block(void *u, int64_t first, int64_t count, const double *const x[], Err *err)
{
    Tee *t = (Tee *)u;
    int k;
    if (t->live && live_keys(t, err) < 0) return -1;
    t->samples += count;
    for (k = 0; x[k]; k++)
        if (t->mt_cur[k] && first + count <= t->mt_n) {
            const double *c = t->ti->cmd[t->mt_j[k]].x + first;
            double sc = t->ti->cmd[t->mt_j[k]].scale;
            int64_t i;
            memcpy(t->mt_cur[k] + first, x[k], (size_t)count * sizeof(double));
            for (i = 0; i < count; i++) t->mt_cmd[k][first + i] = c[i] * sc;
        }
    return t->rec.block(t->rec.user, first, count, x, err);
}

/* The membrane test of a trial: printed, logged, and kept with the input's dataset. */
static int mt_report(Tee *t, const TrialInfo *ti, const TrialRecord *rec, Err *err)
{
    int k;
    for (k = 0; k < ti->nin; k++) {
        MTest m;
        char r[40], line[400];
        if (!t->mt_cur[k]) continue;
        if (mtest_analyse(t->mt_cmd[k], t->mt_cur[k], rec->recorded, (double)ti->rate_p / (double)ti->rate_q, &m) < 0) {
            archive_log(t->a, "membrane test  trial %s  %s: %s", ti->name, ti->ai[k].name, m.why);
            continue;
        }
        mtest_format_r(m.r_mohm, r, sizeof r);
        if (m.cell)
            snprintf(line, sizeof line, "%s  R %s: Ra %.2f MOhm, Rm %.1f MOhm, Cm %.2f pF (tau %.0f us), I hold %.1f pA",
                     ti->ai[k].name, r, m.ra_mohm, m.rm_mohm, m.cm_pf, m.tau_ms * 1000.0, m.ihold_pa);
        else
            snprintf(line, sizeof line, "%s  R %s, I hold %.1f pA", ti->ai[k].name, r, m.ihold_pa);
        archive_log(t->a, "membrane test  trial %s  %s%s%s%s", ti->name, line, m.cell ? "" : "  (", m.cell ? "" : m.why,
                    m.cell ? "" : ")");
        if (t->live) { snprintf(t->line, sizeof t->line, "lcg2: %s  [holding %g mV]", line, t->hold); live_show(t); }
        else if (!t->quiet) fprintf(stderr, "lcg2: trial %s  %s\n", ti->name, line);
        if (record_ai_attr(t->r, k, "membrane_test_dv_mV", m.dv_mv, err) < 0 ||
            record_ai_attr(t->r, k, "membrane_test_R_MOhm", m.r_mohm, err) < 0 ||
            record_ai_attr(t->r, k, "membrane_test_Ihold_pA", m.ihold_pa, err) < 0)
            return -1;
        if (m.cell && (record_ai_attr(t->r, k, "membrane_test_Ra_MOhm", m.ra_mohm, err) < 0 ||
                       record_ai_attr(t->r, k, "membrane_test_Rm_MOhm", m.rm_mohm, err) < 0 ||
                       record_ai_attr(t->r, k, "membrane_test_Cm_pF", m.cm_pf, err) < 0 ||
                       record_ai_attr(t->r, k, "membrane_test_tau_ms", m.tau_ms, err) < 0))
            return -1;
    }
    return 0;
}

static int tee_end(void *u, const TrialInfo *ti, const TrialRecord *rec, Err *err)
{
    Tee *t = (Tee *)u;
    int rc = mt_report(t, ti, rec, err);
    int k;
    mt_free(t);
    if (rc == 0) rc = t->rec.end(t->rec.user, ti, rec, err);
    else { Err e2; t->rec.end(t->rec.user, ti, rec, &e2); }
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
/* What the NEURON adapter did, for the run folder                     */
/* ------------------------------------------------------------------ */

/* After a run on device = neuron: keep the description of the network that was built (network.json),
 * copies of the model files that were read, and the adapter's own messages (neuron.log, which the
 * adapter has finished writing).  Failures are logged, not fatal: the data are what matters. */
static void neuron_archive(Archive *ar, const Backend *be, const Config *c, const char *run_error)
{
    const char *desc = neuron_backend_description(be);
    Err e;
    FILE *f;
    char logpath[300];

    (void)run_error;
    if (desc) {
        Json *j = json_parse(desc, strlen(desc), &e);
        if (archive_put(ar, "network.json", desc, strlen(desc), &e) < 0)
            archive_log(ar, "warning  network.json could not be kept: %s", e.msg);
        if (j) {
            const Json *files = json_get(j, "files");
            size_t k;
            for (k = 0; files && k < files->n; k++) {
                const char *p = json_str(json_at(files, k));
                if (p && archive_copy(ar, p, NULL, &e) < 0)
                    archive_log(ar, "warning  the model file %s could not be copied: %s", p, e.msg);
            }
            const Json *notes = json_get(j, "notes");
            archive_log(ar, "neuron  NEURON %s, Python %s, %d cell(s), %d synapse(s), adapter %s", json_str_or(j, "neuron", "?"),
                        json_str_or(j, "python", "?"), c->n_cells, c->n_synapses, "lcg2_neuron.py");
            for (k = 0; notes && k < notes->n; k++)
                if (json_str(json_at(notes, k))) archive_log(ar, "note  %s", json_str(json_at(notes, k)));
            json_free(j);
        }
    }
    snprintf(logpath, sizeof logpath, "%s/neuron.log", ar->dir);
    f = fopen(logpath, "rb");
    if (f) {
        char *buf = NULL;
        size_t n = 0, cap = 0, got;
        char chunk[4096];
        while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
            if (n + got + 1 > cap) { cap = (n + got + 1) * 2; buf = (char *)realloc(buf, cap); if (!buf) break; }
            memcpy(buf + n, chunk, got);
            n += got;
        }
        fclose(f);
        if (archive_put(ar, "neuron.log", buf ? buf : "", n, &e) < 0)        /* an empty log is kept too */
            archive_log(ar, "warning  neuron.log could not be kept: %s", e.msg);
        free(buf);
    }
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
    const char *sim_name = s->config.device == DEV_NEURON ? "NEURON simulator" : "loopback simulator";
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
    if (s->config.device == DEV_NEURON) {
        char script[1200], hex[65];
        Err e3;
        neuron_script_path(&s->config, script, sizeof script);
        if (lcg_sha256_file(script, hex, NULL, &e3) < 0) strcpy(hex, "unreadable");
        sb_add(&b, "  \"neuron\": {\"sim_python\": "); sb_json(&b, neuron_python(&s->config));
        sb_add(&b, ", \"adapter\": "); sb_json(&b, script);
        sb_add(&b, ", \"adapter_sha256\": "); sb_json(&b, hex);
        sb_addf(&b, ", \"dt_ms\": %.9g, \"celsius\": %.9g, \"v_init\": %.9g, \"settle_ms\": %.9g, "
                "\"network\": \"network.json in the run folder\"},\n", s->config.sim_dt_ms, s->config.sim_celsius,
                s->config.sim_v_init, s->config.sim_settle_ms);
    }
    sb_addf(&b, "  \"rate\": {\"hz\": %.17g, \"p\": %lld, \"q\": %lld, \"source\": ", (double)rate_p / (double)rate_q,
            (long long)rate_p, (long long)rate_q);
    sb_json(&b, rate_source);
    sb_addf(&b, ", \"obtained\": \"the %s has no clock to coerce it: the rate obtained is the rate asked\"},\n", sim_name);
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
    sb_addf(&b, "  \"convert_rate\": null,\n  \"convert_rate_note\": \"the %s has no multiplexed converter, so there is no convert rate\",\n", sim_name);
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

    if (o->live) {                                   /* before anything is written: is there a terminal to read keys from? */
        if (c->mode != MODE_VC) {
            err_set(err, "-L (live patching) runs the voltage-clamp seal test: give -m vc");
            return 1;
        }
        if (!platform_term_live(1)) {
            err_set(err, "-L (live patching) needs a terminal: the keys are read from standard input");
            return 1;
        }
        platform_term_live(0);
    }
    if (c->device == DEV_DAQMX) {
        err_set(err, "device '%s': there is no hardware backend yet (milestone M6, and the machine "
                "that will have the board is not decided); use device = sim", c->device_name);
        return 1;
    }

    /* What is to be played, and its timing. */
    memset(&job, 0, sizeof job);
    memset(&timing, 0, sizeof timing);
    if (o->has_duration) {                         /* -t: one trial with every output at rest (checked by session.c) */
        rate_p = s->report.rate_p; rate_q = s->report.rate_q;
        rate_source = o->has_rate ? "-r" : "the configuration";
        job.idle_n = s->report.total_samples; job.idle_rate_p = rate_p; job.idle_rate_q = rate_q;
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
        double need, margin, slack;
        int64_t freeb = platform_free_bytes(".");
        for (k = 0; k < c->n_in; k++) nin += !c->in[k].skip;
        if (o->has_duration) samples = job.idle_n;
        else { samples = s->report.total_samples; ncmd = s->report.first.nch; }
        (void)act;
        slack = (double)RECORD_CHUNK_SAMPLES * (double)ntrials;     /* the last chunk of each dataset, stored whole */
        if (slack > (double)samples) slack = (double)samples;           /* (a chunk is never longer than its trial) */
        need = ((double)samples + slack) * (double)(nin + ncmd) * 8.0 + 131072.0 + 20480.0 * (double)ntrials;
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
    if (c->device == DEV_NEURON) {
        NeuronOpts no;
        char script[1200], logpath[300];
        memset(&no, 0, sizeof no);
        neuron_script_path(c, script, sizeof script);
        snprintf(logpath, sizeof logpath, "%s/neuron.log", ar.dir);
        no.python = neuron_python(c); no.script = script; no.stderr_path = logpath; no.quiet = o->quiet;
        be = neuron_backend_new(&no);                   /* copies the strings */
    } else {
        be = sim_backend_new(&rp->sim);
    }
    if (!be) { err_set(err, "out of memory"); record_close(rec, &e2); goto setup_failed; }
    if (o->realtime) clock_init_real(&clock); else clock_init_virtual(&clock);
    memset(&tee, 0, sizeof tee);
    record_sink(rec, &tee.rec);
    tee.a = &ar; tee.r = rec; tee.c = c; tee.quiet = o->quiet; tee.live = o->live; tee.be = be; tee.clk = &clock;
    if (o->live) platform_term_live(1);              /* checked at the start: it is a terminal */
    sink.begin = tee_begin; sink.block = tee_block; sink.end = tee_end; sink.user = &tee;
    job.cfg = c; job.backend = be; job.clock = &clock; job.sink = &sink;
    job.timing = timing; job.quiet = o->quiet || o->live;     /* live: the readout line replaces the progress lines */

    if (!o->quiet)
        fprintf(stderr, "lcg2: running %d trial(s) on the %s, %s; writing %s\n", ntrials, be->name,
                o->realtime ? "in real time" : "in simulated time (-R follows the clock)", ar.h5);
    status = engine_run(&job, &rep, err);
    mt_free(&tee);
    if (o->live) {
        platform_term_live(0);
        fprintf(stderr, "\n");
        if (tee.quit && status == ENGINE_ABORTED) {         /* q is the normal end of a live session */
            status = ENGINE_OK;
            platform_clear_abort();
            archive_log(&ar, "note  stopped with q after %d trial(s)", rep.completed);
        }
        live_replay(&tee, &ar);
        sb_free(&tee.breaks); sb_free(&tee.zaps); sb_free(&tee.holds);
    }
    if (c->device == DEV_NEURON) neuron_archive(&ar, be, c, err->msg);
    if (c->device == DEV_NEURON) neuron_backend_free(be); else sim_backend_free(be);

    /* 3. Whatever happened: close the data file, take its digest, write the record. */
    archive_log(&ar, "end  %s: %d of %d trial(s) completed, %d started, largest delay %.3f s, %lld input samples beyond range",
                status == ENGINE_OK ? "completed" : status == ENGINE_ABORTED ? "ABORTED" : "FAILED",
                rep.completed, ntrials, rep.started, (double)rep.max_delay_ns / 1e9, (long long)rep.overrange_total);
    if (status == ENGINE_FAILED && err->msg[0]) archive_log(&ar, "error  %s", err->msg);
    if (rep.rest_failed) archive_log(&ar, "ERROR  the outputs may NOT be at rest");
    if (record_flushes_skipped(rec))
        archive_log(&ar, "note  %d flush(es) of the data file in the middle of a trial were left out: the disk was nearly "
                    "full (the file was flushed at the end of each trial)", record_flushes_skipped(rec));
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
