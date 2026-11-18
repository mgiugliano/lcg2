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
 * json.h -- a small JSON reader.
 *
 * StimGen 2 hands LCG2 two kinds of JSON: the header of the .sgb file that
 * holds the rendered samples, and the list of trials of a protocol.  This
 * module reads such text into a tree that the caller can walk, and nothing
 * more: no writer, no streaming, no comments.  It accepts standard JSON
 * (RFC 8259) and refuses everything else, with the byte offset of the problem.
 *
 * Every node remembers where its text began and ended in the source
 * (begin, end), because LCG2 sometimes has to hand a piece of the input back
 * to StimGen unchanged: the "info" object of a trial goes back as the
 * provenance record of the .sgb file.
 *
 * Limits: nesting at most JSON_MAX_DEPTH levels (arrays and objects inside
 * arrays and objects), so a hostile or damaged file cannot overflow the C stack.
 * Numbers are read as doubles: an integer above 2^53 loses its low digits (which
 * is why StimGen writes its 64-bit seeds as strings).
 */
#ifndef LCG2_JSON_H
#define LCG2_JSON_H

#include <stddef.h>
#include "lcg2.h"

#define JSON_MAX_DEPTH 64

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct Json {
    JType        type;
    double       num;          /* J_NUM                                       */
    int          boolean;      /* J_BOOL                                      */
    char        *str;          /* J_STR: UTF-8, NUL-terminated, owned          */
    size_t       n;            /* J_ARR / J_OBJ: number of items              */
    struct Json **item;        /* J_ARR: the items; J_OBJ: the values         */
    char       **key;          /* J_OBJ: the keys, owned                      */
    size_t       begin, end;   /* the node's text is src[begin .. end)        */
} Json;

/* Read len bytes of text.  On success return the tree (free it with
 * json_free); on failure return NULL and fill err. */
Json *json_parse(const char *text, size_t len, Err *err);

void json_free(Json *j);

/* Value of a key of an object, or NULL if j is not an object or the key is
 * absent (the first one, if it is repeated). */
const Json *json_get(const Json *obj, const char *key);

/* Item i of an array, or NULL. */
const Json *json_at(const Json *arr, size_t i);

/* The string of a node, or NULL if it is not a string. */
const char *json_str(const Json *j);

/* Convenience for values with a known type: return the number or the string,
 * or the fallback when the key is absent or has another type. */
double json_num_or(const Json *obj, const char *key, double fallback);
const char *json_str_or(const Json *obj, const char *key, const char *fallback);

#endif /* LCG2_JSON_H */
