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
 * runtest.c -- a test driver for a whole recorded run (milestone M4).
 *
 *     runtest [driver options] -- [lcg2 options] [stimulus]
 *
 * It does exactly what lcg2 does after the command line (the same session, the same
 * run_execute()), but with the simulator settings of its own, so that a test can ask
 * for what lcg2 deliberately has no switch for:
 *
 *   --noise SD      volts of noise on every input (0 gives exact, checkable values)
 *   --latency NS    a start latency
 *   --seed N        the noise seed
 *   --fail-rest K   the simulator fails its K-th and later set_rest calls
 *
 * The exit status is lcg2's.  Not part of lcg2; built by `make tools`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "run.h"
#include "session.h"

static Session session;

int main(int argc, char **argv)
{
    RunParams rp;
    Err err;
    int dd, rc, n;
    char **args;
    double noise = -1;
    long long latency = 0, seed = 1;
    int fail_rest = 0;

    err.msg[0] = 0;
    for (dd = 1; dd < argc && strcmp(argv[dd], "--") != 0; dd++) {
        if (!strcmp(argv[dd], "--noise") && dd + 1 < argc) noise = atof(argv[++dd]);
        else if (!strcmp(argv[dd], "--latency") && dd + 1 < argc) latency = atoll(argv[++dd]);
        else if (!strcmp(argv[dd], "--seed") && dd + 1 < argc) seed = atoll(argv[++dd]);
        else if (!strcmp(argv[dd], "--fail-rest") && dd + 1 < argc) fail_rest = atoi(argv[++dd]);
        else { fprintf(stderr, "runtest: unknown driver option %s\n", argv[dd]); return 2; }
    }
    if (dd >= argc) { fprintf(stderr, "usage: runtest [options] -- [lcg2 options]\n"); return 2; }

    /* the command line the run will record: "lcg2" and everything after the "--" */
    n = argc - dd;
    args = (char **)malloc((size_t)(n + 1) * sizeof *args);
    memcpy(args + 1, argv + dd + 1, (size_t)(n - 1) * sizeof *args);
    args[0] = "lcg2";
    args[n] = NULL;

    rc = session_prepare(&session, n, args, &err);
    if (rc < 0) { fprintf(stderr, "lcg2: %s\n", err.msg); return 1; }
    if (rc == 1) return 0;
    if (session.opt.dry) { session_print_plan(&session); return 0; }

    run_params_default(&rp, &session);
    if (noise >= 0) rp.sim.noise_sd = noise;
    rp.sim.latency_ns = latency;
    rp.sim.seed = (uint64_t)seed;
    rp.sim.fail_rest_from = fail_rest;
    rc = run_execute(&rp, &err);
    if (rc != 0 && err.msg[0]) fprintf(stderr, "lcg2: %s\n", err.msg);
    return rc;
}
