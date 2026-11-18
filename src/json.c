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
 * json.c -- a small JSON reader.  See json.h.
 *
 * A recursive-descent parser over a byte range.  It never reads past the end
 * of the range, never recurses more than JSON_MAX_DEPTH levels, and releases
 * everything it allocated if it fails half way.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
    const char *s;     /* the text                              */
    size_t      n;     /* its length                            */
    size_t      i;     /* the position                          */
    Err        *err;
    int         failed;
} P;

static void *oom(P *p)
{
    if (!p->failed) { err_set(p->err, "JSON: out of memory"); p->failed = 1; }
    return NULL;
}

static void *bad(P *p, const char *what)
{
    if (!p->failed) {
        err_set(p->err, "JSON error at byte %lu: %s", (unsigned long)p->i, what);
        p->failed = 1;
    }
    return NULL;
}

static void skip_ws(P *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' ||
                           p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

void json_free(Json *j)
{
    size_t k;
    if (!j) return;
    free(j->str);
    for (k = 0; k < j->n; k++) {
        json_free(j->item[k]);
        if (j->key) free(j->key[k]);
    }
    free(j->item);
    free(j->key);
    free(j);
}

/* Four hex digits at p->i; the value or -1. */
static long hex4(P *p)
{
    long v = 0;
    int k;
    if (p->i + 4 > p->n) return -1;
    for (k = 0; k < 4; k++) {
        char c = p->s[p->i + (size_t)k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    p->i += 4;
    return v;
}

/* Append a code point as UTF-8 to buf (room for 4 bytes is guaranteed). */
static size_t utf8(char *buf, unsigned long c)
{
    if (c < 0x80) { buf[0] = (char)c; return 1; }
    if (c < 0x800) { buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        buf[0] = (char)(0xE0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    buf[0] = (char)(0xF0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* A string, p->i at the opening quote.  Returns a malloc'd C string. */
static char *parse_string(P *p)
{
    size_t cap = 16, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return oom(p);
    p->i++;                                           /* the opening quote */
    for (;;) {
        unsigned char c;
        char tmp[4];
        size_t k, m;
        if (p->i >= p->n) { free(out); return bad(p, "unterminated string"); }
        c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c < 0x20) { free(out); p->i--; return bad(p, "control character in a string"); }
        if (c == '\\') {
            unsigned long cp;
            if (p->i >= p->n) { free(out); return bad(p, "unterminated escape"); }
            c = (unsigned char)p->s[p->i++];
            switch (c) {
            case '"': case '\\': case '/': tmp[0] = (char)c; m = 1; break;
            case 'b': tmp[0] = '\b'; m = 1; break;
            case 'f': tmp[0] = '\f'; m = 1; break;
            case 'n': tmp[0] = '\n'; m = 1; break;
            case 'r': tmp[0] = '\r'; m = 1; break;
            case 't': tmp[0] = '\t'; m = 1; break;
            case 'u': {
                long h = hex4(p);
                if (h < 0) { free(out); return bad(p, "bad \\u escape"); }
                cp = (unsigned long)h;
                if (cp >= 0xD800 && cp < 0xDC00) {      /* a surrogate pair */
                    long lo;
                    if (p->i + 2 > p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') {
                        free(out); return bad(p, "lone surrogate");
                    }
                    p->i += 2;
                    lo = hex4(p);
                    if (lo < 0xDC00 || lo > 0xDFFF) { free(out); return bad(p, "bad surrogate pair"); }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned long)lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    free(out); return bad(p, "lone surrogate");
                }
                m = utf8(tmp, cp);
                break;
            }
            default: free(out); p->i--; return bad(p, "unknown escape");
            }
        } else {
            tmp[0] = (char)c;
            m = 1;
        }
        if (len + m + 1 > cap) {
            char *g;
            cap *= 2;
            g = (char *)realloc(out, cap);
            if (!g) { free(out); return oom(p); }
            out = g;
        }
        for (k = 0; k < m; k++) out[len++] = tmp[k];
    }
    out[len] = 0;
    return out;
}

static Json *parse_value(P *p, int depth);

static Json *new_node(P *p, JType t)
{
    Json *j = (Json *)calloc(1, sizeof *j);
    if (!j) return (Json *)oom(p);
    j->type = t;
    j->begin = p->i;
    return j;
}

/* Grow the item (and key) arrays of j by one. */
static int grow(P *p, Json *j, int with_key)
{
    Json **ni = (Json **)realloc(j->item, (j->n + 1) * sizeof *ni);
    if (!ni) { oom(p); return 0; }
    j->item = ni;
    if (with_key) {
        char **nk = (char **)realloc(j->key, (j->n + 1) * sizeof *nk);
        if (!nk) { oom(p); return 0; }
        j->key = nk;
    }
    return 1;
}

static Json *parse_array(P *p, int depth)
{
    Json *j = new_node(p, J_ARR);
    if (!j) return NULL;
    p->i++;                                           /* [ */
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; j->end = p->i; return j; }
    for (;;) {
        Json *v;
        skip_ws(p);
        v = parse_value(p, depth + 1);
        if (!v) { json_free(j); return NULL; }
        if (!grow(p, j, 0)) { json_free(v); json_free(j); return NULL; }
        j->item[j->n++] = v;
        skip_ws(p);
        if (p->i >= p->n) { json_free(j); return (Json *)bad(p, "unterminated array"); }
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] == ']') { p->i++; break; }
        json_free(j);
        return (Json *)bad(p, "expected ',' or ']'");
    }
    j->end = p->i;
    return j;
}

static Json *parse_object(P *p, int depth)
{
    Json *j = new_node(p, J_OBJ);
    if (!j) return NULL;
    p->i++;                                           /* { */
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; j->end = p->i; return j; }
    for (;;) {
        char *k;
        Json *v;
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != '"') { json_free(j); return (Json *)bad(p, "expected a key"); }
        k = parse_string(p);
        if (!k) { json_free(j); return NULL; }
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') {
            free(k); json_free(j); return (Json *)bad(p, "expected ':'");
        }
        p->i++;
        skip_ws(p);
        v = parse_value(p, depth + 1);
        if (!v) { free(k); json_free(j); return NULL; }
        if (!grow(p, j, 1)) { free(k); json_free(v); json_free(j); return NULL; }
        j->key[j->n] = k;
        j->item[j->n++] = v;
        skip_ws(p);
        if (p->i >= p->n) { json_free(j); return (Json *)bad(p, "unterminated object"); }
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] == '}') { p->i++; break; }
        json_free(j);
        return (Json *)bad(p, "expected ',' or '}'");
    }
    j->end = p->i;
    return j;
}

/* A number as RFC 8259 writes it: -? int frac? exp?  (no leading zeros). */
static Json *parse_number(P *p)
{
    size_t b = p->i;
    char tmp[64], *endp;
    Json *j;
    size_t len;

    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i >= p->n) return (Json *)bad(p, "bad number");
    if (p->s[p->i] == '0') p->i++;
    else if (p->s[p->i] >= '1' && p->s[p->i] <= '9')
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    else return (Json *)bad(p, "bad number");
    if (p->i < p->n && p->s[p->i] == '.') {
        size_t d = ++p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == d) return (Json *)bad(p, "digits expected after '.'");
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        size_t d;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == d) return (Json *)bad(p, "digits expected in the exponent");
    }
    len = p->i - b;
    if (len >= sizeof tmp) return (Json *)bad(p, "number is too long");
    memcpy(tmp, p->s + b, len);
    tmp[len] = 0;
    p->i = b;
    j = new_node(p, J_NUM);
    if (!j) return NULL;
    j->num = strtod(tmp, &endp);
    if (!isfinite(j->num)) { free(j); return (Json *)bad(p, "number out of range"); }
    p->i = b + len;
    j->end = p->i;
    return j;
}

static Json *parse_value(P *p, int depth)
{
    Json *j;
    if (depth >= JSON_MAX_DEPTH) return (Json *)bad(p, "nested too deeply");
    skip_ws(p);
    if (p->i >= p->n) return (Json *)bad(p, "unexpected end of the text");
    switch (p->s[p->i]) {
    case '{': return parse_object(p, depth);
    case '[': return parse_array(p, depth);
    case '"':
        j = new_node(p, J_STR);
        if (!j) return NULL;
        j->str = parse_string(p);
        if (!j->str) { free(j); return NULL; }
        j->end = p->i;
        return j;
    case 't': case 'f': case 'n': {
        static const struct { const char *w; JType t; int b; } words[] = {
            { "true", J_BOOL, 1 }, { "false", J_BOOL, 0 }, { "null", J_NULL, 0 } };
        int k;
        for (k = 0; k < 3; k++) {
            size_t l = strlen(words[k].w);
            if (p->i + l <= p->n && memcmp(p->s + p->i, words[k].w, l) == 0) {
                j = new_node(p, words[k].t);
                if (!j) return NULL;
                j->boolean = words[k].b;
                p->i += l;
                j->end = p->i;
                return j;
            }
        }
        return (Json *)bad(p, "unknown word");
    }
    default:
        if (p->s[p->i] == '-' || (p->s[p->i] >= '0' && p->s[p->i] <= '9'))
            return parse_number(p);
        return (Json *)bad(p, "unexpected character");
    }
}

Json *json_parse(const char *text, size_t len, Err *err)
{
    P p;
    Json *j;
    p.s = text; p.n = len; p.i = 0; p.err = err; p.failed = 0;
    j = parse_value(&p, 0);
    if (!j) return NULL;
    skip_ws(&p);
    if (p.i != p.n) {
        json_free(j);
        bad(&p, "unexpected text after the value");
        return NULL;
    }
    return j;
}

const Json *json_get(const Json *obj, const char *key)
{
    size_t k;
    if (!obj || obj->type != J_OBJ) return NULL;
    for (k = 0; k < obj->n; k++)
        if (strcmp(obj->key[k], key) == 0) return obj->item[k];
    return NULL;
}

const Json *json_at(const Json *arr, size_t i)
{
    return (arr && arr->type == J_ARR && i < arr->n) ? arr->item[i] : NULL;
}

const char *json_str(const Json *j)
{
    return (j && j->type == J_STR) ? j->str : NULL;
}

double json_num_or(const Json *obj, const char *key, double fallback)
{
    const Json *v = json_get(obj, key);
    return (v && v->type == J_NUM) ? v->num : fallback;
}

const char *json_str_or(const Json *obj, const char *key, const char *fallback)
{
    const char *s = json_str(json_get(obj, key));
    return s ? s : fallback;
}
