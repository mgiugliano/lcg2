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
 * jsontool.c -- a test driver for the JSON reader.
 *
 *     jsontool < input.json
 *
 * Reads all of standard input, parses it with json.c and writes it back as
 * compact JSON.  A test compares that with what an independent parser makes of
 * the same input.  On a parse error: the message on standard error, exit 1.
 * Not part of lcg2; built by `make tools`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

static void put_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void put(const Json *j)
{
    size_t k;
    switch (j->type) {
    case J_NULL: fputs("null", stdout); break;
    case J_BOOL: fputs(j->boolean ? "true" : "false", stdout); break;
    case J_NUM:  printf("%.17g", j->num); break;
    case J_STR:  put_string(j->str); break;
    case J_ARR:
        putchar('[');
        for (k = 0; k < j->n; k++) { if (k) putchar(','); put(j->item[k]); }
        putchar(']');
        break;
    case J_OBJ:
        putchar('{');
        for (k = 0; k < j->n; k++) {
            if (k) putchar(',');
            put_string(j->key[k]);
            putchar(':');
            put(j->item[k]);
        }
        putchar('}');
        break;
    }
}

int main(void)
{
    size_t cap = 1 << 16, n = 0, got;
    char *buf = (char *)malloc(cap);
    Json *j;
    Err err;

    while ((got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
    }
    j = json_parse(buf, n, &err);
    if (!j) { fprintf(stderr, "%s\n", err.msg); return 1; }
    put(j);
    putchar('\n');
    json_free(j);
    free(buf);
    return 0;
}
