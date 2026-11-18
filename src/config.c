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
 * config.c -- the configuration file: reading, rig profiles, validation.
 *
 * Read config.h first: it explains the three steps (table, profile, typed
 * configuration) and the file format.  This file implements them.
 *
 * The INI syntax itself is read by inih (vendor/inih, BSD licence), which
 * calls ini_handler_fn() below once for every section header and every
 * "key = value" line.  inih is compiled with these options (the Makefile
 * passes the same ones to ini.c and to this file, because they change the
 * handler's prototype):
 *
 *   INI_HANDLER_LINENO=1             the handler learns the line number
 *   INI_CALL_HANDLER_ON_NEW_SECTION  the handler sees empty sections too
 *   INI_ALLOW_MULTILINE=0            no continuation lines (a stray indented
 *                                    line is an error, not part of a value)
 *   INI_STOP_ON_FIRST_ERROR=1        report the first mistake only
 *   INI_MAX_LINE=1000                longer lines are refused (see reader())
 *
 * inih cuts a section name longer than 49 characters (a fixed limit of
 * r58, not an option).  A cut name is still 49 characters, more than the 47
 * that CFG_NAME allows, so the handler refuses it: nothing is truncated
 * silently.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "ini.h"

/* ------------------------------------------------------------------ */
/* Messages (err_set() itself is in err.c)                             */
/* ------------------------------------------------------------------ */

/*
 * Error located in the file: "path:LINE: [section] key: message".  en may be
 * a section header (empty key), then the key is left out.
 */
static int cfg_err(const Table *t, const Entry *en, Err *err, const char *fmt, ...)
{
    char body[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (en->key[0])
        return err_set(err, "%s:%d: [%s] %s: %s", t->path, en->line, en->sect,
                       en->key, body);
    return err_set(err, "%s:%d: [%s] %s", t->path, en->line, en->sect, body);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* An identifier: a letter or underscore, then letters, digits, underscores.
 * It becomes a channel name in the stimulus and a dataset name in HDF5. */
static int is_ident(const char *s)
{
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (s++; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

/* Key names in the file: lower case, so that "Gain_cc" is a typo, not a key. */
static int is_key_name(const char *s)
{
    if (!islower((unsigned char)*s)) return 0;
    for (s++; *s; s++)
        if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_'))
            return 0;
    return 1;
}

/* The kinds of section.  has_name: "[out P1]" has a name, "[rig]" does not. */
static const struct { const char *kind; int has_name; } section_kinds[] = {
    { "rig", 0 }, { "out", 1 }, { "in", 1 }, { "cell", 1 },
    { "synapse", 1 }, { "profile", 1 }, { NULL, 0 }
};

/* Split a section header into kind and name; 0 if it is not a legal one. */
static int split_section(const char *s, char *kind, size_t kn, char *name, size_t nn)
{
    const char *sp = strchr(s, ' ');
    size_t kl = sp ? (size_t)(sp - s) : strlen(s);
    int i;

    if (kl == 0 || kl >= kn) return 0;
    memcpy(kind, s, kl);
    kind[kl] = 0;
    name[0] = 0;
    for (i = 0; section_kinds[i].kind; i++)
        if (strcmp(kind, section_kinds[i].kind) == 0) break;
    if (!section_kinds[i].kind) return 0;
    if (!section_kinds[i].has_name) return sp == NULL;
    if (!sp || strlen(sp + 1) >= nn || !is_ident(sp + 1)) return 0;
    strcpy(name, sp + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Step 1: reading the file                                            */
/* ------------------------------------------------------------------ */

static int find_section(const Table *t, const char *sect)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (t->e[i].key[0] == 0 && strcmp(t->e[i].sect, sect) == 0) return i;
    return -1;
}

static int find_entry(const Table *t, const char *sect, const char *key)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (strcmp(t->e[i].sect, sect) == 0 && strcmp(t->e[i].key, key) == 0)
            return i;
    return -1;
}

/* Append an entry; -1 if the table is full. */
static int add_entry(Table *t, const char *sect, const char *key, const char *val, int line)
{
    Entry *en;
    if (t->n >= CFG_MAX_ENTRIES) return -1;
    en = &t->e[t->n++];
    strcpy(en->sect, sect);
    strcpy(en->key, key);
    strcpy(en->val, val);
    en->line = line;
    return 0;
}

/*
 * Keys of a profile are paths: "rig.KEY" or "KIND.NAME.KEY".  Split such a
 * path into the section it targets ("rig", "out P2") and the key.  Returns 0
 * if the path is not of that form.
 */
static int split_profile_key(const char *path, char *sect, size_t sn, char *key, size_t kn)
{
    char first[16], name[CFG_NAME];
    const char *d1 = strchr(path, '.'), *d2;

    if (!d1 || (size_t)(d1 - path) >= sizeof first) return 0;
    memcpy(first, path, (size_t)(d1 - path));
    first[d1 - path] = 0;
    if (strcmp(first, "rig") == 0) {
        if (strlen(d1 + 1) >= kn || !is_key_name(d1 + 1)) return 0;
        snprintf(sect, sn, "rig");
        strcpy(key, d1 + 1);
        return 1;
    }
    if (strcmp(first, "out") && strcmp(first, "in") && strcmp(first, "cell") &&
        strcmp(first, "synapse")) return 0;
    d2 = strchr(d1 + 1, '.');
    if (!d2 || (size_t)(d2 - d1 - 1) >= sizeof name) return 0;
    memcpy(name, d1 + 1, (size_t)(d2 - d1 - 1));
    name[d2 - d1 - 1] = 0;
    if (!is_ident(name) || strlen(d2 + 1) >= kn || !is_key_name(d2 + 1)) return 0;
    snprintf(sect, sn, "%s %s", first, name);
    strcpy(key, d2 + 1);
    return 1;
}

/* Called by inih for every section header (name == NULL) and every key.
 * Returning 0 tells inih to stop; the message is left in the table. */
static int ini_handler_fn(void *user, const char *section, const char *name,
                          const char *value, int lineno)
{
    Table *t = (Table *)user;
    char kind[16], nm[CFG_NAME], s2[CFG_NAME], k2[CFG_NAME];

#define FAIL(...) do { err_set(&t->pending, __VA_ARGS__); t->has_pending = 1; return 0; } while (0)

    if (name == NULL) {                              /* a section header */
        if (strlen(section) >= CFG_NAME)
            FAIL("%s:%d: section name is too long (at most %d characters)",
                 t->path, lineno, CFG_NAME - 1);
        if (!split_section(section, kind, sizeof kind, nm, sizeof nm))
            FAIL("%s:%d: bad section [%s]; use [rig], [out NAME], [in NAME], "
                 "[cell NAME], [synapse NAME] or [profile NAME], with NAME made "
                 "of letters, digits and _", t->path, lineno, section);
        if (find_section(t, section) >= 0)
            FAIL("%s:%d: section [%s] appears twice", t->path, lineno, section);
        if (add_entry(t, section, "", "", lineno) < 0)
            FAIL("%s:%d: the file is too large (more than %d entries)",
                 t->path, lineno, CFG_MAX_ENTRIES);
        return 1;
    }

    /* A "key = value" line. */
    if (section[0] == 0)
        FAIL("%s:%d: key '%s' is outside any section", t->path, lineno, name);
    if (strlen(name) >= CFG_NAME)
        FAIL("%s:%d: key name is too long", t->path, lineno);
    if (strlen(value) >= CFG_VAL)
        FAIL("%s:%d: [%s] %s: value is too long (at most %d characters)",
             t->path, lineno, section, name, CFG_VAL - 1);
    split_section(section, kind, sizeof kind, nm, sizeof nm);
    if (strcmp(kind, "profile") == 0) {
        if (!split_profile_key(name, s2, sizeof s2, k2, sizeof k2))
            FAIL("%s:%d: [%s] bad key '%s'; a profile line is written "
                 "rig.KEY = value or out.NAME.KEY = value (also in, cell, "
                 "synapse)", t->path, lineno, section, name);
    } else if (!is_key_name(name)) {
        FAIL("%s:%d: [%s] bad key name '%s' (lower case letters, digits, _)",
             t->path, lineno, section, name);
    }
    if (find_entry(t, section, name) >= 0)
        FAIL("%s:%d: [%s] key '%s' is given twice", t->path, lineno, section, name);
    if (add_entry(t, section, name, value, lineno) < 0)
        FAIL("%s:%d: the file is too large (more than %d entries)",
             t->path, lineno, CFG_MAX_ENTRIES);
    return 1;
#undef FAIL
}

/*
 * inih reads lines with a fixed buffer; a longer line would be cut in two
 * pieces and misread.  This reader (fgets plus a check) notes the first line
 * that did not fit, so that config_load() can refuse the file.
 */
typedef struct { FILE *f; int line; int long_line; } Reader;

static char *reader_fn(char *str, int num, void *stream)
{
    Reader *r = (Reader *)stream;
    size_t n;
    char *s = fgets(str, num, r->f);
    if (!s) return NULL;
    r->line++;
    n = strlen(s);
    if (n > 0 && s[n - 1] != '\n' && !feof(r->f) && r->long_line == 0)
        r->long_line = r->line;
    return s;
}

int config_load(Table *t, const char *path, Err *err)
{
    Reader rd;
    int rc;

    memset(t, 0, sizeof *t);
    if (strlen(path) >= sizeof t->path)
        return err_set(err, "configuration path is too long");
    strcpy(t->path, path);
    rd.f = fopen(path, "r");
    if (!rd.f)
        return err_set(err, "cannot open configuration file '%s': %s", path,
                       strerror(errno));
    rd.line = 0;
    rd.long_line = 0;
    rc = ini_parse_stream(reader_fn, &rd, ini_handler_fn, t);
    fclose(rd.f);

    if (rd.long_line)
        return err_set(err, "%s:%d: line is too long (at most %d characters)",
                       path, rd.long_line, INI_MAX_LINE - 2);
    if (rc == -2) return err_set(err, "%s: out of memory", path);
    if (rc != 0) {
        if (t->has_pending) { *err = t->pending; return -1; }
        return err_set(err, "%s:%d: syntax error: expected [section], key = value "
                       "or a comment line starting with ; or #", path, rc);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Step 2: profiles                                                    */
/* ------------------------------------------------------------------ */

int config_apply_profile(Table *t, const char *name, Err *err)
{
    char psect[CFG_NAME + 8], sect[CFG_NAME], key[CFG_NAME];
    int i, n0, k;

    snprintf(psect, sizeof psect, "profile %s", name);
    if (find_section(t, psect) < 0) {
        char list[200] = "";
        for (i = 0; i < t->n; i++)
            if (t->e[i].key[0] == 0 && strncmp(t->e[i].sect, "profile ", 8) == 0) {
                if (list[0]) strncat(list, ", ", sizeof list - strlen(list) - 1);
                strncat(list, t->e[i].sect + 8, sizeof list - strlen(list) - 1);
            }
        if (list[0])
            return err_set(err, "profile '%s' not found in %s (available: %s)",
                           name, t->path, list);
        return err_set(err, "profile '%s' not found: %s defines no [profile NAME] "
                       "section", name, t->path);
    }

    /* New keys are appended while we go, so remember where the file ended. */
    n0 = t->n;
    for (i = 0; i < n0; i++) {
        Entry p = t->e[i];            /* a copy: the table may grow below */
        if (p.key[0] == 0 || strcmp(p.sect, psect) != 0) continue;
        split_profile_key(p.key, sect, sizeof sect, key, sizeof key); /* checked at load */
        if (find_section(t, sect) < 0)
            return err_set(err, "%s:%d: [%s] %s: there is no section [%s] to override",
                           t->path, p.line, psect, p.key, sect);
        k = find_entry(t, sect, key);
        if (k >= 0) {
            strcpy(t->e[k].val, p.val);
            t->e[k].line = p.line;
        } else if (add_entry(t, sect, key, p.val, p.line) < 0) {
            return err_set(err, "%s: too many entries after applying the profile",
                           t->path);
        }
    }
    snprintf(t->profile, sizeof t->profile, "%s", name);
    return 0;
}

int config_override(Table *t, const char *sect, const char *key, const char *val, Err *err)
{
    int k = find_entry(t, sect, key);
    if (k >= 0) {
        snprintf(t->e[k].val, sizeof t->e[k].val, "%s", val);
        t->e[k].line = 0;
    } else if (add_entry(t, sect, key, val, 0) < 0) {
        return err_set(err, "%s: too many entries", t->path);
    }
    return 0;
}

void config_dump(const Table *t, FILE *f)
{
    int pass, i, j;

    fprintf(f, "; LCG2 resolved configuration\n; file: %s\n; profile: %s\n",
            t->path, t->profile[0] ? t->profile : "(none)");
    /* Ordinary sections first, profile sections last. */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < t->n; i++) {
            int is_profile = strncmp(t->e[i].sect, "profile ", 8) == 0;
            if (t->e[i].key[0] != 0 || is_profile != pass) continue;
            fprintf(f, "\n[%s]\n", t->e[i].sect);
            for (j = 0; j < t->n; j++)
                if (t->e[j].key[0] && strcmp(t->e[j].sect, t->e[i].sect) == 0)
                    fprintf(f, "%s = %s%s\n", t->e[j].key, t->e[j].val,
                            t->e[j].line == 0 ? "   ; set on the command line" : "");
        }
}

/* ------------------------------------------------------------------ */
/* Step 3: typed values                                                */
/* ------------------------------------------------------------------ */

const char *config_mode_name(Mode m)
{
    return m == MODE_CC ? "cc" : m == MODE_VC ? "vc" : "none";
}

/* The entry of a key, or NULL if the file does not give it. */
static const Entry *get(const Table *t, const char *sect, const char *key)
{
    int i = find_entry(t, sect, key);
    return i < 0 ? NULL : &t->e[i];
}

/* The header of a section, for errors that concern the whole section. */
static const Entry *header(const Table *t, const char *sect)
{
    return &t->e[find_section(t, sect)];
}

/* Every key of a section must be in the allowed list (NULL-terminated). */
static int check_keys(const Table *t, const char *sect, const char *const *allowed, Err *err)
{
    int i, k;
    for (i = 0; i < t->n; i++) {
        if (t->e[i].key[0] == 0 || strcmp(t->e[i].sect, sect) != 0) continue;
        for (k = 0; allowed[k]; k++)
            if (strcmp(allowed[k], t->e[i].key) == 0) break;
        if (!allowed[k]) {
            char list[300] = "";
            for (k = 0; allowed[k]; k++) {
                if (k) strncat(list, ", ", sizeof list - strlen(list) - 1);
                strncat(list, allowed[k], sizeof list - strlen(list) - 1);
            }
            return cfg_err(t, &t->e[i], err, "unknown key (allowed: %s)", list);
        }
    }
    return 0;
}

/* A finite number, the whole value. */
static int get_number(const Table *t, const Entry *en, double *v, Err *err)
{
    char *end;
    errno = 0;
    *v = strtod(en->val, &end);
    if (en->val[0] == 0 || *end != 0 || errno == ERANGE || !isfinite(*v))
        return cfg_err(t, en, err, "expected a number, got '%s'", en->val);
    return 0;
}

/* yes or no (nothing else, so a typo is not read as "no"). */
static int get_yesno(const Table *t, const Entry *en, int *v, Err *err)
{
    if (strcmp(en->val, "yes") == 0) { *v = 1; return 0; }
    if (strcmp(en->val, "no") == 0)  { *v = 0; return 0; }
    return cfg_err(t, en, err, "expected yes or no, got '%s'", en->val);
}

/* "<prefix>N" with N in 0..max, no sign, no leading zero: returns N or -1. */
static int line_number(const char *s, const char *prefix, int max)
{
    size_t pl = strlen(prefix);
    int n = 0;
    const char *d;
    if (strncmp(s, prefix, pl) != 0) return -1;
    d = s + pl;
    if (!*d || (d[0] == '0' && d[1])) return -1;
    for (; *d; d++) {
        if (*d < '0' || *d > '9') return -1;
        n = n * 10 + (*d - '0');
        if (n > max) return -1;
    }
    return n;
}

/*
 * The per-mode gain and unit of a channel, keys gain_cc/unit_cc and
 * gain_vc/unit_vc.  The two keys of a mode go together.  want[m] is the
 * dimension the unit must have in mode m (0 = cc, 1 = vc), which differs for
 * outputs (cc sends current) and inputs (cc reads voltage).
 */
static int read_gains(const Table *t, const char *sect, const Dim want[2],
                      int has_gain[2], double gain[2], char unit[2][8], Err *err)
{
    static const char *gk[2] = { "gain_cc", "gain_vc" };
    static const char *uk[2] = { "unit_cc", "unit_vc" };
    static const char *dn[2][3] = {
        { "", "a current (pA, nA, ...)", "a voltage (mV, V, ...)" },
        { "", "a current (pA, nA, ...)", "a voltage (mV, V, ...)" } };
    int m;
    for (m = 0; m < 2; m++) {
        const Entry *g = get(t, sect, gk[m]), *u = get(t, sect, uk[m]);
        Dim d;
        double scale;
        has_gain[m] = 0;
        gain[m] = 1.0;
        unit[m][0] = 0;
        if (!g && !u) continue;
        if (!g || !u)
            return cfg_err(t, g ? g : u, err, "%s and %s must be given together",
                           gk[m], uk[m]);
        if (get_number(t, g, &gain[m], err) < 0) return -1;
        if (gain[m] <= 0)
            return cfg_err(t, g, err, "the gain must be positive (units per volt)");
        if (strlen(u->val) >= 8 || units_lookup(u->val, &d, &scale) < 0)
            return cfg_err(t, u, err, "unknown unit '%s' (use a current unit "
                           "fA pA nA uA mA A, or a voltage unit uV mV V)", u->val);
        if (d != want[m])
            return cfg_err(t, u, err, "'%s' has the wrong dimension: here the unit "
                           "must be %s", u->val, dn[m][want[m]]);
        strcpy(unit[m], u->val);
        has_gain[m] = 1;
    }
    return 0;
}

/* cal_offset and cal_scale, shared by outputs and inputs. */
static int read_cal(const Table *t, const char *sect, double *off, double *scl, Err *err)
{
    const Entry *o = get(t, sect, "cal_offset"), *s = get(t, sect, "cal_scale");
    *off = 0.0;
    *scl = 1.0;
    if (o && get_number(t, o, off, err) < 0) return -1;
    if (s) {
        if (get_number(t, s, scl, err) < 0) return -1;
        if (*scl == 0.0) return cfg_err(t, s, err, "cal_scale must not be zero");
    }
    return 0;
}

/* The mode-dependent requirement: a channel in use needs a gain for the mode
 * of this run. */
static int need_mode_gain(const Table *t, const char *sect, int skip, Mode mode,
                          const int has_gain[2], Err *err)
{
    if (skip || has_gain[MODE_INDEX(mode)]) return 0;
    return cfg_err(t, header(t, sect), err,
                   "no gain_%s and unit_%s, which the %s mode of this run needs "
                   "(or give skip = yes)", config_mode_name(mode),
                   config_mode_name(mode), config_mode_name(mode));
}

static const char *const rig_keys[] = { "device", "mode", "rate", "trigger",
    "sim_script", "sim_python", NULL };
static const char *const out_keys[] = { "line", "digital", "gain_cc", "unit_cc",
    "gain_vc", "unit_vc", "cal_offset", "cal_scale", "limit", "rest", "skip", NULL };
static const char *const in_keys[] = { "line", "range", "terminal", "gain_cc",
    "unit_cc", "gain_vc", "unit_vc", "cal_offset", "cal_scale", "skip", NULL };

static int build_rig(const Table *t, Mode mode_flag, Config *c, Err *err)
{
    const Entry *en;
    Mode rig_mode = MODE_NONE;

    if (find_section(t, "rig") < 0)
        return err_set(err, "%s: there is no [rig] section", t->path);
    if (check_keys(t, "rig", rig_keys, err) < 0) return -1;

    /* device */
    en = get(t, "rig", "device");
    if (!en)
        return cfg_err(t, header(t, "rig"), err,
                       "device is required (sim, neuron, or the board name, e.g. Dev1)");
    if (strcmp(en->val, "sim") == 0) c->device = DEV_SIM;
    else if (strcmp(en->val, "neuron") == 0) c->device = DEV_NEURON;
    else if (is_ident(en->val) && strlen(en->val) < sizeof c->device_name)
        c->device = DEV_DAQMX;
    else
        return cfg_err(t, en, err, "'%s' is not sim, neuron or a board name like Dev1",
                       en->val);
    strcpy(c->device_name, en->val);

    /* mode: the flag wins over the file; one of them must exist */
    en = get(t, "rig", "mode");
    if (en) {
        if (strcmp(en->val, "cc") == 0) rig_mode = MODE_CC;
        else if (strcmp(en->val, "vc") == 0) rig_mode = MODE_VC;
        else return cfg_err(t, en, err, "expected cc or vc, got '%s'", en->val);
    }
    c->mode = mode_flag != MODE_NONE ? mode_flag : rig_mode;
    if (c->mode == MODE_NONE)
        return cfg_err(t, header(t, "rig"), err,
                       "the amplifier mode is not set: give -m cc or -m vc, or "
                       "mode = cc|vc here");

    /* rate (optional here: the stimulus or -r may give it) */
    en = get(t, "rig", "rate");
    if (en) {
        if (units_parse_rate(en->val, &c->rate_p, &c->rate_q) < 0)
            return cfg_err(t, en, err, "'%s' is not a rate (examples: 20kHz, 10000, "
                           "2.5Hz)", en->val);
        snprintf(c->rate_text, sizeof c->rate_text, "%s", en->val);
        c->has_rate = 1;
    }

    /* trigger: PFI0 .. PFI15 */
    en = get(t, "rig", "trigger");
    if (en) {
        if (line_number(en->val, "PFI", 15) < 0)
            return cfg_err(t, en, err, "expected PFI0 ... PFI15, got '%s'", en->val);
        strcpy(c->trigger, en->val);
        c->has_trigger = 1;
    }

    /* the simulator script belongs to device = neuron, and only to it */
    en = get(t, "rig", "sim_script");
    if (c->device == DEV_NEURON && !en)
        return cfg_err(t, header(t, "rig"), err,
                       "device = neuron needs sim_script (the model script)");
    if (c->device != DEV_NEURON && (en || get(t, "rig", "sim_python")))
        return cfg_err(t, en ? en : get(t, "rig", "sim_python"), err,
                       "only used with device = neuron");
    if (en) strcpy(c->sim_script, en->val);
    en = get(t, "rig", "sim_python");
    strcpy(c->sim_python, en ? en->val : "python3");
    return 0;
}

static int build_out(const Table *t, const char *sect, const char *name, Config *c, Err *err)
{
    static const Dim want[2] = { DIM_CURRENT, DIM_VOLTAGE };  /* cc sends current */
    OutCh *o;
    const Entry *en;

    if (c->n_out >= CFG_MAX_OUT)
        return cfg_err(t, header(t, sect), err, "too many outputs");
    if (check_keys(t, sect, out_keys, err) < 0) return -1;
    o = &c->out[c->n_out];
    memset(o, 0, sizeof *o);
    strcpy(o->name, name);

    if ((en = get(t, sect, "skip")) && get_yesno(t, en, &o->skip, err) < 0) return -1;
    if ((en = get(t, sect, "digital")) && get_yesno(t, en, &o->digital, err) < 0) return -1;

    en = get(t, sect, "line");
    if (!en)
        return cfg_err(t, header(t, sect), err, "line is required (ao0 ... ao3, or "
                       "port0/line0 ... port0/line7 with digital = yes)");
    if (strlen(en->val) >= sizeof o->line)
        return cfg_err(t, en, err, "unknown line '%s'", en->val);
    strcpy(o->line, en->val);

    if (o->digital) {
        /* A clocked digital output exists on port 0 only (datasheet). */
        o->index = line_number(en->val, "port0/line", 7);
        if (o->index < 0)
            return cfg_err(t, en, err, "a digital output must be on port 0: "
                           "port0/line0 ... port0/line7 (got '%s')", en->val);
        /* Digital lines carry no gain, unit, limit, calibration or rest. */
        {
            static const char *const no[] = { "gain_cc", "unit_cc", "gain_vc",
                "unit_vc", "cal_offset", "cal_scale", "limit", "rest", NULL };
            int k;
            for (k = 0; no[k]; k++)
                if ((en = get(t, sect, no[k])))
                    return cfg_err(t, en, err, "not allowed on a digital line (it "
                                   "rests at the stimulus value, otherwise 0)");
        }
        o->cal_scale = 1.0;
    } else {
        o->index = line_number(en->val, "ao", 3);
        if (o->index < 0)
            return cfg_err(t, en, err, "an analog output is ao0 ... ao3 (got '%s'); "
                           "a digital one needs digital = yes and port0/lineN",
                           en->val);
        if (read_gains(t, sect, want, o->has_gain, o->gain, o->unit, err) < 0 ||
            read_cal(t, sect, &o->cal_offset, &o->cal_scale, err) < 0)
            return -1;
        if (need_mode_gain(t, sect, o->skip, c->mode, o->has_gain, err) < 0) return -1;
        if ((en = get(t, sect, "limit"))) {
            if (get_number(t, en, &o->limit, err) < 0) return -1;
            if (o->limit <= 0 || o->limit > 10)
                return cfg_err(t, en, err, "the limit is in volts, above 0 and at "
                               "most 10 (the board's output range)");
            o->has_limit = 1;
        }
        if ((en = get(t, sect, "rest")) && get_number(t, en, &o->rest, err) < 0)
            return -1;
    }
    c->n_out++;
    return 0;
}

static int build_in(const Table *t, const char *sect, const char *name, Config *c, Err *err)
{
    static const Dim want[2] = { DIM_VOLTAGE, DIM_CURRENT };  /* cc reads voltage */
    static const double ranges[4] = { 0.2, 1.0, 5.0, 10.0 };
    InCh *in;
    const Entry *en;
    int k;

    if (c->n_in >= CFG_MAX_IN)
        return cfg_err(t, header(t, sect), err, "too many inputs");
    if (check_keys(t, sect, in_keys, err) < 0) return -1;
    in = &c->in[c->n_in];
    memset(in, 0, sizeof *in);
    strcpy(in->name, name);

    if ((en = get(t, sect, "skip")) && get_yesno(t, en, &in->skip, err) < 0) return -1;
    en = get(t, sect, "line");
    if (!en)
        return cfg_err(t, header(t, sect), err, "line is required (ai0 ... ai15)");
    in->index = strlen(en->val) < sizeof in->line ? line_number(en->val, "ai", 15) : -1;
    if (in->index < 0)
        return cfg_err(t, en, err, "an analog input is ai0 ... ai15 (got '%s')", en->val);
    strcpy(in->line, en->val);

    in->range = 10.0;
    if ((en = get(t, sect, "range"))) {
        if (get_number(t, en, &in->range, err) < 0) return -1;
        for (k = 0; k < 4 && in->range != ranges[k]; k++) { }
        if (k == 4)
            return cfg_err(t, en, err, "the range is 0.2, 1, 5 or 10 (volts), got '%s'",
                           en->val);
    }
    strcpy(in->terminal, "RSE");
    if ((en = get(t, sect, "terminal"))) {
        if (strcmp(en->val, "RSE") && strcmp(en->val, "NRSE") && strcmp(en->val, "DIFF"))
            return cfg_err(t, en, err, "expected RSE, NRSE or DIFF, got '%s'", en->val);
        strcpy(in->terminal, en->val);
    }
    if (read_gains(t, sect, want, in->has_gain, in->gain, in->unit, err) < 0 ||
        read_cal(t, sect, &in->cal_offset, &in->cal_scale, err) < 0)
        return -1;
    if (need_mode_gain(t, sect, in->skip, c->mode, in->has_gain, err) < 0) return -1;
    c->n_in++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output limits                                                       */
/* ------------------------------------------------------------------ */

double out_wanted_volts(const OutCh *o, Mode m, double value)
{
    return value / o->gain[MODE_INDEX(m)];
}

double out_written_volts(const OutCh *o, double wanted)
{
    return (wanted - o->cal_offset) / o->cal_scale;
}

int out_check_value(const OutCh *o, Mode m, double value, Err *err)
{
    double wanted = out_wanted_volts(o, m, value);
    double written = out_written_volts(o, wanted);
    const char *unit = o->unit[MODE_INDEX(m)];

    /* Every comparison with NaN is false, so a NaN would slip through the two
     * limits below.  A value that is not a finite number, or that becomes one
     * on the way to volts, is refused first. */
    if (!isfinite(value))
        return err_set(err, "the value is not a finite number");
    if (!isfinite(wanted) || !isfinite(written))
        return err_set(err, "%g %s does not give a finite voltage (gain %g, "
                       "calibration scale %g)", value, unit, o->gain[MODE_INDEX(m)],
                       o->cal_scale);
    if (o->has_limit && fabs(wanted) > o->limit)
        return err_set(err, "%g %s needs %g V at the amplifier command, above the "
                       "limit of %g V", value, unit, wanted, o->limit);
    if (fabs(written) > BOARD_AO_MAX_V)
        return err_set(err, "%g %s needs %g V at the board after calibration, above "
                       "its range of %g V", value, unit, written, BOARD_AO_MAX_V);
    return 0;
}

/* Two active channels must not use the same physical line. */
static int check_lines(const Table *t, const Config *c, Err *err)
{
    int i, j;
    for (i = 0; i < c->n_out; i++)
        for (j = i + 1; j < c->n_out; j++)
            if (!c->out[i].skip && !c->out[j].skip &&
                c->out[i].digital == c->out[j].digital &&
                c->out[i].index == c->out[j].index) {
                char sect[CFG_NAME + 8];
                snprintf(sect, sizeof sect, "out %s", c->out[j].name);
                return cfg_err(t, get(t, sect, "line"), err,
                               "line %s is already used by output %s", c->out[j].line,
                               c->out[i].name);
            }
    for (i = 0; i < c->n_in; i++)
        for (j = i + 1; j < c->n_in; j++)
            if (!c->in[i].skip && !c->in[j].skip && c->in[i].index == c->in[j].index) {
                char sect[CFG_NAME + 8];
                snprintf(sect, sizeof sect, "in %s", c->in[j].name);
                return cfg_err(t, get(t, sect, "line"), err,
                               "line %s is already used by input %s", c->in[j].line,
                               c->in[i].name);
            }
    /* A differential input uses a pair of pins: ai(N) and ai(N+8). */
    for (i = 0; i < c->n_in; i++) {
        char sect[CFG_NAME + 8];
        if (c->in[i].skip || strcmp(c->in[i].terminal, "DIFF") != 0) continue;
        snprintf(sect, sizeof sect, "in %s", c->in[i].name);
        if (c->in[i].index > 7)
            return cfg_err(t, get(t, sect, "line"), err, "a differential input is "
                           "ai0 ... ai7 (its pair is ai8 ... ai15)");
        for (j = 0; j < c->n_in; j++)
            if (j != i && !c->in[j].skip && c->in[j].index == c->in[i].index + 8)
                return cfg_err(t, get(t, sect, "terminal"), err, "DIFF uses ai%d as "
                               "its second pin, which input %s already uses",
                               c->in[i].index + 8, c->in[j].name);
    }
    return 0;
}

int config_build(const Table *t, Mode mode_flag, Config *c, Err *err)
{
    int i;
    char kind[16], name[CFG_NAME];
    int active_in = 0;

    memset(c, 0, sizeof *c);
    if (build_rig(t, mode_flag, c, err) < 0) return -1;

    for (i = 0; i < t->n; i++) {
        if (t->e[i].key[0]) continue;                /* section headers only */
        split_section(t->e[i].sect, kind, sizeof kind, name, sizeof name);
        if (strcmp(kind, "out") == 0) {
            if (build_out(t, t->e[i].sect, name, c, err) < 0) return -1;
        } else if (strcmp(kind, "in") == 0) {
            if (build_in(t, t->e[i].sect, name, c, err) < 0) return -1;
        } else if (strcmp(kind, "cell") == 0) {
            c->n_cells++;                            /* checked in M3b */
        } else if (strcmp(kind, "synapse") == 0) {
            c->n_synapses++;
        }
    }
    if (check_lines(t, c, err) < 0) return -1;

    /* The idle value of every analog output in use must be a legal voltage:
     * it is driven before, between and after trials.  Never silently clipped
     * (SPEC 3.13), so a rest that cannot be produced is refused now. */
    for (i = 0; i < c->n_out; i++) {
        const OutCh *o = &c->out[i];
        char sect[CFG_NAME + 8];
        Err why;
        if (o->skip || o->digital) continue;
        if (out_check_value(o, c->mode, o->rest, &why) < 0) {
            const Entry *en;
            snprintf(sect, sizeof sect, "out %s", o->name);
            en = get(t, sect, "rest");
            if (en) return cfg_err(t, en, err, "%s", why.msg);
            return cfg_err(t, header(t, sect), err, "the default rest of 0 is not "
                           "possible: %s", why.msg);
        }
    }
    for (i = 0; i < c->n_in; i++) active_in += !c->in[i].skip;
    if (active_in == 0)
        return err_set(err, "%s: no input to record: give at least one [in NAME] "
                       "section that is not skipped", t->path);
    return 0;
}

int config_check_rate(const Config *c, int64_t p, int64_t q, Err *err)
{
    int i, n = 0;
    for (i = 0; i < c->n_in; i++) n += !c->in[i].skip;
    /* The board's analog input limit is 500 kS/s shared by all the channels
     * in use (datasheet).  Compare p/q * n with 500000 exactly. */
    if (p > INT64_MAX / 16 || (double)p * n > 500000.0 * (double)q) {
        char r[40];
        units_format_rate(r, sizeof r, p, q);
        return err_set(err, "rate %s with %d analog input(s) is above the board's "
                       "500 kS/s total (%d channel(s) at most %.6g kHz each)", r, n, n,
                       500.0 / n);
    }
    return 0;
}
