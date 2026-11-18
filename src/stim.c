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
 * stim.c -- from StimGen text to checked drive signals.  See stim.h for the
 * path a stimulus takes and for what is promised.
 *
 * This is the only file that calls StimGen, and it calls only sg_api_*
 * functions (declared in stimgen_api.h).  The rules that matter, and where
 * they are enforced:
 *
 *   - The results of an sg_api_* call are valid until the next call, so each
 *     result is copied at once (the .sgb bytes are decoded into the Trial; the
 *     trial list is copied into the Stim).
 *   - A rendered trial is bit for bit what `sg render` writes, because the same
 *     library renders it and the .sgb samples are decoded without any rounding.
 *   - Nothing is clipped silently: stim_check() refuses a trial whose samples
 *     would leave the limits of their output, before anything is played.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "stim.h"
#include "stimgen_api.h"
#include "units.h"

#define MAX_TEXT_BYTES (64L * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *xdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Copy src into a fixed buffer; 0 if it does not fit (never truncates). */
static int copy_fit(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

int stim_rate_fraction(double hz, int64_t *p, int64_t *q)
{
    int64_t den;
    if (!(hz > 0) || !isfinite(hz)) return -1;
    for (den = 1; den <= 1000000; den *= 10) {
        double scaled = hz * (double)den;
        int64_t num;
        if (scaled > 9e15) return -1;
        num = (int64_t)floor(scaled + 0.5);
        if (num > 0 && (double)num / (double)den == hz) {
            /* lowest terms, like units_parse_rate */
            int64_t a = num, b = den;
            while (b) { int64_t t = a % b; a = b; b = t; }
            *p = num / a;
            *q = den / a;
            return 0;
        }
    }
    return -1;
}

/*
 * Turn the message of an sg_api_* call into one that names the user's own
 * stimulus.  StimGen reads every text as a file called "editor", and its rate
 * error names its own flag, --rate.  What the user sees is the file (or "the -e
 * text"), and LCG2's flag -r:
 *
 *   editor:3: bad thing        ->  step.sg:3: bad thing
 *   line 3: bad thing          ->  step.sg:3: bad thing
 *   --rate 10kHz differs ...   ->  -r 10kHz differs ...
 *
 * A trial of a protocol is rendered from its own expanded text, so a line number
 * in an error counts lines of THAT text, not of the protocol file.  Then the
 * message says so, rather than pointing at the wrong line of the file:
 *
 *   p.sg (trial 0003, line 5 of that trial's text): bad thing
 *
 * `trial` is NULL for everything else.
 */
static int sg_fail(const char *file, const char *trial, Err *err)
{
    const char *m = sg_api_error();
    char head[360], out[320], num[16];
    const char *p = m;
    size_t k = 0, j = 0;

    num[0] = 0;
    if (strncmp(p, "editor", 6) == 0 && (p[6] == ':' || p[6] == ' ')) {
        p += 6;
        if (*p == ':' && p[1] >= '0' && p[1] <= '9') {      /* "editor:3: ..." */
            const char *d = p + 1;
            while (*d >= '0' && *d <= '9' && j < sizeof num - 1) num[j++] = *d++;
            num[j] = 0;
            p = d;
        }
    } else if (strncmp(p, "line ", 5) == 0) {
        const char *d = p + 5;
        while (*d >= '0' && *d <= '9' && j < sizeof num - 1) num[j++] = *d++;
        num[j] = 0;
        if (j && *d == ':') p = d; else num[0] = 0;
    }

    if (trial && num[0])
        snprintf(head, sizeof head, "%s (trial %s, line %s of that trial's text)", file, trial, num);
    else if (trial)
        snprintf(head, sizeof head, "%s (trial %s)", file, trial);
    else if (num[0])
        snprintf(head, sizeof head, "%s:%s", file, num);
    else
        snprintf(head, sizeof head, "%s", file);
    if (*p != ':') {                                 /* the separator is always there */
        k = (size_t)snprintf(out, sizeof out, "%s: ", head);
    } else {
        k = (size_t)snprintf(out, sizeof out, "%s", head);
    }
    if (k >= sizeof out) k = sizeof out - 1;
    /* the rest, with "--rate" spelled as LCG2 spells it */
    while (*p && k < sizeof out - 1) {
        if (strncmp(p, "--rate", 6) == 0) {
            if (k + 2 >= sizeof out) break;
            out[k++] = '-'; out[k++] = 'r';
            p += 6;
        } else {
            out[k++] = *p++;
        }
    }
    out[k] = 0;
    return err_set(err, "%s", out);
}

/* ------------------------------------------------------------------ */
/* Freeing                                                             */
/* ------------------------------------------------------------------ */

void trial_free(Trial *t)
{
    int i;
    for (i = 0; i < t->nch; i++) free(t->ch[i].x);
    for (i = 0; i < t->nwarn; i++) free(t->warn[i]);
    for (i = 0; i < t->nfiles; i++) { free(t->file_path[i]); free(t->file_sha[i]); }
    free(t->warn); free(t->file_path); free(t->file_sha); free(t->mark); free(t->info);
    memset(t, 0, sizeof *t);
}

void stim_free(Stim *st)
{
    int i;
    for (i = 0; st->td && i < st->n; i++) {
        free(st->td[i].name); free(st->td[i].seed);
        free(st->td[i].text); free(st->td[i].info);
    }
    free(st->td);
    free(st->text);
    free(st->protocol_json);
    free(st->digest);
    for (i = 0; i < st->nused; i++) { free(st->used_path[i]); free(st->used_sha[i]); }
    free(st->used_path); free(st->used_sha);
    memset(st, 0, sizeof *st);
}

/* ------------------------------------------------------------------ */
/* Reading the text                                                    */
/* ------------------------------------------------------------------ */

int stim_read_file(const char *path, char **text, Err *err)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 4096, n = 0, got;
    char *buf;

    if (!f)
        return err_set(err, "cannot open stimulus file '%s': %s", path, strerror(errno));
    buf = (char *)malloc(cap);
    if (!buf) { fclose(f); return err_set(err, "out of memory"); }
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n > (size_t)MAX_TEXT_BYTES) {
            free(buf); fclose(f);
            return err_set(err, "%s: the stimulus file is over 64 MiB", path);
        }
        if (n + 1 >= cap) {
            char *g;
            cap *= 2;
            g = (char *)realloc(buf, cap);
            if (!g) { free(buf); fclose(f); return err_set(err, "out of memory"); }
            buf = g;
        }
    }
    if (ferror(f)) {
        free(buf); fclose(f);
        return err_set(err, "error reading '%s': %s", path, strerror(errno));
    }
    fclose(f);
    buf[n] = 0;
    if (memchr(buf, 0, n)) {
        free(buf);
        return err_set(err, "%s: the stimulus file contains a NUL byte, so it is not text", path);
    }
    *text = buf;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decoding what StimGen renders                                       */
/* ------------------------------------------------------------------ */

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static double get_f64(const uint8_t *p)
{
    uint64_t v = get_u64(p);
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}

/* Warnings as StimGen reports them: a JSON array of strings. */
static void take_warnings(Trial *t, const char *json)
{
    Json *j = json_parse(json, strlen(json), &(Err){ {0} });
    size_t k;
    if (!j) return;
    for (k = 0; k < j->n; k++) {
        const char *s = json_str(j->item[k]);
        char **g;
        if (!s) continue;
        g = (char **)realloc(t->warn, ((size_t)t->nwarn + 1) * sizeof *g);
        if (!g) break;
        t->warn = g;
        t->warn[t->nwarn] = xdup(s);
        if (t->warn[t->nwarn]) t->nwarn++;
    }
    json_free(j);
}

/*
 * The complete .sgb file (StimGen spec, Table 15.1):
 *     8 bytes   "SGB" and five zero bytes
 *     8 bytes   length L of the JSON header, little-endian
 *     L bytes   the JSON header
 *     8*J*N     the samples, IEEE 754 binary64, little-endian, channel after channel
 * Everything is checked, in particular that the file has exactly the size the
 * header promises.
 */
static int decode_sgb(const uint8_t *b, size_t size, Trial *t, Err *err)
{
    static const uint8_t magic[8] = { 'S', 'G', 'B', 0, 0, 0, 0, 0 };
    uint64_t L;
    Json *h = NULL;
    const Json *chs, *mk, *prov, *files;
    int64_t N;
    size_t J, k, i, offset;
    double rate;

    if (size < 16 || memcmp(b, magic, 8) != 0)
        return err_set(err, "StimGen returned something that is not an .sgb file");
    L = get_u64(b + 8);
    if (L > size - 16)
        return err_set(err, "the .sgb header is longer than the file");
    h = json_parse((const char *)b + 16, (size_t)L, err);
    if (!h) return -1;

    rate = json_num_or(h, "rate", 0);
    N = (int64_t)json_num_or(h, "samples", 0);
    chs = json_get(h, "channels");
    if (!(rate > 0) || N < 1 || !chs || chs->type != J_ARR || chs->n < 1) {
        json_free(h);
        return err_set(err, "the .sgb header lacks a rate, samples or channels");
    }
    J = chs->n;
    if (J > CFG_MAX_OUT) {
        json_free(h);
        return err_set(err, "the stimulus has %lu channels; at most %d are supported",
                       (unsigned long)J, CFG_MAX_OUT);
    }
    if ((uint64_t)N > (size - 16 - L) / (8 * J) || size != 16 + L + 8 * J * (uint64_t)N) {
        json_free(h);
        return err_set(err, "the .sgb file has a different size from what its header says");
    }

    t->rate = rate;
    if (stim_rate_fraction(rate, &t->rate_p, &t->rate_q) < 0) {
        json_free(h);
        return err_set(err, "StimGen reports the rate %.17g Hz, which is not a decimal rate", rate);
    }
    t->n = N;
    offset = 16 + (size_t)L;
    for (k = 0; k < J; k++) {
        const Json *c = chs->item[k];
        SChan *ch = &t->ch[k];
        const char *name = json_str_or(c, "name", ""), *unit = json_str_or(c, "unit", "");
        const Json *dg = json_get(c, "digital");
        if (!copy_fit(ch->name, sizeof ch->name, name) || !name[0] ||
            !copy_fit(ch->unit, sizeof ch->unit, unit)) {
            json_free(h);
            return err_set(err, "a channel name or unit in the stimulus is empty or too long "
                           "(names are at most %d characters)", CFG_NAME - 1);
        }
        ch->rest = json_num_or(c, "rest", 0);
        ch->digital = dg && dg->type == J_BOOL && dg->boolean;
        ch->x = (double *)malloc((size_t)N * sizeof(double));
        t->nch = (int)k + 1;                         /* so trial_free sees it */
        if (!ch->x) { json_free(h); return err_set(err, "out of memory"); }
        for (i = 0; i < (size_t)N; i++)
            ch->x[i] = get_f64(b + offset + 8 * (k * (size_t)N + i));
    }

    mk = json_get(h, "markers");
    if (mk && mk->type == J_ARR && mk->n) {
        t->mark = (Marker *)calloc(mk->n, sizeof *t->mark);
        if (!t->mark) { json_free(h); return err_set(err, "out of memory"); }
        for (k = 0; k < mk->n; k++) {
            copy_fit(t->mark[k].name, sizeof t->mark[k].name, json_str_or(mk->item[k], "name", ""));
            t->mark[k].sample = (int64_t)json_num_or(mk->item[k], "sample", 0);
        }
        t->nmark = (int)mk->n;
    }

    prov = json_get(h, "provenance");
    if (!copy_fit(t->seed, sizeof t->seed, json_str_or(prov, "master_seed", "")) || !t->seed[0]) {
        json_free(h);
        return err_set(err, "the .sgb header has no master seed");
    }
    copy_fit(t->samples_sha256, sizeof t->samples_sha256, json_str_or(prov, "samples_sha256", ""));
    files = json_get(prov, "files");
    if (files && files->type == J_ARR && files->n) {
        t->file_path = (char **)calloc(files->n, sizeof(char *));
        t->file_sha = (char **)calloc(files->n, sizeof(char *));
        if (!t->file_path || !t->file_sha) { json_free(h); return err_set(err, "out of memory"); }
        for (k = 0; k < files->n; k++) {
            t->file_path[k] = xdup(json_str_or(files->item[k], "path", ""));
            t->file_sha[k] = xdup(json_str_or(files->item[k], "sha256", ""));
            t->nfiles = (int)k + 1;
            if (!t->file_path[k] || !t->file_sha[k]) { json_free(h); return err_set(err, "out of memory"); }
        }
    }
    json_free(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendering one trial                                                 */
/* ------------------------------------------------------------------ */

/* Render trial i with an explicit rate text.  `where` names the trial in
 * messages. */
static int render_with(const Stim *st, int i, const char *rate_arg, Trial *t, Err *err)
{
    const char *text, *seed, *info, *name, *unit, *trial;
    int rc;

    memset(t, 0, sizeof *t);
    if (st->is_protocol) {
        const TrialDesc *td = &st->td[i];
        text = td->text; seed = td->seed; info = td->info; name = td->name;
        trial = td->name;
    } else {
        text = st->text; seed = st->seed_arg; info = ""; name = "stimulus";
        trial = NULL;
    }
    unit = st->waveform ? st->unit : "";

    rc = sg_api_render_trial(text, rate_arg, seed, unit, info, st->is_protocol ? st->text : "");
    if (rc != 0) return sg_fail(st->source, trial, err);
    take_warnings(t, sg_api_warnings());
    if (decode_sgb(sg_api_result(), sg_api_result_size(), t, err) < 0) {
        trial_free(t);
        return -1;
    }
    t->index = i;
    copy_fit(t->name, sizeof t->name, name);
    if (st->is_protocol && info[0]) t->info = xdup(info);
    return 0;
}

int stim_render(const Stim *st, int i, Trial *t, Err *err)
{
    return render_with(st, i, st->rate_arg, t, err);
}

/* ------------------------------------------------------------------ */
/* Opening a stimulus                                                  */
/* ------------------------------------------------------------------ */

static const OutCh *find_out(const Config *c, const char *name)
{
    int k;
    for (k = 0; k < c->n_out; k++)
        if (strcmp(c->out[k].name, name) == 0) return &c->out[k];
    return NULL;
}

/* Copy a string of the expanded protocol; NULL if absent. */
static char *jdup(const Json *o, const char *key)
{
    const char *s = json_str(json_get(o, key));
    return s ? xdup(s) : NULL;
}

/* The trial list of a protocol, from sg_api_expand's JSON. */
static int read_trials(Stim *st, const char *json, Err *err)
{
    Json *j = json_parse(json, strlen(json), err);
    const Json *tr;
    size_t k;

    if (!j) return -1;
    tr = json_get(j, "trials");
    if (!tr || tr->type != J_ARR || tr->n == 0) {
        json_free(j);
        return err_set(err, "%s: the protocol has no trials", st->source);
    }
    st->td = (TrialDesc *)calloc(tr->n, sizeof *st->td);
    if (!st->td) { json_free(j); return err_set(err, "out of memory"); }
    st->n = 0;
    for (k = 0; k < tr->n; k++) {
        const Json *o = tr->item[k], *info = json_get(o, "info");
        TrialDesc *td = &st->td[k];
        td->name = jdup(o, "name");
        td->seed = jdup(o, "seed");
        td->text = jdup(o, "text");
        td->info = (info && info->end > info->begin)
                   ? (char *)calloc(info->end - info->begin + 1, 1) : NULL;
        if (td->info) memcpy(td->info, json + info->begin, info->end - info->begin);
        st->n = (int)k + 1;                       /* so stim_free sees it */
        if (!td->name || !td->seed || !td->text || !td->info) {
            json_free(j);
            return err_set(err, "%s: the expanded protocol is malformed (trial %lu)",
                           st->source, (unsigned long)k);
        }
    }
    copy_fit(st->protocol_seed, sizeof st->protocol_seed, json_str_or(j, "protocol_seed", ""));
    copy_fit(st->timing, sizeof st->timing, json_str_or(j, "timing", ""));
    copy_fit(st->start, sizeof st->start, json_str_or(j, "start", ""));
    json_free(j);
    return 0;
}

int stim_open(Stim *st, char *text, const char *source, const Options *opt,
              const Config *cfg, Err *err)
{
    char kind[16];
    Trial t;
    char first_rate[48];
    Err e1;
    int rc;

    memset(st, 0, sizeof *st);
    st->text = text;
    if (!copy_fit(st->source, sizeof st->source, source)) {
        stim_free(st);
        return err_set(err, "the stimulus name is too long");
    }

    /* What is it? */
    if (sg_api_kind(st->text) != 0) { sg_fail(st->source, NULL, err); stim_free(st); return -1; }
    if (!copy_fit(kind, sizeof kind, sg_api_text())) kind[0] = 0;
    if (strcmp(kind, "index") == 0) {
        stim_free(st);
        return err_set(err, "%s: a folder of trials (protocol.sgi) is not supported; give "
                       "the protocol text", source);
    }
    if (strcmp(kind, "protocol") == 0) {
        char *json;
        st->is_protocol = 1;
        if (sg_api_expand(st->text, opt->seed ? opt->seed : "") != 0) {
            sg_fail(st->source, NULL, err); stim_free(st); return -1;
        }
        json = xdup(sg_api_text());                /* copy before the next call */
        if (!json) { stim_free(st); return err_set(err, "out of memory"); }
        st->protocol_json = json;
        if (read_trials(st, json, err) < 0) { stim_free(st); return -1; }
        /* Where did the protocol seed come from?  -s, or the protocol itself
         * (a "seed" line: every expansion gives the same seed), or a random draw
         * (every expansion gives another).  Expand once more and compare. */
        if (opt->seed) {
            st->seed_source = SEED_GIVEN;
        } else if (sg_api_expand(st->text, "") == 0) {
            Json *again = json_parse(sg_api_text(), strlen(sg_api_text()), &(Err){ {0} });
            if (again && strcmp(json_str_or(again, "protocol_seed", ""), st->protocol_seed) == 0)
                st->seed_source = SEED_STATED;
            json_free(again);
        }
        /* All the trials must be of one kind: a plain waveform or a stimulus. */
        {
            int i, wf = -1;
            for (i = 0; i < st->n; i++) {
                int w;
                if (sg_api_kind(st->td[i].text) != 0) {
                    sg_fail(st->source, st->td[i].name, err); stim_free(st); return -1;
                }
                w = strcmp(sg_api_text(), "waveform") == 0;
                if (wf >= 0 && w != wf) {
                    stim_free(st);
                    return err_set(err, "%s: the trials are not all of the same kind (some are "
                                   "plain waveforms, some are stimuli)", source);
                }
                wf = w;
            }
            st->waveform = wf == 1;
        }
    } else if (strcmp(kind, "waveform") == 0) {
        st->waveform = 1;
        st->n = 1;
    } else if (strcmp(kind, "stimulus") == 0) {
        st->n = 1;
    } else {
        stim_free(st);
        return err_set(err, "%s: StimGen does not know what this text is ('%s')", source, kind);
    }

    /* A plain waveform names no channel: -T says who gets it, and its unit is
     * the unit of that output in the mode of this run.  -T means nothing for a
     * stimulus that names its channels, so it is an error, not ignored. */
    if (st->waveform) {
        const OutCh *o;
        if (!opt->target) {
            stim_free(st);
            return err_set(err, "%s is a plain waveform: it names no channel, so say which "
                           "pipette gets it with -T NAME", source);
        }
        o = find_out(cfg, opt->target);
        if (!o || o->skip || o->digital || !o->unit[MODE_INDEX(cfg->mode)][0]) {
            stim_free(st);
            return err_set(err, "-T: '%s' is not an analog output in use", opt->target);
        }
        snprintf(st->unit, sizeof st->unit, "%s", o->unit[MODE_INDEX(cfg->mode)]);
    } else if (opt->target) {
        stim_free(st);
        return err_set(err, "-T is only for a plain waveform; %s names its channels itself",
                       source);
    }

    /* Which rate?  SEED 4.1: the stimulus's own rate is used; -r must agree
     * with it (StimGen refuses otherwise); with neither, the [rig] rate.  So
     * first try with -r only (or none).  If StimGen then says that no rate was
     * given, try the rate of the configuration.  The wording of that one message
     * is fixed by the pin (STIMGEN_PIN) and is tested. */
    /* An explicit -s is the seed of a plain stimulus from the first render on
     * (for a protocol it was handed to sg_api_expand above).  Without -s the
     * first render draws a seed, which is read back below and fixed. */
    if (!st->is_protocol && opt->seed && !copy_fit(st->seed_arg, sizeof st->seed_arg, opt->seed)) {
        stim_free(st);
        return err_set(err, "the seed is too long");
    }
    snprintf(first_rate, sizeof first_rate, "%s", opt->has_rate ? opt->rate_text : "");
    snprintf(st->rate_arg, sizeof st->rate_arg, "%s", first_rate);
    rc = render_with(st, 0, st->rate_arg, &t, &e1);
    if (rc < 0 && !opt->has_rate && strstr(e1.msg, "no sampling rate") != NULL) {
        if (!cfg->has_rate) {
            stim_free(st);
            return err_set(err, "%s: no sampling rate: the stimulus does not state one, so "
                           "give -r RATE or write rate = in [rig]", source);
        }
        snprintf(st->rate_arg, sizeof st->rate_arg, "%s", cfg->rate_text);
        rc = render_with(st, 0, st->rate_arg, &t, &e1);
    }
    if (rc < 0) { *err = e1; stim_free(st); return -1; }

    /* Where did the seed of a plain stimulus come from?  -s; or the stimulus
     * itself (a "seed" line: every render gives the same seed); or a random
     * draw (every render gives another).  Render once more and compare. */
    if (!st->is_protocol) {
        if (opt->seed) {
            st->seed_source = SEED_GIVEN;
        } else {
            Trial again;
            Err ignore;
            if (render_with(st, 0, st->rate_arg, &again, &ignore) == 0) {
                if (strcmp(again.seed, t.seed) == 0) st->seed_source = SEED_STATED;
                trial_free(&again);
            }
        }
    }

    /* A plain stimulus: fix its seed now, so that every later render is the
     * same.  (A protocol's trial seeds are already fixed in its trial list.) */
    if (!st->is_protocol && !copy_fit(st->seed_arg, sizeof st->seed_arg, t.seed)) {
        trial_free(&t);
        stim_free(st);
        return err_set(err, "internal error: seed too long");
    }
    trial_free(&t);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Which output carries each channel                                   */
/* ------------------------------------------------------------------ */

static void list_outputs(const Config *c, char *buf, size_t n)
{
    int k;
    buf[0] = 0;
    for (k = 0; k < c->n_out; k++)
        if (!c->out[k].skip) {
            if (buf[0]) strncat(buf, ", ", n - strlen(buf) - 1);
            strncat(buf, c->out[k].name, n - strlen(buf) - 1);
        }
    if (!buf[0]) snprintf(buf, n, "none");
}

int stim_map(const Stim *st, const Trial *t, const Options *opt, const Config *cfg,
             Map *map, Err *err)
{
    int k, j, m = MODE_INDEX(cfg->mode);

    for (k = 0; k < t->nch; k++) {
        const SChan *ch = &t->ch[k];
        const char *name = st->waveform ? opt->target : ch->name;
        const OutCh *o = NULL;
        int idx = -1;

        for (j = 0; j < cfg->n_out; j++)
            if (strcmp(cfg->out[j].name, name) == 0) { o = &cfg->out[j]; idx = j; }
        if (!o) {
            char list[200];
            list_outputs(cfg, list, sizeof list);
            return err_set(err, "the stimulus has a channel '%s', but the configuration has no "
                           "[out %s] (outputs in use: %s)", name, name, list);
        }
        if (o->skip)
            return err_set(err, "the stimulus has a channel '%s', but [out %s] is skipped in this "
                           "configuration", name, name);
        if (ch->digital != o->digital)
            return err_set(err, "channel '%s' is %s in the stimulus but [out %s] is %s", name,
                           ch->digital ? "digital" : "analog", name,
                           o->digital ? "a digital line" : "analog");
        map[k].out = idx;
        map[k].scale = 1.0;
        if (!ch->digital) {
            Dim sd, od;
            double ss, os;
            if (units_lookup(ch->unit, &sd, &ss) < 0)
                return err_set(err, "channel '%s' has the unit '%s', which is not a current "
                               "(pA, nA, ...) or a voltage (mV, V, ...)", name, ch->unit);
            if (units_lookup(o->unit[m], &od, &os) < 0)
                return err_set(err, "internal error: the unit of [out %s] is unknown", name);
            if (sd != od)
                return err_set(err, "channel '%s' is in %s (%s), but [out %s] carries %s in %s "
                               "mode (%s); use -m %s, or change the unit of the stimulus", name,
                               ch->unit, sd == DIM_CURRENT ? "a current" : "a voltage", name,
                               od == DIM_CURRENT ? "a current" : "a voltage",
                               config_mode_name(cfg->mode), o->unit[m],
                               cfg->mode == MODE_CC ? "vc" : "cc");
            map[k].scale = ss / os;
        }
        for (j = 0; j < k; j++)
            if (map[j].out == idx)
                return err_set(err, "two channels of the stimulus go to [out %s]", name);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The clipping check                                                  */
/* ------------------------------------------------------------------ */

static void label(const Trial *t, char *buf, size_t n)
{
    if (t->info) snprintf(buf, n, "trial %s", t->name);
    else snprintf(buf, n, "the stimulus");
}

int stim_check(const Trial *t, const Map *map, const Config *cfg, double *peak_volts,
               Err *err)
{
    int k;
    char who[96];
    label(t, who, sizeof who);

    for (k = 0; k < t->nch; k++) {
        const SChan *ch = &t->ch[k];
        const OutCh *o = &cfg->out[map[k].out];
        double lo = INFINITY, hi = -INFINITY, sc = map[k].scale;
        int64_t i, first = -1, bad = 0;
        Err why;

        if (ch->digital) {
            for (i = 0; i < t->n; i++)
                if (ch->x[i] != 0.0 && ch->x[i] != 1.0)
                    return err_set(err, "%s, channel %s: sample %lld is %g, but a digital line "
                                   "takes only 0 and 1", who, ch->name, (long long)i, ch->x[i]);
            continue;
        }
        for (i = 0; i < t->n; i++) {
            double v = ch->x[i];
            if (!isfinite(v))
                return err_set(err, "%s, channel %s: sample %lld is not a finite number", who,
                               ch->name, (long long)i);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        /* The idle value is driven before, between and after trials. */
        if (out_check_value(o, cfg->mode, ch->rest * sc, &why) < 0)
            return err_set(err, "%s, channel %s: the rest value %g %s: %s", who, ch->name,
                           ch->rest, ch->unit, why.msg);
        /* The map from a sample to volts is a straight line (a positive gain, a
         * calibration scale of either sign), so if the smallest and the largest
         * sample are allowed, every sample between them is. */
        if (out_check_value(o, cfg->mode, lo * sc, &why) == 0 &&
            out_check_value(o, cfg->mode, hi * sc, &why) == 0) {
            if (peak_volts) {
                double a = fabs(out_wanted_volts(o, cfg->mode, lo * sc));
                double b = fabs(out_wanted_volts(o, cfg->mode, hi * sc));
                if (a > peak_volts[k]) peak_volts[k] = a;
                if (b > peak_volts[k]) peak_volts[k] = b;
            }
            continue;
        }

        /* Something clips: find the first sample and count them all. */
        for (i = 0; i < t->n; i++)
            if (out_check_value(o, cfg->mode, ch->x[i] * sc, &why) < 0) {
                if (first < 0) first = i;
                bad++;
            }
        out_check_value(o, cfg->mode, ch->x[first] * sc, &why);
        if (bad > 1)
            return err_set(err, "%s, channel %s: sample %lld (%.6g s): %s; %lld of %lld samples "
                           "clip in all", who, ch->name, (long long)first,
                           (double)first / t->rate, why.msg, (long long)bad, (long long)t->n);
        return err_set(err, "%s, channel %s: sample %lld (%.6g s): %s", who, ch->name,
                       (long long)first, (double)first / t->rate, why.msg);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The signals for the backend                                         */
/* ------------------------------------------------------------------ */

void drive_free(Drive *d)
{
    int k;
    for (k = 0; k < d->nao; k++) free(d->ao[k].v);
    for (k = 0; k < d->ndig; k++) free(d->dig[k].b);
    memset(d, 0, sizeof *d);
}

/*
 * Build the drive of a trial, or (t == NULL) of an idle stretch of n samples.
 * One function for both, so that an idle output means the same thing in both.
 */
static int build_drive(const Trial *t, const Map *map, const Config *cfg, int64_t n,
                       Drive *d, Err *err)
{
    int k, j;
    int64_t i, total = n + STIM_REST_PAD;

    memset(d, 0, sizeof *d);
    d->n = n;
    d->pad = STIM_REST_PAD;

    for (k = 0; k < cfg->n_out; k++) {
        const OutCh *o = &cfg->out[k];
        const SChan *ch = NULL;
        double sc = 1.0;

        if (o->skip) continue;
        for (j = 0; t && j < t->nch; j++)
            if (map[j].out == k) { ch = &t->ch[j]; sc = map[j].scale; }

        if (o->digital) {
            unsigned char *b;
            if (d->ndig >= 8) { drive_free(d); return err_set(err, "too many digital lines"); }
            b = (unsigned char *)calloc((size_t)total, 1);
            if (!b) { drive_free(d); return err_set(err, "out of memory"); }
            if (ch) {
                for (i = 0; i < n; i++) b[i] = ch->x[i] != 0.0;
                for (i = n; i < total; i++) b[i] = ch->rest != 0.0;
            }
            d->dig_rest[d->ndig] = ch ? (ch->rest != 0.0) : 0;
            d->dig[d->ndig].out = k;
            d->dig[d->ndig++].b = b;
        } else {
            double *v;
            double rest_volts;
            if (d->nao >= 4) { drive_free(d); return err_set(err, "too many analog outputs"); }
            v = (double *)malloc((size_t)total * sizeof(double));
            if (!v) { drive_free(d); return err_set(err, "out of memory"); }
            /* An output the stimulus does not name sits at the rest value of
             * the configuration (in the unit of the mode); a named one rests at
             * the stimulus's own rest value. */
            rest_volts = out_written_volts(o, out_wanted_volts(o, cfg->mode,
                                           ch ? ch->rest * sc : o->rest));
            if (ch)
                for (i = 0; i < n; i++)
                    v[i] = out_written_volts(o, out_wanted_volts(o, cfg->mode, ch->x[i] * sc));
            else
                for (i = 0; i < n; i++) v[i] = rest_volts;
            for (i = n; i < total; i++) v[i] = rest_volts;
            d->ao_rest[d->nao] = rest_volts;
            d->ao[d->nao].out = k;
            d->ao[d->nao++].v = v;
        }
    }
    return 0;
}

int stim_drive(const Trial *t, const Map *map, const Config *cfg, Drive *d, Err *err)
{
    return build_drive(t, map, cfg, t->n, d, err);
}

int stim_drive_idle(const Config *cfg, int64_t n, Drive *d, Err *err)
{
    return build_drive(NULL, NULL, cfg, n, d, err);
}

/* ------------------------------------------------------------------ */
/* What the stimulus read, and its canonical form                      */
/* ------------------------------------------------------------------ */

int stim_note_files(Stim *st, const Trial *t)
{
    int k, j;
    for (k = 0; k < t->nfiles; k++) {
        char **np, **ns;
        int known = 0;
        for (j = 0; j < st->nused; j++)
            if (strcmp(st->used_path[j], t->file_path[k]) == 0 && strcmp(st->used_sha[j], t->file_sha[k]) == 0)
                known = 1;
        if (known) continue;
        np = (char **)realloc(st->used_path, ((size_t)st->nused + 1) * sizeof *np);
        if (!np) return -1;
        st->used_path = np;
        ns = (char **)realloc(st->used_sha, ((size_t)st->nused + 1) * sizeof *ns);
        if (!ns) return -1;
        st->used_sha = ns;
        st->used_path[st->nused] = xdup(t->file_path[k]);
        st->used_sha[st->nused] = xdup(t->file_sha[k]);
        if (!st->used_path[st->nused] || !st->used_sha[st->nused]) return -1;
        st->nused++;
    }
    return 0;
}

char *stim_canonical(const Stim *st)
{
    if (st->is_protocol) return NULL;
    if (sg_api_canon(st->text, st->waveform ? st->unit : "") != 0) return NULL;
    return xdup(sg_api_text());
}

/* ------------------------------------------------------------------ */
/* What was checked is what is played                                  */
/* ------------------------------------------------------------------ */

int stim_remember_digest(Stim *st, int i, const char *sha)
{
    if (i < 0 || i >= st->n || strlen(sha) >= 72) return -1;
    if (!st->digest) {
        st->digest = (char *)calloc((size_t)st->n, 72);
        if (!st->digest) return -1;
    }
    strcpy(st->digest + (size_t)i * 72, sha);
    st->have_digest = 1;
    return 0;
}

int stim_digest_matches(const Stim *st, int i, const char *sha)
{
    if (!st->have_digest || i < 0 || i >= st->n) return 0;
    return sha[0] != 0 && strcmp(st->digest + (size_t)i * 72, sha) == 0;
}
