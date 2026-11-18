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
 * backend_neuron.h -- the NEURON simulator as a backend (device = neuron, milestone M3b).
 *
 * LCG2 starts the adapter (neuron/lcg2_neuron.py) once, in a Python that has NEURON, and talks to it
 * through two pipes.  The adapter builds the virtual cells that the [cell] and [synapse] sections
 * describe; this file plays the trial into them and brings back what the electrodes measured.
 *
 * Where the boundary is.  The board is replaced from the volts inwards: LCG2 turns the volts of the
 * Drive back into the command the amplifier would have produced (inverting the output's calibration and
 * gain: pA or mV), the cells answer in pA or mV, and those are turned into the volts the board
 * would have read (inverting the input's gain and calibration, clamped to the input range and counted).
 * Everything above the backend, the engine, the recording and the archive, is therefore exactly what
 * a run on the board does, and the calibration, the gains and the range are exercised.
 *
 * Time is virtual or real as for every backend: the cells are integrated block by block, and the
 * clock decides when a block is delivered.  Between trials the cells are advanced through the pause at
 * the rest values, so that they keep their state (the pause is the time the clock shows between the end
 * of a trial and the start of the next).
 */
#ifndef LCG2_BACKEND_NEURON_H
#define LCG2_BACKEND_NEURON_H

#include "backend.h"

typedef struct {
    const char *python;          /* the interpreter that has NEURON            */
    const char *script;          /* the adapter                                */
    const char *stderr_path;     /* where the adapter's messages go            */
    int         check_only;      /* build the network, report, stop (for -n)   */
    int         init_timeout_s;  /* building can include compiling .mod files  */
    int         block_timeout_s; /* longest wait for one block of 0.1 s        */
} NeuronOpts;

/* The adapter to run for cfg: [rig] sim_script (a relative path starts in the folder of the configuration
 * file), or the one that ships with LCG2 (its place is baked in by the Makefile). */
void neuron_script_path(const Config *cfg, char *out, size_t n);

Backend *neuron_backend_new(const NeuronOpts *opts);
void     neuron_backend_free(Backend *be);

/* After open(): the adapter's description of what it built, as a JSON line (NULL before open). */
const char *neuron_backend_description(const Backend *be);

/* Start the adapter, build the network of cfg, print one line per cell and synapse to stdout, stop.
 * This is what `lcg2 -n` runs for device = neuron.  Returns 0, or -1 with a message. */
int neuron_check(const NeuronOpts *opts, const Config *cfg, Err *err);

#endif /* LCG2_BACKEND_NEURON_H */
