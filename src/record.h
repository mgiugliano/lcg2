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
 * record.h -- the HDF5 data file (milestone M4).
 *
 * The Recorder is a Sink (see engine.h): the engine hands it the inputs of each
 * trial in blocks, in physical units, and it writes them into one HDF5 file per
 * run as they arrive.  This is the only module that uses HDF5, and only its
 * serial API, with calls that exist since HDF5 1.10.
 *
 * LAYOUT (read it with h5py, MATLAB, or h5dump; section 7 of the spec, and
 * docs/13-data-format.md, describe every name):
 *
 *   /                          attributes: format, lcg2_version, stimgen_commit,
 *                              stimgen_sources_sha256, stimgen_library_version,
 *                              run_name, started_utc, mode, device
 *   /stimulus_text             the stimulus as given (a string; absent for -t)
 *   /config                    the configuration as used, profile resolved
 *   /command_line              how LCG2 was started
 *   /provenance                a JSON record of what produced the run
 *   /trial_0000/               one group per trial, in the order played
 *       attributes: index, name, kind, seed, variables, rate_hz, rate_p, rate_q,
 *                   n_samples, scheduled_ns, start_ns, end_ns, delay_ns, latency_ns,
 *                   start_utc, virtual_time, recorded, incomplete
 *       ai/<name>              the recorded input, float64, physical units; attributes:
 *                              unit, line, gain, cal_scale, cal_offset, range_v,
 *                              terminal, mode, overrange_samples
 *       ao/<name>              the command that was played on an output the stimulus
 *                              names, in that output's unit (digital: 0/1, uint8);
 *                              attributes: unit, line, gain, limit_v, digital, rest
 *       markers                [name, sample] pairs, if the stimulus has markers
 *       info                   the protocol's own record of this trial (JSON), if any
 *
 * The file format is held between HDF5 1.8 and 1.10 (H5Pset_libver_bounds), whatever the
 * version of the library that writes it, so that older readers can open it (a file
 * written by HDF5 2.2 was opened by HDF5 1.10.7: see docs/13-data-format.md).
 *
 * The inputs are float64 and not compressed.  Outputs the stimulus does not name
 * are idle and have no dataset (their rest value is in the configuration).
 *
 * A TRIAL IS MARKED incomplete = 1 when it begins and set to 0 only when it ends
 * complete.  After an abort the file is closed in the normal way, the running
 * trial is kept with the samples it had, and the mark stays 1.  The file is
 * flushed after every trial: after a kill -9 the completed trials are normally
 * readable (HDF5 does not promise more).
 *
 * HDF5 never prints on its own here: its error reporting is switched off, and every
 * failure comes back as an Err that names the operation, the object and the file.
 */
#ifndef LCG2_RECORD_H
#define LCG2_RECORD_H

#include "config.h"
#include "engine.h"
#include "lcg2.h"

typedef struct Recorder Recorder;

typedef struct {
    const char   *path;                  /* the data file: created, and it must not exist */
    const Config *cfg;
    const char   *run_name;              /* the stem of the file names                     */
    const char   *config_text;           /* the resolved configuration                     */
    const char   *stimulus_text;         /* NULL for a record-only run                     */
    const char   *command_line;
    const char   *provenance_json;
    const char   *lcg2_version;
    const char   *stimgen_commit, *stimgen_sources_sha256, *stimgen_library_version;
    const char   *started_utc;
} RecorderInit;

/* Create the file and write what is known before the run.  Returns 0 or -1. */
int record_open(Recorder **out, const RecorderInit *init, Err *err);

/* The Sink that writes into it. */
void record_sink(Recorder *r, Sink *sink);

/* How many trials were begun. */
int record_trials(const Recorder *r);

/* Flush and close.  Always frees the Recorder.  Returns 0, or -1 if something could
 * not be written, or if an HDF5 object was left open. */
int record_close(Recorder *r, Err *err);

/* 1 if any HDF5 call failed during this process (see record.c). */
int record_had_error(void);

/* While a trial is open (between the sink's begin and end): a number attached to the dataset of active
 * input k (its order in TrialInfo.ai), such as the result of a membrane test.  Returns 0 or -1. */
int record_ai_attr(Recorder *r, int k, const char *name, double value, Err *err);

#endif /* LCG2_RECORD_H */
