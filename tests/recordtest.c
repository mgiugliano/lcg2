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
 * recordtest.c -- provokes failures of the HDF5 recorder (milestone M4).
 *
 *     recordtest DIR
 *
 * HDF5 prints a stack of its own messages on stderr when a call fails, unless told
 * not to; LCG2 tells it not to, and turns every failure into a message that begins
 * "lcg2:".  This driver makes three things fail on purpose and prints, on standard
 * output, the message of each; a test then checks that standard error stayed empty.
 *
 *   1  creating a data file that already exists
 *   2  beginning a trial whose group already exists
 *   3  an ordinary close afterwards (which must report no object left open)
 *
 * Not part of lcg2; built by `make tools`.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "record.h"

static Config cfg;

int main(int argc, char **argv)
{
    char path[400];
    Recorder *r = NULL;
    RecorderInit ri;
    Sink sink;
    TrialInfo ti;
    Err err;
    FILE *f;

    if (argc < 2) return 2;
    snprintf(path, sizeof path, "%s/existing.h5", argv[1]);
    memset(&cfg, 0, sizeof cfg);
    cfg.mode = MODE_CC;
    strcpy(cfg.device_name, "sim");
    memset(&ri, 0, sizeof ri);
    ri.path = path; ri.cfg = &cfg; ri.run_name = "x"; ri.config_text = "c"; ri.command_line = "x";
    ri.provenance_json = "{}"; ri.lcg2_version = "t"; ri.stimgen_commit = "t"; ri.stimgen_sources_sha256 = "t";
    ri.stimgen_library_version = "t"; ri.started_utc = "t";

    f = fopen(path, "wb");                              /* 1: the file is already there */
    if (f) { fputs("not an hdf5 file", f); fclose(f); }
    if (record_open(&r, &ri, &err) < 0) printf("1 %s\n", err.msg); else printf("1 NO ERROR\n");

    snprintf(path, sizeof path, "%s/fresh.h5", argv[1]);
    if (record_open(&r, &ri, &err) < 0) { printf("open failed: %s\n", err.msg); return 1; }
    record_sink(r, &sink);
    memset(&ti, 0, sizeof ti);
    ti.index = 0; ti.count = 1; ti.name = "x"; ti.variables = ""; ti.n = 10;
    ti.rate_p = 1; ti.rate_q = 1;
    if (sink.begin(sink.user, &ti, &err) < 0) { printf("first begin failed: %s\n", err.msg); return 1; }
    {
        TrialRecord rec;
        memset(&rec, 0, sizeof rec);
        strcpy(rec.start_utc, "t");
        if (sink.end(sink.user, &ti, &rec, &err) < 0) { printf("end failed: %s\n", err.msg); return 1; }
    }
    if (sink.begin(sink.user, &ti, &err) < 0) printf("2 %s\n", err.msg); else printf("2 NO ERROR\n");
    if (record_close(r, &err) < 0) printf("3 FAILED %s\n", err.msg); else printf("3 closed cleanly\n");
    return 0;
}
