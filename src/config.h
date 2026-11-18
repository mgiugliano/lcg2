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
 * config.h -- the configuration file: reading, rig profiles, validation.
 *
 * Reading happens in three steps, each a function below:
 *
 *   1. config_load()          INI file -> Table, a flat list of
 *                             (section, key, value, line) entries, kept in
 *                             file order.  Only the syntax is checked.
 *   2. config_apply_profile() the entries of [profile NAME] overwrite entries
 *                             of other sections ("out.P2.line = ao1").  The
 *                             Table now is the "resolved" configuration that
 *                             the run archive will store (config_dump()).
 *   3. config_build()         Table -> Config, typed and validated.  Every key
 *                             is checked: name known, value well formed,
 *                             units of the right dimension, lines legal for
 *                             the board, no two channels on one line.
 *
 * Why a flat table first?  A profile has to patch single values, and the run
 * archive has to store exactly what was used.  Both are trivial on a list of
 * text entries and awkward on typed structures.
 *
 * File format (INI, one section per piece of hardware):
 *
 *   [rig]                     device = Dev1 | sim | neuron
 *                             mode = cc | vc          (or -m on the command line)
 *                             rate = 20kHz            (optional)
 *                             trigger = PFI0          (optional, see SPEC 3.16)
 *                             sim_script, sim_python, sim_dt, sim_celsius,
 *                             sim_v_init, sim_settle  (device = neuron only)
 *   [out P1]                  one analog or digital output, named like the
 *                             stimulus channel it carries
 *   [in V1]                   one analog input, recorded in every trial
 *   [cell P1], [synapse A_B]  virtual preparation (device = neuron): the cell
 *                             that every pipette P1 is attached to, and the
 *                             synapses between cells (SPEC 4e)
 *   [profile NAME]            overrides, selected with -P NAME
 *
 * Gain convention: a gain is "channel units per volt".  An output with
 * gain_cc = 400 and unit_cc = pA turns 400 pA into 1 V at the board.  An
 * input with gain_cc = 100 and unit_cc = mV turns 1 V at the board into
 * 100 mV.  So one number per channel and mode, the same for both directions.
 *
 * Calibration: cal_offset (volts, default 0) and cal_scale (default 1) correct
 * the board against a bench measurement: v_true = cal_scale * v_nominal +
 * cal_offset.  For an input LCG2 applies this to the volts it read; for an
 * output it applies the inverse to the volts it wants.  (Used from M2 on.)
 * The names and defaults come from Geko's geko.ini; Geko itself never applies
 * them (its code does not read the keys), so this formula is LCG2's own and
 * is yet to be confirmed by the owner.
 *
 * Two limits guard an output, in this order (see out_wanted_volts() below):
 *   1. |wanted| <= limit      wanted = value / gain, the volts that the
 *                             amplifier's command input is meant to receive;
 *                             the "limit" key protects the amplifier.
 *   2. |written| <= 10 V      written = (wanted - cal_offset) / cal_scale, the
 *                             volts the board is asked for; 10 V is the
 *                             board's output range.
 * One pair of helpers does both, and is used for the idle (rest) values at
 * load time and, in M2, for every sample of every trial, so the checks
 * cannot differ.
 */
#ifndef LCG2_CONFIG_H
#define LCG2_CONFIG_H

#include <stdint.h>
#include <stdio.h>
#include "lcg2.h"
#include "units.h"

#define CFG_MAX_ENTRIES 512      /* entries (keys and section markers)    */
#define CFG_NAME        48       /* section and key names, with the NUL   */
#define CFG_VAL         400      /* one value, with the NUL               */
#define CFG_MAX_OUT     16       /* 4 analog + 8 digital lines, with room */
#define CFG_MAX_IN      16       /* the board has 16 analog inputs        */
#define CFG_MAX_CELLS   16       /* virtual cells                         */
#define CFG_PREP        16384    /* text of the [cell]/[synapse] sections */

/* ------------------------------------------------------------------ */
/* Step 1 and 2: the flat table                                        */
/* ------------------------------------------------------------------ */

/* One line of the file.  A section header is an entry with an empty key. */
typedef struct {
    char sect[CFG_NAME];         /* "rig", "out P1", "profile rigB", ...  */
    char key[CFG_NAME];          /* "" for a section header               */
    char val[CFG_VAL];
    int  line;                   /* line in the file (of the profile for an
                                    overridden value)                     */
} Entry;

typedef struct {
    char  path[512];             /* the file, for messages                */
    int   n;
    Entry e[CFG_MAX_ENTRIES];
    char  profile[CFG_NAME];     /* profile applied, "" if none           */
    Err   pending;               /* message left by the INI handler       */
    int   has_pending;
} Table;

/* Read and syntax-check the file.  Checks: sections are rig, out, in, cell,
 * synapse or profile; names are identifiers; no duplicate section or key; no
 * key outside a section; lines are shorter than 1000 characters. */
int config_load(Table *t, const char *path, Err *err);

/* Apply [profile NAME].  Every key of the profile is "rig.KEY" or
 * "KIND.NAME.KEY" and must name an existing section; it replaces the value of
 * that key, or adds the key to that section.  An unknown profile is an error
 * that lists the profiles the file has. */
int config_apply_profile(Table *t, const char *name, Err *err);

/* Set sect/key to val in the table, as if the file said so, and mark the
 * entry as set on the command line (line 0).  Used for -m, so that the
 * resolved configuration shows the mode that was really in force. */
int config_override(Table *t, const char *sect, const char *key, const char *val, Err *err);

/* Write the table as INI text (sections in file order, profile sections
 * last, a comment line saying which profile was applied). */
void config_dump(const Table *t, FILE *f);

/* ------------------------------------------------------------------ */
/* Step 3: the typed configuration                                     */
/* ------------------------------------------------------------------ */

typedef enum { MODE_NONE = 0, MODE_CC = 1, MODE_VC = 2 } Mode;
typedef enum { DEV_SIM, DEV_NEURON, DEV_DAQMX } DevKind;

/* Index into the two-element per-mode arrays below: cc -> 0, vc -> 1. */
#define MODE_INDEX(m) ((int)(m) - 1)

typedef struct {
    char   name[CFG_NAME];       /* the stimulus channel it carries       */
    int    skip;                 /* skip = yes: not used in this run      */
    int    digital;              /* 1: a clocked port-0 line              */
    int    index;                /* analog: AO 0..3; digital: line 0..7   */
    char   line[24];             /* as written: "ao0" or "port0/line3"    */
    int    has_gain[2];          /* per mode (0 = cc, 1 = vc)             */
    double gain[2];              /* channel units per volt                */
    char   unit[2][8];           /* channel unit, e.g. "pA"               */
    double cal_offset, cal_scale;
    int    has_limit;
    double limit;                /* volts, safety clamp (|v| <= limit)    */
    double rest;                 /* idle value, in the unit of the mode   */
} OutCh;

typedef struct {
    char   name[CFG_NAME];
    int    skip;
    int    index;                /* AI 0..15                              */
    char   line[8];              /* "ai0"                                 */
    double range;                /* volts, one of 0.2 1 5 10              */
    char   terminal[8];          /* "RSE", "NRSE" or "DIFF"               */
    int    has_gain[2];
    double gain[2];
    char   unit[2][8];
    double cal_offset, cal_scale;
} InCh;

typedef struct {
    DevKind device;
    char    device_name[32];     /* "sim", "neuron" or a DAQmx name       */
    Mode    mode;                /* the mode in force for this run        */
    int     has_rate;            /* a rate in [rig] (after any profile)   */
    int64_t rate_p, rate_q;      /* exact rate p/q Hz                     */
    char    rate_text[48];       /* as written, to hand to StimGen        */
    int     has_trigger;
    char    trigger[16];         /* "PFI0" ... "PFI15"                    */
    char    sim_script[CFG_VAL];
    char    sim_python[CFG_VAL];
    int     n_out;  OutCh out[CFG_MAX_OUT];
    int     n_in;   InCh  in[CFG_MAX_IN];
    int     n_cells, n_synapses; /* virtual preparation (device = neuron) */
    char    cell_name[CFG_MAX_CELLS][CFG_NAME];
    /* What the NEURON adapter needs (device = neuron), all of it optional in the file. */
    double  sim_dt_ms;           /* largest integration step, default 0.025 */
    double  sim_celsius;         /* default 6.3 (the classical squid axon)  */
    double  sim_v_init;          /* mV, default -65                         */
    double  sim_settle_ms;       /* time the cells rest before the first trial, default 0 */
    char    cfg_dir[512];        /* absolute folder of the configuration file: relative model paths start here */
    char    prep[CFG_PREP];      /* the [cell] and [synapse] sections, resolved, as INI text */
} Config;

/* Check the resolved table and fill c.  mode_flag is -m (MODE_NONE if not
 * given); it overrides [rig] mode, and one of the two must be present. */
int config_build(const Table *t, Mode mode_flag, Config *c, Err *err);

/* Volts that the amplifier command input should receive so that output o
 * produces `value` (in the channel unit of mode m): value / gain. */
double out_wanted_volts(const OutCh *o, Mode m, double value);

/* Volts the board is asked for to get `wanted` volts true: the inverse of the
 * calibration, (wanted - cal_offset) / cal_scale. */
double out_written_volts(const OutCh *o, double wanted);

/* The board's analog output range, volts (|v| <= 10). */
#define BOARD_AO_MAX_V 10.0

/* Check one value of output o against both limits above.  Returns 0 if it is
 * allowed; otherwise -1 and a message that says which limit and by how much
 * (without the name of the output or any file position: the caller adds
 * them).  `value` is in the unit of the mode m. */
int out_check_value(const OutCh *o, Mode m, double value, Err *err);

/* Is the rate p/q Hz possible with the inputs in use?  The board's analog
 * input limit is 500 kS/s, shared by all channels recorded. */
int config_check_rate(const Config *c, int64_t p, int64_t q, Err *err);

/* Name of a mode, for messages. */
const char *config_mode_name(Mode m);

#endif /* LCG2_CONFIG_H */
