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
 * unit.c -- unit tests for functions that are hard to reach from the command
 * line: the limit check of an output (finiteness, exact boundaries, the sign
 * of the calibration) and the exact rate fraction.
 *
 * Prints one line per failure and a summary; exit status 1 if any failed.
 * Not part of lcg2; built and run by `make test`.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "config.h"
#include "engine.h"
#include "sha256.h"
#include "stim.h"
#include "units.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* An output: gain 400 pA per volt in cc, limit 5 V unless limit < 0. */
static OutCh make(double limit, double off, double scl)
{
    OutCh o;
    memset(&o, 0, sizeof o);
    strcpy(o.name, "P1");
    o.gain[0] = 400; o.has_gain[0] = 1; strcpy(o.unit[0], "pA");
    o.has_limit = limit >= 0;
    o.limit = limit;
    o.cal_offset = off;
    o.cal_scale = scl;
    return o;
}

static char *dup(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

/*
 * SPEC 3.1: an error inside StimGen must come back as an error, never end the
 * program, because the outputs of a board may be live.  The library's die() calls
 * exit() unless a trap is set; the sg_api_* functions set one.  Prove it: open
 * stimuli that StimGen rejects in different ways, in turn with good ones, in one
 * process.  If any rejection exited, this test would stop here without its summary.
 */
static void test_error_trap(void)
{
    static Config c;
    Options opt;
    Stim st;
    Err e;
    int round;
    const char *bad[] = {
        "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s bogus(1) }\n",          /* unknown name  */
        "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc( }\n",                /* syntax        */
        "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s file(\"no_such_file.txt\") }\n",
        "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) }\nchannel P1 unit=pA { 1s dc(2) }\n",
        "sg 2 protocol\nsweep a = []\nstimulus {\n channel P1 unit=pA { 1s dc($a) }\n}\n",
        "",
        "\x01\x02 not a stimulus at all {{{",
    };
    const char *good = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) }\n";

    memset(&c, 0, sizeof c);
    memset(&opt, 0, sizeof opt);
    c.mode = MODE_CC;
    c.has_rate = 1; c.rate_p = 20000; c.rate_q = 1; strcpy(c.rate_text, "20kHz");
    c.n_out = 1;
    strcpy(c.out[0].name, "P1"); strcpy(c.out[0].line, "ao0");
    c.out[0].gain[0] = 400; c.out[0].has_gain[0] = 1; strcpy(c.out[0].unit[0], "pA");
    c.out[0].cal_scale = 1;

    for (round = 0; round < 3; round++) {
        size_t k;
        for (k = 0; k < sizeof bad / sizeof *bad; k++) {
            int rc = stim_open(&st, dup(bad[k]), "bad.sg", &opt, &c, &e);
            CHECK(rc == -1);
            CHECK(strlen(e.msg) > 0 && strncmp(e.msg, "sg:", 3) != 0);
            if (rc == 0) stim_free(&st);
            /* the library still works afterwards */
            rc = stim_open(&st, dup(good), "good.sg", &opt, &c, &e);
            CHECK(rc == 0);
            if (rc == 0) {
                Trial t;
                CHECK(stim_render(&st, 0, &t, &e) == 0 && t.n == 20000);
                trial_free(&t);
                stim_free(&st);
            }
        }
    }
}

/* Exact time arithmetic: the 128-bit intermediate, and no overflow where an int64 would. */
static void test_time_arithmetic(void)
{
    uint64_t q, r;
    int64_t ns, n;
    int exact;

    CHECK(units_muldiv(1ull << 63, 4, 1ull << 62, &q, &r) && q == 8 && r == 0);
    CHECK(units_muldiv(UINT64_MAX, UINT64_MAX, UINT64_MAX, &q, &r) && q == UINT64_MAX && r == 0);
    CHECK(units_muldiv(UINT64_MAX, 2, 3, &q, &r) && q == 12297829382473034410ull && r == 0);
    CHECK(units_muldiv(10, 10, 3, &q, &r) && q == 33 && r == 1);
    CHECK(units_muldiv(0, 12345, 7, &q, &r) && q == 0 && r == 0);
    CHECK(!units_muldiv(UINT64_MAX, UINT64_MAX, 1, &q, &r));          /* does not fit   */
    CHECK(!units_muldiv(1, 1, 0, &q, &r));                            /* divide by zero */
    CHECK(units_muldiv(123456789012345ull, 987654321ull, 1000003ull, &q, &r)
          && q == 121932265328031877ull && r == 497114ull);                                /* reference computed with Python */
    CHECK(!units_muldiv(123456789012345ull, 987654321098765ull, 1000003ull, &q, &r));  /* 1.2e23 does not fit */

    /* n samples to nanoseconds */
    CHECK(units_samples_to_ns(1, 20000, 1, &ns) && ns == 50000);
    CHECK(units_samples_to_ns(40000, 20000, 1, &ns) && ns == 2000000000);
    CHECK(units_samples_to_ns(1800000000, 500000, 1, &ns) && ns == 3600000000000);    /* an hour at 500 kS/s */
    CHECK(units_samples_to_ns(5, 5, 2, &ns) && ns == 2000000000);                      /* 2.5 Hz: 5 samples = 2 s */
    CHECK(units_samples_to_ns(1, 3, 1, &ns) && ns == 333333333);                       /* rounded down */
    CHECK(!units_samples_to_ns(-1, 20000, 1, &ns));
    CHECK(!units_samples_to_ns(1, 0, 1, &ns));
    CHECK(!units_samples_to_ns(INT64_MAX, 1, 1000000, &ns));                           /* would overflow */

    /* a duration to samples */
    CHECK(units_ps_to_samples(30000000000000ll, 20000, 1, &n, &exact) && n == 600000 && exact);
    CHECK(units_ps_to_samples(3600ll * 1000000000000ll, 500000, 1, &n, &exact) && n == 1800000000 && exact);
    CHECK(units_ps_to_samples(125000000ll, 20000, 1, &n, &exact) && n == 3 && !exact);   /* 125 us = 2.5 samples: half rounds up */
    CHECK(units_ps_to_samples(120000000ll, 20000, 1, &n, &exact) && n == 2 && !exact);   /* 2.4 samples */
    CHECK(units_ps_to_samples(1, 20000, 1, &n, &exact) && n == 0 && !exact);             /* shorter than one sample */
    CHECK(units_ps_to_samples(INT64_MAX, 5000000000000ll, 1, &n, &exact) == 0);         /* 4.6e19 samples: too many to count */

    /* durations that timing statements may use */
    CHECK(units_parse_duration_ex("0s", &ns, 1, 0) == 0 && ns == 0);
    CHECK(units_parse_duration_ex("0", &ns, 1, 1) == 0 && ns == 0);
    CHECK(units_parse_duration_ex("5", &ns, 1, 1) == 0 && ns == 5000000000000ll);
    CHECK(units_parse_duration_ex("5", &ns, 1, 0) == -1);              /* a bare number needs allow_bare */
    CHECK(units_parse_duration_ex("0s", &ns, 0, 0) == -1);             /* zero needs allow_zero */
    CHECK(units_parse_duration_ex("2.5s", &ns, 1, 1) == 0 && ns == 2500000000000ll);
}

/* The timing statements that StimGen writes into a protocol's trial list. */
static void test_timing_parser(void)
{
    Timing t;
    Err e;
    CHECK(engine_parse_timing("", "", &t, &e) == 0 && !t.is_period && t.ns == 0 && !t.on_trigger);   /* a plain stimulus */
    CHECK(engine_parse_timing("gap 0s", "immediately", &t, &e) == 0 && !t.is_period && t.ns == 0 && !t.on_trigger);
    CHECK(engine_parse_timing("period 5s", "immediately", &t, &e) == 0 && t.is_period && t.ns == 5000000000);
    CHECK(engine_parse_timing("period 2.5s", "", &t, &e) == 0 && t.is_period && t.ns == 2500000000);
    CHECK(engine_parse_timing("gap 500ms", "", &t, &e) == 0 && !t.is_period && t.ns == 500000000);
    CHECK(engine_parse_timing("gap 250us", "", &t, &e) == 0 && t.ns == 250000);
    CHECK(engine_parse_timing("period 1min", "", &t, &e) == 0 && t.ns == 60000000000);
    CHECK(engine_parse_timing("gap 0", "", &t, &e) == 0 && t.ns == 0);               /* a bare number is seconds */
    CHECK(engine_parse_timing("period 5", "", &t, &e) == 0 && t.ns == 5000000000);
    CHECK(engine_parse_timing("period 5s", "on trigger", &t, &e) == 0 && t.on_trigger);
    CHECK(engine_parse_timing("gap  3s", "", &t, &e) == 0 && t.ns == 3000000000);    /* two spaces */
    CHECK(engine_parse_timing("every 5s", "", &t, &e) == -1 && strstr(e.msg, "unknown timing") != NULL);
    CHECK(engine_parse_timing("period", "", &t, &e) == -1);
    CHECK(engine_parse_timing("period fast", "", &t, &e) == -1);
    CHECK(engine_parse_timing("period -5s", "", &t, &e) == -1);
    CHECK(engine_parse_timing("gap 0s", "when ready", &t, &e) == -1 && strstr(e.msg, "start rule") != NULL);
    CHECK(engine_parse_timing("gap 0s", "ON TRIGGER", &t, &e) == -1);
}

/* n bytes of 'x' hash to the given digest, in one go and in odd-sized pieces. */
static int sha_x(int n, const char *want)
{
    static char buf[256];
    char hex[65];
    LcgSha256 c;
    unsigned char d[32];
    int i, pos = 0, step = 1;
    memset(buf, 'x', sizeof buf);
    lcg_sha256_hex(buf, (size_t)n, hex);
    if (strcmp(hex, want) != 0) return 0;
    lcg_sha256_init(&c);
    while (pos < n) {                                  /* pieces of 1, 2, 3, ... bytes */
        int take = step < n - pos ? step : n - pos;
        lcg_sha256_update(&c, buf + pos, (size_t)take);
        pos += take; step++;
    }
    lcg_sha256_final(&c, d);
    for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return strcmp(hex, want) == 0;
}

static void test_sha256(void)
{
    char hex[65];
    static char million[1000000];
    int64_t size = 0;
    FILE *f;
    Err e;

    lcg_sha256_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    lcg_sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    lcg_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);   /* 448 bits */
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    lcg_sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
               112, hex);                                                               /* 896 bits */
    CHECK(strcmp(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") == 0);
    memset(million, 'a', sizeof million);
    lcg_sha256_hex(million, sizeof million, hex);                                           /* a million 'a' */
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    /* lengths around the padding boundaries, against an independent implementation */
    CHECK(sha_x(0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_x(1, "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"));
    CHECK(sha_x(54, "45f316e10b2c99abf374b22bda893cf3300d77263f1e272349ed414680522952"));
    CHECK(sha_x(55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"));
    CHECK(sha_x(56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"));
    CHECK(sha_x(57, "ae14a2563ccf969d99aca69ce6bb74981f734bbf9f655f73b8f06db68cab5217"));
    CHECK(sha_x(63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"));
    CHECK(sha_x(64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"));
    CHECK(sha_x(65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"));
    CHECK(sha_x(119, "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c"));
    CHECK(sha_x(120, "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98"));
    CHECK(sha_x(121, "79072a47bfaa54e6057a9ee21e0dea64b9edbfd160c4ccadbb657e64b6d22ee4"));
    CHECK(sha_x(128, "24da1b81d0b16df6428eee73c69fcb2a93c76bc6df706f0c6670fe6bfe800464"));

    /* a file */
    f = fopen("unit_sha256_probe.tmp", "wb");
    if (f) {
        fwrite("abc", 1, 3, f);
        fclose(f);
        CHECK(lcg_sha256_file("unit_sha256_probe.tmp", hex, &size, &e) == 0 && size == 3 &&
              strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
        remove("unit_sha256_probe.tmp");
    }
    CHECK(lcg_sha256_file("/nonexistent/probe", hex, &size, &e) == -1 && strstr(e.msg, "cannot read") != NULL);
}

int main(void)
{
    Err e;
    OutCh o;
    int64_t p, q;

    /* Exact boundaries: |wanted| <= limit passes, a hair above fails. */
    o = make(5, 0, 1);
    CHECK(out_check_value(&o, MODE_CC, 2000, &e) == 0);          /* exactly 5 V   */
    CHECK(out_check_value(&o, MODE_CC, -2000, &e) == 0);
    CHECK(out_check_value(&o, MODE_CC, 2000.0000001, &e) == -1);
    CHECK(strstr(e.msg, "above the limit of 5 V") != NULL);
    CHECK(out_check_value(&o, MODE_CC, -2001, &e) == -1);

    /* The board range, with no limit set. */
    o = make(-1, 0, 1);
    CHECK(out_check_value(&o, MODE_CC, 4000, &e) == 0);          /* exactly 10 V  */
    CHECK(out_check_value(&o, MODE_CC, 4000.001, &e) == -1);
    CHECK(strstr(e.msg, "above its range of 10 V") != NULL);

    /* The calibration enters after the limit: written = (wanted - off) / scale. */
    o = make(-1, 0, 0.99);
    CHECK(out_check_value(&o, MODE_CC, 3990, &e) == -1);         /* 9.975 / .99 > 10 */
    o = make(-1, 0, -1.0);                                        /* an inverting board */
    CHECK(out_check_value(&o, MODE_CC, 4000, &e) == 0);
    CHECK(out_check_value(&o, MODE_CC, -4000, &e) == 0);
    CHECK(out_check_value(&o, MODE_CC, 4001, &e) == -1);
    o = make(-1, 0.5, 1);                                         /* offset 0.5 V  */
    /* wanted = value/400, written = wanted - 0.5 */
    CHECK(out_check_value(&o, MODE_CC, -3800, &e) == 0);          /* -9.5 - .5 = -10 */
    CHECK(out_check_value(&o, MODE_CC, -3801, &e) == -1);         /* just below -10  */
    CHECK(out_check_value(&o, MODE_CC, 4200, &e) == 0);           /* 10.5 - .5 = 10  */
    CHECK(out_check_value(&o, MODE_CC, 4201, &e) == -1);

    /* Never silent about a value that is not a number. */
    o = make(5, 0, 1);
    CHECK(out_check_value(&o, MODE_CC, NAN, &e) == -1);
    CHECK(strstr(e.msg, "not a finite number") != NULL);
    CHECK(out_check_value(&o, MODE_CC, INFINITY, &e) == -1);
    CHECK(out_check_value(&o, MODE_CC, -INFINITY, &e) == -1);
    o = make(-1, 0, 1);
    CHECK(out_check_value(&o, MODE_CC, NAN, &e) == -1);
    o = make(-1, 0, 1e-320);                                      /* overflows to inf */
    CHECK(out_check_value(&o, MODE_CC, 1, &e) == -1);
    CHECK(strstr(e.msg, "finite voltage") != NULL);
    o = make(-1, 0, 1);
    o.gain[0] = 1e-320;                                           /* wanted overflows */
    CHECK(out_check_value(&o, MODE_CC, 1e10, &e) == -1);

    /* Exact rate fractions of what StimGen reports. */
    CHECK(stim_rate_fraction(20000, &p, &q) == 0 && p == 20000 && q == 1);
    CHECK(stim_rate_fraction(2.5, &p, &q) == 0 && p == 5 && q == 2);
    CHECK(stim_rate_fraction(0.001, &p, &q) == 0 && p == 1 && q == 1000);
    CHECK(stim_rate_fraction(1e6, &p, &q) == 0 && p == 1000000 && q == 1);
    CHECK(stim_rate_fraction(0.000001, &p, &q) == 0 && p == 1 && q == 1000000);
    CHECK(stim_rate_fraction(1e-7, &p, &q) == -1);
    CHECK(stim_rate_fraction(0.0, &p, &q) == -1);
    CHECK(stim_rate_fraction(-5, &p, &q) == -1);
    CHECK(stim_rate_fraction(NAN, &p, &q) == -1);
    CHECK(stim_rate_fraction(INFINITY, &p, &q) == -1);
    CHECK(stim_rate_fraction(1.0 / 3.0, &p, &q) == -1);           /* not decimal */

    test_sha256();
    test_error_trap();
    test_time_arithmetic();
    test_timing_parser();

    printf("unit tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
