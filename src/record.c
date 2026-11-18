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
 * record.c -- the HDF5 data file.  See record.h for the layout and the promises.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hdf5.h>
#include "record.h"

/*
 * The range of file formats the objects may use: from HDF5 1.8 to HDF5 1.10.  The upper
 * bound is what makes the file readable by older readers (Ubuntu 22.04 ships 1.10.7); the
 * lower one is what makes a file survive a full disk.  Measured (12 file-size limits, a
 * 5-trial run each):
 *
 *     format range                       opens after a full disk    and keeps a complete trial
 *     library default (no cap)                  11 of 12                   10 of 12
 *     EARLIEST .. 1.10                           1 of 12                    1 of 12
 *     1.8      .. 1.10   (this one)             11 of 12                   10 of 12
 *     1.10     .. 1.10                           0 of 12                    0 of 12
 *
 * With the earlier formats, a failed write leaves a superblock that promises more file than
 * there is ("truncated file"); with 1.10 only, the "open for write" flag is never cleared.
 */
#ifndef LCG2_LIBVER_LOW
#define LCG2_LIBVER_LOW H5F_LIBVER_V18
#endif

#define CHUNK_SAMPLES 8192               /* a fixed chunk: not tied to the block size */
#define WRITE_SLICE   65536              /* samples written at a time for a command    */

struct Recorder {
    hid_t        file;
    char         path[320];
    const Config *cfg;
    int          trials;
    /* the trial being written */
    hid_t        group, ai_group;
    hid_t        ai[CFG_MAX_IN];
    int          nai;
    int64_t      n_planned;
    int          open_trial;
    const TrialInfo *ti;                 /* valid between begin and end */
    int          failed;
};

/* ------------------------------------------------------------------ */
/* Errors: HDF5 says nothing by itself                                 */
/* ------------------------------------------------------------------ */

/* HDF5's own account of what went wrong: the descriptions of the first few errors on its
 * stack, innermost first.  The innermost one is not always the cause (a failed write can
 * show up as "slist already enabled?" with "file write failed ... File too large" a level
 * above), so a few are kept. */
static char first_error[360];

static herr_t walk(unsigned n, const H5E_error2_t *e, void *client)
{
    size_t used = strlen(first_error);
    (void)client;
    if (n < 3 && e->desc && e->desc[0] && used < sizeof first_error - 8)
        snprintf(first_error + used, sizeof first_error - used, "%s%s", used ? "; " : "", e->desc);
    return 0;
}

/* Turn the failure of an HDF5 call into an Err, keeping what HDF5 says about it. */
static int hfail(Err *err, const char *path, const char *what, const char *object)
{
    first_error[0] = 0;
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, walk, NULL);
    H5Eclear2(H5E_DEFAULT);
    return err_set(err, "cannot %s%s%s in the data file '%s'%s%s%s", what, object[0] ? " " : "", object, path,
                   first_error[0] ? " (HDF5: " : "", first_error, first_error[0] ? ")" : "");
}

/* ------------------------------------------------------------------ */
/* Small writers                                                       */
/* ------------------------------------------------------------------ */

/* Text is stored as variable-length UTF-8 strings. */
static hid_t string_type(void)
{
    hid_t t = H5Tcopy(H5T_C_S1);
    if (t < 0) return t;
    H5Tset_size(t, H5T_VARIABLE);
    H5Tset_cset(t, H5T_CSET_UTF8);
    return t;
}

static int attr_scalar(hid_t loc, const char *name, hid_t filetype, hid_t memtype, const void *value)
{
    hid_t space = H5Screate(H5S_SCALAR), a;
    int ok;
    if (space < 0) return -1;
    a = H5Acreate2(loc, name, filetype, space, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (a < 0) return -1;
    ok = H5Awrite(a, memtype, value) >= 0;
    H5Aclose(a);
    return ok ? 0 : -1;
}

static int attr_str(hid_t loc, const char *name, const char *v)
{
    hid_t t = string_type();
    const char *p = v ? v : "";
    int r;
    if (t < 0) return -1;
    r = attr_scalar(loc, name, t, t, &p);
    H5Tclose(t);
    return r;
}
static int attr_i64(hid_t loc, const char *name, int64_t v)
{
    return attr_scalar(loc, name, H5T_STD_I64LE, H5T_NATIVE_INT64, &v);
}
static int attr_i32(hid_t loc, const char *name, int v)
{
    return attr_scalar(loc, name, H5T_STD_I32LE, H5T_NATIVE_INT, &v);
}
static int attr_f64(hid_t loc, const char *name, double v)
{
    return attr_scalar(loc, name, H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, &v);
}

/* Replace the value of an existing integer attribute. */
static int attr_overwrite_i32(hid_t loc, const char *name, int v)
{
    hid_t a = H5Aopen(loc, name, H5P_DEFAULT);
    int ok;
    if (a < 0) return -1;
    ok = H5Awrite(a, H5T_NATIVE_INT, &v) >= 0;
    H5Aclose(a);
    return ok ? 0 : -1;
}

/* A text dataset (a scalar variable-length string). */
static int dataset_text(hid_t loc, const char *name, const char *text)
{
    hid_t t = string_type(), space, d;
    const char *p = text ? text : "";
    int ok;
    if (t < 0) return -1;
    space = H5Screate(H5S_SCALAR);
    if (space < 0) { H5Tclose(t); return -1; }
    d = H5Dcreate2(loc, name, t, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (d < 0) { H5Tclose(t); return -1; }
    ok = H5Dwrite(d, t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &p) >= 0;
    H5Dclose(d);
    H5Tclose(t);
    return ok ? 0 : -1;
}

/* Write count samples at offset `first` of a 1-D dataset (which has been made big enough). */
static int write_slice(hid_t dset, hid_t memtype, int64_t first, int64_t count, const void *data)
{
    hsize_t start = (hsize_t)first, cnt = (hsize_t)count;
    hid_t fs = H5Dget_space(dset), ms;
    int ok;
    if (fs < 0) return -1;
    if (H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, NULL, &cnt, NULL) < 0) { H5Sclose(fs); return -1; }
    ms = H5Screate_simple(1, &cnt, NULL);
    if (ms < 0) { H5Sclose(fs); return -1; }
    ok = H5Dwrite(dset, memtype, ms, fs, H5P_DEFAULT, data) >= 0;
    H5Sclose(ms);
    H5Sclose(fs);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                 */
/* ------------------------------------------------------------------ */

int record_open(Recorder **out, const RecorderInit *in, Err *err)
{
    Recorder *r = (Recorder *)calloc(1, sizeof *r);
    hid_t g = -1, fapl;
    *out = NULL;
    if (!r) return err_set(err, "out of memory");
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);           /* HDF5 must not print on its own */
    snprintf(r->path, sizeof r->path, "%s", in->path);
    r->cfg = in->cfg;
    r->group = r->ai_group = -1;

    /* The file format is capped at HDF5 1.10: the library that writes it may be newer (2.x
     * here), but the readers of the owner (Ubuntu 22.04 ships 1.10.7, MATLAB and h5py bundle
     * their own) must be able to open it.  See the table above for the lower bound. */
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0 || H5Pset_libver_bounds(fapl, LCG2_LIBVER_LOW, H5F_LIBVER_V110) < 0) {
        hfail(err, in->path, "set the file format", "of the file");
        if (fapl >= 0) H5Pclose(fapl);
        free(r);
        return -1;
    }
    r->file = H5Fcreate(in->path, H5F_ACC_EXCL, H5P_DEFAULT, fapl);          /* it must not exist */
    if (r->file < 0) {
        hfail(err, in->path, "create", "the file");      /* before closing fapl: that clears HDF5's error stack */
        H5Pclose(fapl);
        free(r);
        return -1;
    }
    H5Pclose(fapl);
    g = r->file;

    if (attr_str(g, "format", "lcg2-hdf5/1") < 0 || attr_str(g, "lcg2_version", in->lcg2_version) < 0 ||
        attr_str(g, "stimgen_commit", in->stimgen_commit) < 0 ||
        attr_str(g, "stimgen_sources_sha256", in->stimgen_sources_sha256) < 0 ||
        attr_str(g, "stimgen_library_version", in->stimgen_library_version) < 0 ||
        attr_str(g, "run_name", in->run_name) < 0 || attr_str(g, "started_utc", in->started_utc) < 0 ||
        attr_str(g, "mode", config_mode_name(in->cfg->mode)) < 0 ||
        attr_str(g, "device", in->cfg->device_name) < 0) {
        hfail(err, in->path, "write the attributes", "of the file");
        H5Fclose(r->file); free(r); return -1;
    }
    if ((in->stimulus_text && dataset_text(g, "stimulus_text", in->stimulus_text) < 0) ||
        dataset_text(g, "config", in->config_text) < 0 ||
        dataset_text(g, "command_line", in->command_line) < 0 ||
        dataset_text(g, "provenance", in->provenance_json) < 0) {
        hfail(err, in->path, "write the text datasets", "of the file");
        H5Fclose(r->file); free(r); return -1;
    }
    if (H5Fflush(r->file, H5F_SCOPE_GLOBAL) < 0) { hfail(err, in->path, "flush", "the file"); H5Fclose(r->file); free(r); return -1; }
    *out = r;
    return 0;
}

int record_trials(const Recorder *r) { return r->trials; }

/* Close whatever belongs to the trial in hand. */
static void trial_close(Recorder *r)
{
    int k;
    for (k = 0; k < r->nai; k++) if (r->ai[k] >= 0) H5Dclose(r->ai[k]);
    r->nai = 0;
    if (r->ai_group >= 0) H5Gclose(r->ai_group);
    if (r->group >= 0) H5Gclose(r->group);
    r->ai_group = r->group = -1;
    r->open_trial = 0;
    r->ti = NULL;
}

int record_close(Recorder *r, Err *err)
{
    int rc = 0;
    if (!r) return 0;
    trial_close(r);
    if (H5Fflush(r->file, H5F_SCOPE_GLOBAL) < 0) rc = hfail(err, r->path, "flush", "the file");
    /* Every object that was opened must have been closed: the file is the only one left. */
    if (rc == 0 && H5Fget_obj_count(r->file, H5F_OBJ_ALL) != 1)
        rc = err_set(err, "internal error: HDF5 objects were left open in '%s'", r->path);
    if (H5Fclose(r->file) < 0 && rc == 0) rc = hfail(err, r->path, "close", "the file");
    free(r);
    return rc;
}

/* ------------------------------------------------------------------ */
/* The trial                                                           */
/* ------------------------------------------------------------------ */

typedef struct { char name[64]; int64_t sample; } MarkerRec;

static int write_markers(hid_t group, const Trial *t)
{
    hid_t nm = H5Tcopy(H5T_C_S1), ct, space, d;
    hsize_t n = (hsize_t)t->nmark;
    MarkerRec *rec;
    int k, ok = 0;
    if (nm < 0) return -1;
    H5Tset_size(nm, 64);
    H5Tset_strpad(nm, H5T_STR_NULLTERM);
    H5Tset_cset(nm, H5T_CSET_UTF8);
    ct = H5Tcreate(H5T_COMPOUND, sizeof(MarkerRec));
    if (ct < 0) { H5Tclose(nm); return -1; }
    H5Tinsert(ct, "name", offsetof(MarkerRec, name), nm);
    H5Tinsert(ct, "sample", offsetof(MarkerRec, sample), H5T_NATIVE_INT64);
    rec = (MarkerRec *)calloc(n, sizeof *rec);
    if (rec) {
        for (k = 0; k < t->nmark; k++) {
            snprintf(rec[k].name, sizeof rec[k].name, "%s", t->mark[k].name);
            rec[k].sample = t->mark[k].sample;
        }
        space = H5Screate_simple(1, &n, NULL);
        if (space >= 0) {
            d = H5Dcreate2(group, "markers", ct, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            if (d >= 0) {
                ok = H5Dwrite(d, ct, H5S_ALL, H5S_ALL, H5P_DEFAULT, rec) >= 0;
                H5Dclose(d);
            }
            H5Sclose(space);
        }
        free(rec);
    }
    H5Tclose(ct);
    H5Tclose(nm);
    return ok ? 0 : -1;
}

/* The command played on one output, in its own unit. */
static int write_command(Recorder *r, hid_t ao, const TrialInfo *ti, int k)
{
    const OutCh *oc = &r->cfg->out[ti->cmd[k].out];
    int m = MODE_INDEX(r->cfg->mode);
    hsize_t n = (hsize_t)ti->n, chunk = n < WRITE_SLICE ? n : WRITE_SLICE;
    hid_t space = H5Screate_simple(1, &n, NULL), d;
    int ok = 1;
    int64_t first;
    double *buf = NULL;
    unsigned char *bytes = NULL;

    if (space < 0) return -1;
    d = H5Dcreate2(ao, ti->cmd[k].out_name, ti->cmd[k].digital ? H5T_STD_U8LE : H5T_IEEE_F64LE, space,
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (d < 0) return -1;
    if (ti->cmd[k].digital) bytes = (unsigned char *)malloc((size_t)chunk);
    else buf = (double *)malloc((size_t)chunk * sizeof(double));
    if (!buf && !bytes) { H5Dclose(d); return -1; }
    for (first = 0; first < ti->n && ok; first += (int64_t)chunk) {
        int64_t cnt = ti->n - first < (int64_t)chunk ? ti->n - first : (int64_t)chunk, i;
        if (ti->cmd[k].digital) {
            for (i = 0; i < cnt; i++) bytes[i] = ti->cmd[k].x[first + i] != 0.0;
            ok = write_slice(d, H5T_NATIVE_UINT8, first, cnt, bytes) == 0;
        } else {
            for (i = 0; i < cnt; i++) buf[i] = ti->cmd[k].x[first + i] * ti->cmd[k].scale;
            ok = write_slice(d, H5T_NATIVE_DOUBLE, first, cnt, buf) == 0;
        }
    }
    free(buf); free(bytes);
    if (ok) {
        ok = attr_str(d, "unit", ti->cmd[k].unit) == 0 && attr_str(d, "line", oc->line) == 0 &&
             attr_i32(d, "digital", oc->digital) == 0 && attr_f64(d, "rest", ti->cmd[k].rest) == 0;
        if (ok && !oc->digital)
            ok = attr_f64(d, "gain", oc->gain[m]) == 0 && attr_f64(d, "limit_v", oc->has_limit ? oc->limit : 10.0) == 0;
    }
    H5Dclose(d);
    return ok ? 0 : -1;
}

static int sink_begin(void *user, const TrialInfo *ti, Err *err)
{
    Recorder *r = (Recorder *)user;
    char gname[32];
    const InCh *ic;
    int k, m = MODE_INDEX(r->cfg->mode);
    const char *kind;

    trial_close(r);                                    /* if an earlier trial was left half open */
    snprintf(gname, sizeof gname, "trial_%04d", ti->index);
    r->group = H5Gcreate2(r->file, gname, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (r->group < 0) return hfail(err, r->path, "create the group", gname);
    r->open_trial = 1;
    r->ti = ti;
    r->n_planned = ti->n;
    r->trials++;
    kind = ti->trial ? (ti->trial->info ? "protocol trial" : "stimulus") : "record";

    /* incomplete = 1 now; it becomes 0 only when the trial ends complete. */
    if (attr_i32(r->group, "incomplete", 1) < 0 || attr_i32(r->group, "index", ti->index) < 0 ||
        attr_str(r->group, "name", ti->name) < 0 || attr_str(r->group, "kind", kind) < 0 ||
        attr_str(r->group, "seed", ti->trial ? ti->trial->seed : "") < 0 ||
        attr_str(r->group, "variables", ti->variables) < 0 ||
        attr_f64(r->group, "rate_hz", (double)ti->rate_p / (double)ti->rate_q) < 0 ||
        attr_i64(r->group, "rate_p", ti->rate_p) < 0 || attr_i64(r->group, "rate_q", ti->rate_q) < 0 ||
        attr_i64(r->group, "n_samples", ti->n) < 0 || attr_i64(r->group, "scheduled_ns", ti->scheduled_ns) < 0 ||
        attr_i32(r->group, "virtual_time", ti->is_virtual) < 0)
        return hfail(err, r->path, "write the attributes of the group", gname);

    /* the inputs: empty, unlimited, filled block by block */
    r->ai_group = H5Gcreate2(r->group, "ai", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (r->ai_group < 0) return hfail(err, r->path, "create the group", "ai");
    for (k = 0; k < ti->nin; k++) {
        hsize_t d0 = 0, dmax = H5S_UNLIMITED, chunk = CHUNK_SAMPLES;
        hid_t space = H5Screate_simple(1, &d0, &dmax), dcpl = H5Pcreate(H5P_DATASET_CREATE);
        int ok;
        ic = &r->cfg->in[ti->ai[k].in];
        if (space < 0 || dcpl < 0 || H5Pset_chunk(dcpl, 1, &chunk) < 0) {
            if (space >= 0) H5Sclose(space);
            if (dcpl >= 0) H5Pclose(dcpl);
            return hfail(err, r->path, "prepare the dataset", ti->ai[k].name);
        }
        r->ai[k] = H5Dcreate2(r->ai_group, ti->ai[k].name, H5T_IEEE_F64LE, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        H5Sclose(space);
        H5Pclose(dcpl);
        if (r->ai[k] < 0) return hfail(err, r->path, "create the dataset", ti->ai[k].name);
        r->nai = k + 1;
        ok = attr_str(r->ai[k], "unit", ti->ai[k].unit) == 0 && attr_str(r->ai[k], "line", ic->line) == 0 &&
             attr_f64(r->ai[k], "gain", ic->gain[m]) == 0 && attr_f64(r->ai[k], "cal_scale", ic->cal_scale) == 0 &&
             attr_f64(r->ai[k], "cal_offset", ic->cal_offset) == 0 && attr_f64(r->ai[k], "range_v", ic->range) == 0 &&
             attr_str(r->ai[k], "terminal", ic->terminal) == 0 &&
             attr_str(r->ai[k], "mode", config_mode_name(r->cfg->mode)) == 0;
        if (!ok) return hfail(err, r->path, "write the attributes of the dataset", ti->ai[k].name);
    }

    /* the command that is being played, if the stimulus names any output */
    if (ti->ncmd > 0) {
        hid_t ao = H5Gcreate2(r->group, "ao", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (ao < 0) return hfail(err, r->path, "create the group", "ao");
        for (k = 0; k < ti->ncmd; k++)
            if (write_command(r, ao, ti, k) < 0) {
                H5Gclose(ao);
                return hfail(err, r->path, "write the command", ti->cmd[k].out_name);
            }
        H5Gclose(ao);
    }
    if (ti->trial && ti->trial->nmark > 0 && write_markers(r->group, ti->trial) < 0)
        return hfail(err, r->path, "write the markers", gname);
    if (ti->trial && ti->trial->info && dataset_text(r->group, "info", ti->trial->info) < 0)
        return hfail(err, r->path, "write the record of the trial", gname);
    return 0;
}

static int sink_block(void *user, int64_t first, int64_t count, const double *const x[], Err *err)
{
    Recorder *r = (Recorder *)user;
    int k;
    hsize_t newdim = (hsize_t)(first + count);
    for (k = 0; k < r->nai; k++) {
        if (H5Dset_extent(r->ai[k], &newdim) < 0 || write_slice(r->ai[k], H5T_NATIVE_DOUBLE, first, count, x[k]) < 0)
            return hfail(err, r->path, "write inputs to the dataset", r->ti->ai[k].name);
    }
    return 0;
}

static int sink_end(void *user, const TrialInfo *ti, const TrialRecord *rec, Err *err)
{
    Recorder *r = (Recorder *)user;
    int k;
    if (!r->open_trial) return 0;
    for (k = 0; k < r->nai; k++)
        if (attr_i64(r->ai[k], "overrange_samples", rec->overrange[k]) < 0)
            return hfail(err, r->path, "write the attributes of the dataset", ti->ai[k].name);
    if (attr_i64(r->group, "start_ns", rec->start_ns) < 0 || attr_i64(r->group, "end_ns", rec->end_ns) < 0 ||
        attr_i64(r->group, "delay_ns", rec->delay_ns) < 0 || attr_i64(r->group, "latency_ns", rec->latency_ns) < 0 ||
        attr_str(r->group, "start_utc", rec->start_utc) < 0 || attr_i64(r->group, "recorded", rec->recorded) < 0)
        return hfail(err, r->path, "write the attributes of the group", ti->name);
    if (!rec->incomplete && attr_overwrite_i32(r->group, "incomplete", 0) < 0)
        return hfail(err, r->path, "mark the trial complete", ti->name);
    trial_close(r);
    if (H5Fflush(r->file, H5F_SCOPE_GLOBAL) < 0) return hfail(err, r->path, "flush", "the file");
    return 0;
}

void record_sink(Recorder *r, Sink *sink)
{
    sink->begin = sink_begin;
    sink->block = sink_block;
    sink->end = sink_end;
    sink->user = r;
}
