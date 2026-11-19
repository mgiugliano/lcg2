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
 * backend_neuron.c -- the NEURON simulator as a backend.  See backend_neuron.h.
 *
 * The conversation with the adapter (neuron/lcg2_neuron.py) is described at the top of that file: a JSON
 * line per message, followed by raw 8-byte floats for the signals.  Things that matter here:
 *
 *   - The adapter's standard error goes to a file in the run folder (so it is archived and cannot fill
 *     a pipe and stall the conversation); when the adapter dies, the end of that file is the explanation
 *     and is quoted in the error message.
 *   - SIGPIPE is ignored, so that a dead adapter is an error that this program reports and survives (the
 *     outputs are still put at rest and the data file still closed), not a death of the whole program.
 *   - Every wait is a poll with a timeout that also looks at the abort flag, so Ctrl-C is heard within
 *     a tenth of a second even while NEURON is busy.  The adapter has its own process group and never sees
 *     the Ctrl-C itself.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "backend_neuron.h"
#include "json.h"
#include "platform.h"
#include "units.h"

/* The NEURON adapter that ships with LCG2, as the Makefile found it. */
#ifndef LCG2_NEURON_ADAPTER
#define LCG2_NEURON_ADAPTER "neuron/lcg2_neuron.py"
#endif

#define MAXCH 16
#define LINE_MAX_BYTES 65536

typedef struct {
    int    idx;                  /* index into cfg->out / cfg->in               */
    int    current;              /* 1: pA, 0: mV (the dimension of the channel) */
    double scale;                /* channel unit -> SI                          */
} Chan;

typedef struct {
    NeuronOpts o;
    char       python[400], script[400], errpath[400];
    long       pid;
    int        to, from;
    int        up;               /* the adapter is running and answering        */
    int        broken;           /* the conversation lost step (abort mid-block): no more trials */
    char      *desc;             /* the init reply, one line of JSON            */
    char      *lastline;         /* the most recent reply header                */
    Chan       out[MAXCH], in[MAXCH];
    int        nout, nin;
    double     rest_cmd[MAXCH];  /* pA or mV, per out[]                          */
    int        have_last;
    int64_t    last_end_ns;
} Nrn;

void neuron_script_path(const Config *c, char *out, size_t n)
{
    if (!c->sim_script[0]) snprintf(out, n, "%s", LCG2_NEURON_ADAPTER);
    else if (c->sim_script[0] == '/') snprintf(out, n, "%s", c->sim_script);
    else snprintf(out, n, "%s/%s", c->cfg_dir, c->sim_script);
}

const char *neuron_python(const Config *c)
{
    const char *e = getenv("LCG2_SIM_PYTHON");
    return e && *e ? e : c->sim_python;
}

/* ------------------------------------------------------------------ */
/* Units: the amplifier, inverted                                      */
/* ------------------------------------------------------------------ */

/* pA or mV that the amplifier would produce for these volts at the board */
static double volts_to_cmd(const OutCh *o, int m, const Chan *c, double v)
{
    double phys = o->gain[m] * (o->cal_scale * v + o->cal_offset);       /* channel unit */
    return phys * c->scale / (c->current ? 1e-12 : 1e-3);
}

/* the volts the board would read when the cell reports this pA or mV */
static double meas_to_volts(const InCh *in, int m, const Chan *c, double x)
{
    double phys = x * (c->current ? 1e-12 : 1e-3) / c->scale;
    return (phys / in->gain[m] - in->cal_offset) / in->cal_scale;
}

static int setup_channels(Nrn *s, const Config *cfg, Err *err)
{
    int m = MODE_INDEX(cfg->mode), i;
    s->nout = s->nin = 0;
    for (i = 0; i < cfg->n_out; i++) {
        const OutCh *o = &cfg->out[i];
        Dim dim; double sc;
        if (o->skip || o->digital) continue;
        if (s->nout >= MAXCH || units_lookup(o->unit[m], &dim, &sc) < 0)
            return err_set(err, "output %s: the unit '%s' is not known to the NEURON backend", o->name, o->unit[m]);
        s->out[s->nout].idx = i; s->out[s->nout].current = dim == DIM_CURRENT; s->out[s->nout].scale = sc;
        s->nout++;
    }
    for (i = 0; i < cfg->n_in; i++) {
        const InCh *in = &cfg->in[i];
        Dim dim; double sc;
        if (in->skip) continue;
        if (s->nin >= MAXCH || units_lookup(in->unit[m], &dim, &sc) < 0)
            return err_set(err, "input %s: the unit '%s' is not known to the NEURON backend", in->name, in->unit[m]);
        s->in[s->nin].idx = i; s->in[s->nin].current = dim == DIM_CURRENT; s->in[s->nin].scale = sc;
        s->nin++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Talking to the adapter                                              */
/* ------------------------------------------------------------------ */

/* The last lines of the adapter's messages, for an error message. */
static void log_tail(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    char buf[1200];
    size_t k, i, start;
    out[0] = 0;
    if (!f) return;
    fseek(f, 0, SEEK_END);
    {
        long sz = ftell(f);
        long from = sz > (long)sizeof buf - 1 ? sz - (long)sizeof buf + 1 : 0;
        fseek(f, from, SEEK_SET);
    }
    k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == ' ')) k--;
    buf[k] = 0;
    /* keep the last two lines */
    start = k;
    for (i = k, k = 0; i > 0; i--)
        if (buf[i - 1] == '\n' && ++k == 2) { start = i; break; }
    if (k < 2) start = 0;
    snprintf(out, n, "%s", buf + start);
    for (i = 0; out[i]; i++) if (out[i] == '\n') out[i] = '|';
}

static int die(Nrn *s, Err *err, const char *what)
{
    char tail[420];
    int status = platform_child_stop(s->pid, s->to, s->from, 500);
    s->pid = 0; s->to = s->from = -1; s->up = 0;
    log_tail(s->errpath, tail, sizeof tail);
    return err_set(err, "the NEURON adapter %s (exit status %d)%s%s; its messages are in %s", what, status,
                   tail[0] ? ": " : "", tail, s->errpath);
}

static int send_line(Nrn *s, const char *line, Err *err)
{
    if (platform_write_all(s->to, line, strlen(line)) < 0 || platform_write_all(s->to, "\n", 1) < 0)
        return die(s, err, "stopped (its pipe is closed)");
    return 0;
}

/* Read the reply header.  Returns 0 and the parsed JSON, 1 on an abort, -1 on an error. */
static int read_reply(Nrn *s, int timeout_s, Json **out, Err *err)
{
    char *line = (char *)malloc(LINE_MAX_BYTES);
    size_t n = 0;
    int r;
    Json *j;
    *out = NULL;
    if (!line) return err_set(err, "out of memory");
    for (;;) {
        char c;
        r = platform_read_exact(s->from, &c, 1, (int64_t)timeout_s * 1000);
        if (r == 1) {
            if (c == '\n') break;
            if (n + 1 >= LINE_MAX_BYTES) { free(line); return err_set(err, "the NEURON adapter sent a message that is too long"); }
            line[n++] = c;
            continue;
        }
        free(line);
        if (r == -3) return 1;
        if (r == 0) return die(s, err, "stopped without answering");
        if (r == -2) {
            char m[80];
            snprintf(m, sizeof m, "did not answer within %d s", timeout_s);
            die(s, err, m);
            return -1;
        }
        return err_set(err, "reading from the NEURON adapter failed");
    }
    line[n] = 0;
    free(s->lastline);
    s->lastline = (char *)malloc(n + 1);                  /* a copy (strdup is not ISO C99, and without its prototype a
                                                             64-bit pointer is cut to an int: a crash on Linux) */
    if (s->lastline) memcpy(s->lastline, line, n + 1);
    j = json_parse(line, n, err);
    free(line);
    if (!j) { char m[200]; snprintf(m, sizeof m, "sent something that is not JSON (%s)", err->msg); return die(s, err, m); }
    {
        const Json *ok = json_get(j, "ok");
        if (!ok || ok->type != J_BOOL || !ok->boolean) {
            const char *e = json_str_or(j, "error", "an unknown error");
            err_set(err, "%s", e);
            json_free(j);
            return -1;
        }
    }
    *out = j;
    return 0;
}

static int build_init(Nrn *s, const Config *cfg, int check, StrBuf *b)
{
    int k, first;
    sb_add(b, "{\"op\":\"init\",\"prep\":"); sb_json(b, cfg->prep);
    sb_add(b, ",\"dir\":"); sb_json(b, cfg->cfg_dir);
    sb_add(b, ",\"outs\":[");
    for (k = 0, first = 1; k < s->nout; k++, first = 0)
        sb_addf(b, "%s{\"name\":\"%s\",\"dim\":\"%s\"}", first ? "" : ",", cfg->out[s->out[k].idx].name,
                s->out[k].current ? "current" : "voltage");
    sb_add(b, "],\"ins\":[");
    for (k = 0, first = 1; k < s->nin; k++, first = 0)
        sb_addf(b, "%s{\"name\":\"%s\",\"dim\":\"%s\"}", first ? "" : ",", cfg->in[s->in[k].idx].name,
                s->in[k].current ? "current" : "voltage");
    sb_addf(b, "],\"dt_ms\":%.9g,\"celsius\":%.9g,\"v_init\":%.9g,\"settle_ms\":%.9g,\"check\":%s}",
            cfg->sim_dt_ms, cfg->sim_celsius, cfg->sim_v_init, cfg->sim_settle_ms, check ? "true" : "false");
    return b->failed ? -1 : 0;
}

static int start_adapter(Nrn *s, const Config *cfg, int check, Err *err)
{
    char *argv[3], msg[200];
    StrBuf b;
    Json *rep = NULL;
    int r;

    platform_ignore_sigpipe();
    if (setup_channels(s, cfg, err) < 0) return -1;
    argv[0] = s->python; argv[1] = s->script; argv[2] = NULL;
    if (platform_spawn(argv, s->errpath, &s->pid, &s->to, &s->from, msg, sizeof msg) < 0)
        return err_set(err, "%s", msg);
    memset(&b, 0, sizeof b);
    if (build_init(s, cfg, check, &b) < 0) { sb_free(&b); return err_set(err, "out of memory"); }
    if (send_line(s, b.s, err) < 0) { sb_free(&b); return -1; }
    sb_free(&b);
    r = read_reply(s, s->o.init_timeout_s, &rep, err);
    if (r == 1) {                                     /* Ctrl-C while the model was being built */
        platform_child_stop(s->pid, s->to, s->from, 500);
        s->pid = 0; s->to = s->from = -1;
        return err_set(err, "interrupted while the NEURON model was being built");
    }
    if (r < 0) {                                      /* the adapter's own message says what is wrong */
        if (s->pid) platform_child_stop(s->pid, s->to, s->from, 500);
        s->pid = 0; s->to = s->from = -1;
        return -1;
    }
    if (!check && !s->o.quiet) {                      /* what the adapter wants said when a run starts */
        const Json *notes = json_get(rep, "notes");
        size_t k;
        for (k = 0; notes && k < notes->n; k++)
            if (json_str(json_at(notes, k))) fprintf(stderr, "lcg2: %s\n", json_str(json_at(notes, k)));
    }
    s->desc = s->lastline;                            /* keep the description for the archive */
    s->lastline = NULL;
    s->up = 1;
    json_free(rep);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The five operations                                                 */
/* ------------------------------------------------------------------ */

static int nrn_open(Backend *be, const Config *cfg, Err *err)
{
    Nrn *s = (Nrn *)be->impl;
    int k;
    if (cfg->device != DEV_NEURON)
        return err_set(err, "the NEURON backend was asked to run device '%s'", cfg->device_name);
    if (start_adapter(s, cfg, 0, err) < 0) return -1;
    for (k = 0; k < s->nout; k++) s->rest_cmd[k] = 0.0;
    return 0;
}

static int find_ao(const Drive *d, int out_idx)
{
    int k;
    for (k = 0; k < d->nao; k++)
        if (d->ao[k].out == out_idx) return k;
    return -1;
}

static int nrn_set_rest(Backend *be, const Config *cfg, const Drive *d, Err *err)
{
    Nrn *s = (Nrn *)be->impl;
    int m = MODE_INDEX(cfg->mode), j;
    (void)err;
    for (j = 0; j < s->nout; j++) {
        int k = find_ao(d, s->out[j].idx);
        if (k >= 0) s->rest_cmd[j] = volts_to_cmd(&cfg->out[s->out[j].idx], m, &s->out[j], d->ao_rest[k]);
    }
    return 0;
}

static int nrn_wait_trigger(Backend *be, const Config *cfg, Clock *clk, Err *err)
{
    int r;
    (void)be; (void)clk; (void)err;
    if (cfg->has_trigger) return 0;         /* no PFI line in a simulation: the edge is simulated, at once */
    fprintf(stderr, "lcg2: start on trigger: no trigger line is configured; press Enter to start\n");
    r = platform_wait_enter();
    return r == 1 ? 0 : 1;
}

static int send_advance(Nrn *s, int64_t dur_ns, Err *err)
{
    char line[64 + MAXCH * 32];
    int n = snprintf(line, sizeof line, "{\"op\":\"advance\",\"dur_ns\":%lld,\"cmd\":[", (long long)dur_ns), j;
    Json *rep;
    int r;
    for (j = 0; j < s->nout; j++)
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%.17g", j ? "," : "", s->rest_cmd[j]);
    snprintf(line + n, sizeof line - (size_t)n, "]}");
    if (send_line(s, line, err) < 0) return -1;
    r = read_reply(s, s->o.block_timeout_s + (int)(dur_ns / 1000000000LL), &rep, err);
    if (r == 0) json_free(rep);
    if (r == 1) s->broken = 1;
    return r;
}

static int nrn_play(Backend *be, Clock *clk, const Config *cfg, const Drive *d,
                    int64_t rate_p, int64_t rate_q, int64_t block_samples,
                    BlockFn block, void *user, PlayResult *res, Err *err)
{
    Nrn *s = (Nrn *)be->impl;
    int m = MODE_INDEX(cfg->mode), j, k, rc = 0;
    int64_t first = 0, t_start, nb = block_samples > 0 ? block_samples : 1, now;
    double *cmd = NULL, *meas = NULL, *volts[MAXCH];
    const double *view[MAXCH];
    int src[MAXCH];
    char line[256];
    Json *rep;

    memset(res, 0, sizeof *res);
    memset(volts, 0, sizeof volts);
    if (!s->up || s->broken) return err_set(err, "the NEURON adapter is not available");
    for (j = 0; j < s->nout; j++) src[j] = find_ao(d, s->out[j].idx);

    cmd = (double *)malloc((size_t)(nb * (s->nout ? s->nout : 1)) * sizeof(double));
    meas = (double *)malloc((size_t)(nb * s->nin) * sizeof(double));
    for (k = 0; k < s->nin; k++) volts[k] = (double *)malloc((size_t)nb * sizeof(double));
    if (!cmd || !meas) { rc = err_set(err, "out of memory"); goto done; }
    for (k = 0; k < s->nin; k++) if (!volts[k]) { rc = err_set(err, "out of memory"); goto done; }

    /* the pause since the previous trial passes in the cells at the rest values */
    now = clock_now(clk);
    if (s->have_last && now > s->last_end_ns) {
        int r = send_advance(s, now - s->last_end_ns, err);
        if (r != 0) { rc = r; if (r == 1) res->incomplete = 1; goto done; }
    }
    snprintf(line, sizeof line, "{\"op\":\"trial\",\"rate_p\":%lld,\"rate_q\":%lld}", (long long)rate_p, (long long)rate_q);
    if (send_line(s, line, err) < 0) { rc = -1; goto done; }
    {
        int r = read_reply(s, s->o.block_timeout_s, &rep, err);
        if (r == 1) { s->broken = 1; res->incomplete = 1; rc = 1; goto done; }
        if (r < 0) { rc = -1; goto done; }
        json_free(rep);
    }

    /* The trial starts, in the cells' time, where the pause ended: at `now`.  Taking the clock again here would
     * leave out the time spent simulating the pause, and in real time (-R) the cells would fall behind the
     * clock by that much at every trial. */
    t_start = now;
    while (first < d->n) {
        int64_t count = d->n - first < nb ? d->n - first : nb, end_ns, i;
        int r;
        if (!units_samples_to_ns(first + count, rate_p, rate_q, &end_ns)) {
            rc = err_set(err, "the trial is too long to time"); goto done;
        }
        for (j = 0; j < s->nout; j++)
            for (i = 0; i < count; i++)
                cmd[j * count + i] = src[j] >= 0
                    ? volts_to_cmd(&cfg->out[s->out[j].idx], m, &s->out[j], d->ao[src[j]].v[first + i]) : 0.0;
        snprintf(line, sizeof line, "{\"op\":\"block\",\"count\":%lld}", (long long)count);
        if (send_line(s, line, err) < 0) { rc = -1; goto done; }
        if (s->nout && platform_write_all(s->to, cmd, (size_t)(count * s->nout) * sizeof(double)) < 0) {
            rc = die(s, err, "stopped (its pipe is closed)"); goto done;
        }
        r = read_reply(s, s->o.block_timeout_s, &rep, err);
        if (r == 1) { s->broken = 1; res->incomplete = 1; rc = 1; goto done; }
        if (r < 0) { rc = -1; goto done; }
        json_free(rep);
        r = platform_read_exact(s->from, meas, (size_t)(count * s->nin) * sizeof(double), (int64_t)s->o.block_timeout_s * 1000);
        if (r == -3) { s->broken = 1; res->incomplete = 1; rc = 1; goto done; }
        if (r != 1) { rc = die(s, err, "stopped in the middle of a block"); goto done; }

        for (k = 0; k < s->nin; k++) {
            const InCh *in = &cfg->in[s->in[k].idx];
            for (i = 0; i < count; i++) {
                double v = meas_to_volts(in, m, &s->in[k], meas[k * count + i]);
                if (!isfinite(v)) { rc = err_set(err, "the NEURON model produced a value that is not a number "
                                                 "(input %s, sample %lld): the model diverged", in->name, (long long)(first + i)); goto done; }
                if (v > in->range)       { v = in->range;  res->overrange[k]++; }
                else if (v < -in->range) { v = -in->range; res->overrange[k]++; }
                volts[k][i] = v;
            }
            view[k] = volts[k];
        }
        /* the block is delivered when its last sample has been taken */
        if (!clock_wait_until(clk, t_start + end_ns)) { res->incomplete = 1; rc = 1; break; }
        if (block(user, first, count, view, err) < 0) { rc = -1; goto done; }
        res->recorded += count;
        first += count;
    }
    if (rc == 0) {
        int64_t total;
        if (units_samples_to_ns(d->n, rate_p, rate_q, &total)) { s->last_end_ns = t_start + total; s->have_last = 1; }
    } else if (rc == 1) {
        s->have_last = 0;
    }
done:
    free(cmd); free(meas);
    for (k = 0; k < MAXCH; k++) free(volts[k]);
    return rc;
}

static int nrn_event(Backend *be, const char *what, char *said, size_t n_said, double *t_ms,
                     char *stage, size_t n_stage, char *cell, size_t n_cell, Err *err)
{
    Nrn *s = (Nrn *)be->impl;
    char line[200];
    Json *rep;
    int r, applied;
    if (!s->up || s->broken) return err_set(err, "the NEURON adapter is not available");
    /* what: a fixed word, from run.c; *t_ms on the way in: when it happened on the run's clock */
    snprintf(line, sizeof line, "{\"op\":\"event\",\"what\":\"%s\",\"t_ms\":%.17g}", what, *t_ms);
    if (send_line(s, line, err) < 0) return -1;
    r = read_reply(s, s->o.block_timeout_s, &rep, err);
    if (r == 1) { s->broken = 1; return err_set(err, "interrupted"); }
    if (r < 0) return -1;
    applied = json_get(rep, "applied") && json_get(rep, "applied")->type == J_BOOL && json_get(rep, "applied")->boolean;
    snprintf(said, n_said, "%s", json_str_or(rep, "said", ""));
    snprintf(stage, n_stage, "%s", json_str_or(rep, "stage", ""));
    snprintf(cell, n_cell, "%s", json_str_or(rep, "cell", ""));
    *t_ms = json_num_or(rep, "t_ms", 0.0);
    json_free(rep);
    return applied;
}

static void nrn_close(Backend *be)
{
    Nrn *s = (Nrn *)be->impl;
    if (s->up && s->to >= 0) {
        const char *q = "{\"op\":\"quit\"}\n";
        platform_write_all(s->to, q, strlen(q));
    }
    if (s->pid) platform_child_stop(s->pid, s->to, s->from, 3000);
    s->pid = 0; s->to = s->from = -1; s->up = 0;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

Backend *neuron_backend_new(const NeuronOpts *opts)
{
    Backend *be = (Backend *)calloc(1, sizeof *be);
    Nrn *s = (Nrn *)calloc(1, sizeof *s);
    if (!be || !s) { free(be); free(s); return NULL; }
    s->o = *opts;
    snprintf(s->python, sizeof s->python, "%s", opts->python);
    snprintf(s->script, sizeof s->script, "%s", opts->script);
    snprintf(s->errpath, sizeof s->errpath, "%s", opts->stderr_path);
    s->o.python = s->python; s->o.script = s->script; s->o.stderr_path = s->errpath;
    if (s->o.init_timeout_s <= 0) s->o.init_timeout_s = 900;
    if (s->o.block_timeout_s <= 0) s->o.block_timeout_s = 300;
    s->to = s->from = -1;
    be->name = "NEURON simulator";
    be->open = nrn_open; be->set_rest = nrn_set_rest; be->wait_trigger = nrn_wait_trigger;
    be->play = nrn_play; be->close = nrn_close; be->event = nrn_event;
    be->impl = s;
    return be;
}

void neuron_backend_free(Backend *be)
{
    Nrn *s;
    if (!be) return;
    s = (Nrn *)be->impl;
    if (s) { free(s->desc); free(s->lastline); free(s); }
    free(be);
}

const char *neuron_backend_description(const Backend *be)
{
    const Nrn *s = (const Nrn *)be->impl;
    return s ? s->desc : NULL;
}

int neuron_check(const NeuronOpts *opts, const Config *cfg, Err *err)
{
    Backend *be = neuron_backend_new(opts);
    Nrn *s;
    StrBuf b;
    Json *rep = NULL;
    int r, k;

    if (!be) return err_set(err, "out of memory");
    s = (Nrn *)be->impl;
    platform_ignore_sigpipe();
    if (setup_channels(s, cfg, err) < 0) { neuron_backend_free(be); return -1; }
    {
        char *argv[3], msg[200];
        argv[0] = s->python; argv[1] = s->script; argv[2] = NULL;
        if (platform_spawn(argv, s->errpath, &s->pid, &s->to, &s->from, msg, sizeof msg) < 0) {
            neuron_backend_free(be);
            return err_set(err, "%s", msg);
        }
    }
    memset(&b, 0, sizeof b);
    if (build_init(s, cfg, 1, &b) < 0) { sb_free(&b); neuron_backend_free(be); return err_set(err, "out of memory"); }
    r = send_line(s, b.s, err);
    sb_free(&b);
    if (r == 0) r = read_reply(s, s->o.init_timeout_s, &rep, err);
    if (r != 0) {
        if (s->pid) platform_child_stop(s->pid, s->to, s->from, 500);
        neuron_backend_free(be);
        if (r == 1) return err_set(err, "interrupted");
        return -1;
    }
    printf("\n  virtual preparation (NEURON %s, Python %s): %d cell(s), %d synapse(s)\n",
           json_str_or(rep, "neuron", "?"), json_str_or(rep, "python", "?"), cfg->n_cells, cfg->n_synapses);
    {
        const Json *cells = json_get(rep, "cells"), *syns = json_get(rep, "synapses");
        for (k = 0; cells && (size_t)k < cells->n; k++) {
            const Json *c = json_at(cells, (size_t)k);
            printf("    cell %-8s %-22s %3d section(s), %4d segment(s), electrode %s, rests at %.1f mV\n",
                   json_str_or(c, "name", "?"), json_str_or(c, "model", "?"), (int)json_num_or(c, "sections", 0),
                   (int)json_num_or(c, "segments", 0), json_str_or(c, "electrode", "?"), json_num_or(c, "v_rest_mV", 0));
        }
        for (k = 0; syns && (size_t)k < syns->n; k++) {
            const Json *c = json_at(syns, (size_t)k);
            printf("    synapse %s: %s -> %s (%s)\n", json_str_or(c, "name", "?"), json_str_or(c, "pre", "?"),
                   json_str_or(c, "post", "?"), json_str_or(c, "type", "?"));
        }
    }
    json_free(rep);
    platform_child_stop(s->pid, s->to, s->from, 3000);
    s->pid = 0;
    neuron_backend_free(be);
    return 0;
}
