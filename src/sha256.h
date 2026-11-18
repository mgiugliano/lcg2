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
 * sha256.h -- SHA-256 (FIPS 180-4), written out so that LCG2 needs no library
 * for it.  The run archive stores the digest of everything that went into a run
 * and of the data it made, so that a result can be checked later.
 *
 * The functions carry the prefix lcg_ on purpose: StimGen's library, which is linked
 * into the same program, exports a sha256_hex of its own, and two functions of one
 * name would not link.
 *
 * Incremental use:
 *     LcgSha256 c;  lcg_sha256_init(&c);  lcg_sha256_update(&c, data, n); ...
 *     unsigned char d[32];  lcg_sha256_final(&c, d);
 * Or in one go: lcg_sha256_hex(data, n, hex) and lcg_sha256_file(path, hex, &size, err).
 */
#ifndef LCG2_SHA256_H
#define LCG2_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include "lcg2.h"

typedef struct {
    uint32_t      h[8];          /* the running state                         */
    uint64_t      bytes;         /* how many bytes were fed in                */
    unsigned char block[64];     /* the partial block                         */
    size_t        used;          /* bytes in it                               */
} LcgSha256;

void lcg_sha256_init(LcgSha256 *c);
void lcg_sha256_update(LcgSha256 *c, const void *data, size_t n);
void lcg_sha256_final(LcgSha256 *c, unsigned char digest[32]);

/* The digest of a block of bytes as 64 lower-case hex digits and a NUL. */
void lcg_sha256_hex(const void *data, size_t n, char hex[65]);

/* The digest of a whole file, and its size.  Returns 0, or -1 with a message. */
int lcg_sha256_file(const char *path, char hex[65], int64_t *size, Err *err);

#endif /* LCG2_SHA256_H */
