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
 * archive.c -- the run folder.  See archive.h.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "platform.h"
#include "sha256.h"

/* ------------------------------------------------------------------ */
/* A growing string                                                    */
/* ------------------------------------------------------------------ */

static int sb_room(StrBuf *b, size_t more)
{
    if (b->failed) return 0;
    if (b->n + more + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char *g;
        while (cap < b->n + more + 1) cap *= 2;
        g = (char *)realloc(b->s, cap);
        if (!g) { b->failed = 1; return 0; }
        b->s = g;
        b->cap = cap;
    }
    return 1;
}

void sb_add(StrBuf *b, const char *text)
{
    size_t k = strlen(text);
    if (!sb_room(b, k)) return;
    memcpy(b->s + b->n, text, k + 1);
    b->n += k;
}

void sb_addf(StrBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) { b->failed = 1; return; }
    if ((size_t)k < sizeof tmp) { sb_add(b, tmp); return; }
    {                                              /* longer than the buffer: do it again at size */
        char *big = (char *)malloc((size_t)k + 1);
        if (!big) { b->failed = 1; return; }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)k + 1, fmt, ap);
        va_end(ap);
        sb_add(b, big);
        free(big);
    }
}

void sb_json(StrBuf *b, const char *text)
{
    sb_add(b, "\"");
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') { char t[3] = { '\\', (char)c, 0 }; sb_add(b, t); }
        else if (c == '\n') sb_add(b, "\\n");
        else if (c == '\r') sb_add(b, "\\r");
        else if (c == '\t') sb_add(b, "\\t");
        else if (c < 0x20) sb_addf(b, "\\u%04x", c);
        else { char t[2] = { (char)c, 0 }; sb_add(b, t); }
    }
    sb_add(b, "\"");
}

void sb_free(StrBuf *b)
{
    free(b->s);
    memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------------ */
/* The folder                                                          */
/* ------------------------------------------------------------------ */

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int archive_create(Archive *a, const char *prefix, Err *err)
{
    char stamp[20], base[160];
    int attempt;

    memset(a, 0, sizeof *a);
    platform_local_stamp(stamp);
    snprintf(base, sizeof base, "%s%s%s", prefix ? prefix : "", prefix ? "_" : "", stamp);

    /* Claim a name by making the folder: only one run can succeed in making it. */
    for (attempt = 1; attempt <= 99; attempt++) {
        int r;
        if (attempt == 1) snprintf(a->stem, sizeof a->stem, "%s", base);
        else snprintf(a->stem, sizeof a->stem, "%s-%d", base, attempt);
        snprintf(a->dir, sizeof a->dir, ".%s", a->stem);
        snprintf(a->h5, sizeof a->h5, "%s.h5", a->stem);
        if (exists(a->h5)) continue;                       /* a data file of that name is already there */
        r = platform_mkdir(a->dir);
        if (r == 0) break;
        if (r < 0)
            return err_set(err, "cannot create the run folder '%s': %s (is the current folder writable?)",
                           a->dir, strerror(errno));
    }
    if (attempt > 99)
        return err_set(err, "cannot find a free name for the run: %s.h5 and its -2 ... -99 are all taken", base);

    {
        char path[260];
        snprintf(path, sizeof path, "%s/log.txt", a->dir);
        a->log = fopen(path, "w");
        if (!a->log)
            return err_set(err, "cannot create '%s': %s", path, strerror(errno));
    }
    return 0;
}

static int remember(Archive *a, const char *path, const char *sha, int64_t size, Err *err)
{
    if (a->nitem >= ARCHIVE_MAX_ITEMS)
        return err_set(err, "the run folder would hold more than %d items", ARCHIVE_MAX_ITEMS);
    snprintf(a->item[a->nitem].path, sizeof a->item[a->nitem].path, "%s", path);
    snprintf(a->item[a->nitem].sha, sizeof a->item[a->nitem].sha, "%s", sha);
    a->item[a->nitem].size = size;
    a->nitem++;
    return 0;
}

int archive_put(Archive *a, const char *name, const void *data, size_t n, Err *err)
{
    char path[400], rel[400], hex[65];
    FILE *f;
    snprintf(rel, sizeof rel, "%s/%s", a->dir, name);
    snprintf(path, sizeof path, "%s", rel);
    f = fopen(path, "wb");
    if (!f) return err_set(err, "cannot create '%s': %s", path, strerror(errno));
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        return err_set(err, "cannot write '%s': %s", path, strerror(errno));
    }
    if (fclose(f) != 0) return err_set(err, "cannot finish writing '%s': %s", path, strerror(errno));
    lcg_sha256_hex(data, n, hex);
    return remember(a, rel, hex, (int64_t)n, err);
}

const char *archive_sha(const Archive *a, const char *name)
{
    char rel[400];
    int k;
    snprintf(rel, sizeof rel, "%s/%s", a->dir, name);
    for (k = 0; k < a->nitem; k++)
        if (strcmp(a->item[k].path, rel) == 0) return a->item[k].sha;
    return NULL;
}

/* A safe name for a copied file: its last component, with anything unusual replaced. */
static void safe_name(const char *src, char *out, size_t n)
{
    const char *base = src, *p;
    size_t k = 0;
    for (p = src; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (p = base; *p && k + 1 < n; p++)
        out[k++] = (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') ? *p : '_';
    out[k] = 0;
    if (k == 0) snprintf(out, n, "file");
    if (out[0] == '.') out[0] = '_';                        /* never a hidden or relative name */
}

int archive_copy(Archive *a, const char *src, const char *expect_sha, Err *err)
{
    char dirpath[260], base[120], name[160], rel[400], hex[65];
    FILE *in, *out;
    unsigned char buf[65536];
    size_t got;
    int64_t total = 0;
    int r;

    if (a->nfiles == 0) {
        snprintf(dirpath, sizeof dirpath, "%s/files", a->dir);
        r = platform_mkdir(dirpath);
        if (r < 0) return err_set(err, "cannot create '%s': %s", dirpath, strerror(errno));
    }
    safe_name(src, base, sizeof base);
    snprintf(name, sizeof name, "%02d_%s", a->nfiles + 1, base);
    snprintf(rel, sizeof rel, "%s/files/%s", a->dir, name);

    in = fopen(src, "rb");
    if (!in) return err_set(err, "cannot read '%s' to keep a copy: %s", src, strerror(errno));
    out = fopen(rel, "wb");
    if (!out) { fclose(in); return err_set(err, "cannot create '%s': %s", rel, strerror(errno)); }
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fclose(in); fclose(out);
            return err_set(err, "cannot write '%s': %s", rel, strerror(errno));
        }
        total += (int64_t)got;
    }
    fclose(in);
    if (fclose(out) != 0) return err_set(err, "cannot finish writing '%s': %s", rel, strerror(errno));
    if (lcg_sha256_file(rel, hex, NULL, err) < 0) return -1;
    if (expect_sha && strcmp(hex, expect_sha) != 0)
        return err_set(err, "'%s' changed while LCG2 was reading it: StimGen saw digest %s, the copy has %s",
                       src, expect_sha, hex);
    a->nfiles++;
    archive_log(a, "file  %s  copied to %s  sha256 %s", src, rel, hex);
    return remember(a, rel, hex, total, err);
}

/* ------------------------------------------------------------------ */
/* The files that `use` pulls in                                       */
/* ------------------------------------------------------------------ */

/* Is text[i..] the word w, as a whole word? */
static int word_at(const char *text, size_t i, const char *w)
{
    size_t k = strlen(w);
    if (strncmp(text + i, w, k) != 0) return 0;
    if (i > 0 && (isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_' || text[i - 1] == '$')) return 0;
    return !(isalnum((unsigned char)text[i + k]) || text[i + k] == '_');
}

/*
 * Call visit(path) for every  use "path"  or  stimulus "path"  in the text, skipping
 * comments (from # to the end of the line, outside a string).  It is a scan, not a
 * parse: it can miss a construct StimGen would accept, which is why canonical.sg is
 * the record that does not depend on it.
 */
static void scan_uses(const char *text, void (*visit)(void *, const char *), void *ctx)
{
    size_t i = 0;
    while (text[i]) {
        if (text[i] == '#') { while (text[i] && text[i] != '\n') i++; continue; }
        if (text[i] == '"') {                              /* a string that is not a use: skip it */
            i++;
            while (text[i] && text[i] != '"' && text[i] != '\n') i++;
            if (text[i] == '"') i++;
            continue;
        }
        if (word_at(text, i, "use") || word_at(text, i, "stimulus")) {
            size_t j = i + (text[i] == 'u' ? 3 : 8), s;
            while (text[j] == ' ' || text[j] == '\t') j++;
            if (text[j] == '"') {
                char path[320];
                size_t k = 0;
                s = ++j;
                while (text[j] && text[j] != '"' && text[j] != '\n' && k + 1 < sizeof path) path[k++] = text[j++];
                path[k] = 0;
                if (text[j] == '"' && k > 0) visit(ctx, path);
                i = j;
                continue;
            }
            (void)s;
        }
        i++;
    }
}

typedef struct { char path[16][320]; int n; } Found;

static void note(void *ctx, const char *path)
{
    Found *f = (Found *)ctx;
    if (f->n < 16) snprintf(f->path[f->n++], 320, "%s", path);
}

static int collect(Archive *a, const char *text, int depth, char done[][320], int *ndone, Err *err)
{
    Found f;
    int k, copied = 0;
    f.n = 0;
    scan_uses(text, note, &f);
    for (k = 0; k < f.n; k++) {
        int d, seen = 0;
        for (d = 0; d < *ndone; d++) if (strcmp(done[d], f.path[k]) == 0) seen = 1;
        if (seen) continue;
        if (*ndone >= 64 || depth >= 8) {
            archive_log(a, "note  use \"%s\": not followed (too many files or too deep)", f.path[k]);
            continue;
        }
        snprintf(done[(*ndone)++], 320, "%s", f.path[k]);
        {
            FILE *in = fopen(f.path[k], "rb");
            char *body;
            long len;
            if (!in) {
                archive_log(a, "note  use \"%s\": cannot be read here, not archived (canonical.sg still has its content)",
                            f.path[k]);
                continue;
            }
            fseek(in, 0, SEEK_END);
            len = ftell(in);
            fseek(in, 0, SEEK_SET);
            if (len < 0 || len > 16L * 1024 * 1024) { fclose(in); continue; }
            body = (char *)malloc((size_t)len + 1);
            if (!body) { fclose(in); return err_set(err, "out of memory"); }
            len = (long)fread(body, 1, (size_t)len, in);
            body[len] = 0;
            fclose(in);
            if (archive_copy(a, f.path[k], NULL, err) < 0) { free(body); return -1; }
            copied++;
            {
                int sub = collect(a, body, depth + 1, done, ndone, err);
                if (sub < 0) { free(body); return -1; }
                copied += sub;
            }
            free(body);
        }
    }
    return copied;
}

int archive_collect_uses(Archive *a, const char *text, Err *err)
{
    static char done[64][320];
    int ndone = 0;
    return collect(a, text, 0, done, &ndone, err);
}

/* ------------------------------------------------------------------ */
/* The log, the digests, the command line                              */
/* ------------------------------------------------------------------ */

void archive_log(Archive *a, const char *fmt, ...)
{
    va_list ap;
    if (!a->log) return;
    va_start(ap, fmt);
    vfprintf(a->log, fmt, ap);
    va_end(ap);
    fputc('\n', a->log);
    fflush(a->log);
}

int archive_finish(Archive *a, Err *err)
{
    char hex[65], path[260];
    int64_t size;
    FILE *f;
    int k, ok = 1;

    if (a->finished) return 0;
    a->finished = 1;
    if (lcg_sha256_file(a->h5, hex, &size, err) < 0) return -1;
    archive_log(a, "data  %s  %lld bytes  sha256 %s", a->h5, (long long)size, hex);
    /* the data file goes first in SHA256SUMS, then every item, paths relative to the working folder */
    snprintf(path, sizeof path, "%s/SHA256SUMS", a->dir);
    f = fopen(path, "w");
    if (!f) return err_set(err, "cannot create '%s': %s", path, strerror(errno));
    fprintf(f, "%s  %s\n", hex, a->h5);
    for (k = 0; k < a->nitem; k++) fprintf(f, "%s  %s\n", a->item[k].sha, a->item[k].path);
    if (fclose(f) != 0) ok = 0;
    if (!ok) return err_set(err, "cannot finish writing '%s': %s", path, strerror(errno));
    archive_log(a, "digests of %d items written to %s", a->nitem + 1, path);
    if (a->log) {
        if (fclose(a->log) != 0) { a->log = NULL; return err_set(err, "cannot finish writing the log: %s", strerror(errno)); }
        a->log = NULL;
    }
    return 0;
}

char *archive_quote_args(int argc, char **argv)
{
    StrBuf b;
    int i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < argc; i++) {
        const char *p = argv[i];
        int plain = *p != 0;
        const char *q;
        if (i) sb_add(&b, " ");
        for (q = p; *q; q++)
            if (!(isalnum((unsigned char)*q) || strchr("_@%+=:,./-", *q))) plain = 0;
        if (plain) { sb_add(&b, p); continue; }
        sb_add(&b, "'");
        for (q = p; *q; q++) {
            if (*q == '\'') sb_add(&b, "'\\''");
            else { char t[2] = { *q, 0 }; sb_add(&b, t); }
        }
        sb_add(&b, "'");
    }
    if (b.failed) { sb_free(&b); return NULL; }
    if (!b.s) { b.s = (char *)calloc(1, 1); }
    return b.s;
}
