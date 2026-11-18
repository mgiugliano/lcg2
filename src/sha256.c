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
 * sha256.c -- SHA-256 (FIPS 180-4).  See sha256.h.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* One 64-byte block into the state. */
static void compress(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void lcg_sha256_init(LcgSha256 *c)
{
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, init, sizeof init);
    c->bytes = 0;
    c->used = 0;
}

void lcg_sha256_update(LcgSha256 *c, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    c->bytes += n;
    if (c->used) {                                  /* top up the partial block first */
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->block + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used < 64) return;
        compress(c->h, c->block);
        c->used = 0;
    }
    for (; n >= 64; n -= 64, p += 64) compress(c->h, p);
    if (n) { memcpy(c->block, p, n); c->used = n; }
}

void lcg_sha256_final(LcgSha256 *c, unsigned char digest[32])
{
    uint64_t bits = c->bytes * 8;
    unsigned char pad[72];
    size_t padlen = (c->used < 56) ? 56 - c->used : 120 - c->used;
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + (size_t)i] = (unsigned char)(bits >> (56 - 8 * i));
    lcg_sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        digest[4 * i]     = (unsigned char)(c->h[i] >> 24);
        digest[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        digest[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        digest[4 * i + 3] = (unsigned char)c->h[i];
    }
}

static void to_hex(const unsigned char d[32], char hex[65])
{
    static const char dig[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { hex[2 * i] = dig[d[i] >> 4]; hex[2 * i + 1] = dig[d[i] & 15]; }
    hex[64] = 0;
}

void lcg_sha256_hex(const void *data, size_t n, char hex[65])
{
    LcgSha256 c;
    unsigned char d[32];
    lcg_sha256_init(&c);
    lcg_sha256_update(&c, data, n);
    lcg_sha256_final(&c, d);
    to_hex(d, hex);
}

int lcg_sha256_file(const char *path, char hex[65], int64_t *size, Err *err)
{
    FILE *f = fopen(path, "rb");
    LcgSha256 c;
    unsigned char buf[65536], d[32];
    size_t got;
    int64_t total = 0;

    if (!f) return err_set(err, "cannot read '%s' to compute its digest: %s", path, strerror(errno));
    lcg_sha256_init(&c);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) { lcg_sha256_update(&c, buf, got); total += (int64_t)got; }
    if (ferror(f)) {
        fclose(f);
        return err_set(err, "error reading '%s': %s", path, strerror(errno));
    }
    fclose(f);
    lcg_sha256_final(&c, d);
    to_hex(d, hex);
    if (size) *size = total;
    return 0;
}
