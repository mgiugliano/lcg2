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
 * archive.h -- the run folder.
 *
 * Every run that plays something leaves a record next to its data file:
 *
 *     [PREFIX_]YYYYMMDD_HHMMSS.h5        the data (written by record.c)
 *     .[PREFIX_]YYYYMMDD_HHMMSS/         this folder, hidden, with the same stem
 *         config.ini                     the configuration as it was used (profile resolved)
 *         config_original.ini            the file as it was on disk
 *         command.txt                    the command line (quoted, so it can be run again) and the folder
 *         stimulus.sg                    the stimulus text as given
 *         canonical.sg                   the same, in StimGen's canonical form (self-contained)
 *         trials.json                    the trial list: seeds, timing (what `sg expand` gives)
 *         provenance.json                what produced the run (versions, pin, digests, seeds)
 *         files/NN_name                  every file the stimulus read (see below)
 *         log.txt                        trial starts and delays, warnings, how it ended
 *         SHA256SUMS                     the digest of the data file and of all the above
 *
 * `shasum -a 256 -c .STEM/SHA256SUMS`, run in the folder that holds the data
 * file, checks that nothing was changed afterwards.
 *
 * THE NAME IS CLAIMED BY MAKING THE FOLDER.  Making a folder is atomic, so of two
 * runs that start in the same second one gets a "-2" suffix instead of both
 * writing into the same place.  The data file is created afterwards, and must not
 * exist.
 *
 * FILES THE STIMULUS READ come from two sources.  StimGen lists the data files of
 * file(...) in the header of what it renders (with their digests, which LCG2 checks
 * against the copy it makes).  It does not list the files that `use` pulls in; for
 * those LCG2 scans the text for  use "name"  (and, in a protocol,  stimulus "name"),
 * and follows what it finds into the files themselves.  That scan is best effort.
 * What does not depend on it is canonical.sg: StimGen's canonical form has every
 * `use` expanded, so it reproduces the stimulus with no other file at all.
 */
#ifndef LCG2_ARCHIVE_H
#define LCG2_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "lcg2.h"

#define ARCHIVE_MAX_ITEMS 256

typedef struct {
    char    path[336];          /* relative to the working folder */
    char    sha[65];
    int64_t size;
} ArchiveItem;

typedef struct {
    char        stem[200];      /* "[PREFIX_]YYYYMMDD_HHMMSS" (with "-2" ... if the name was taken) */
    char        dir[210];       /* ".STEM"                                                          */
    char        h5[210];        /* "STEM.h5"                                                        */
    FILE       *log;
    int         nitem;
    ArchiveItem item[ARCHIVE_MAX_ITEMS];
    int         nfiles;         /* copies made in files/                                            */
    int         finished;
} Archive;

/* Claim a name and make the folder and the log.  prefix may be NULL.  Returns 0 or
 * -1 with a message (an unwritable folder, for example). */
int archive_create(Archive *a, const char *prefix, Err *err);

/* Write dir/name with these bytes, and remember its digest. */
int archive_put(Archive *a, const char *name, const void *data, size_t n, Err *err);

/* The digest of an item already put, or NULL. */
const char *archive_sha(const Archive *a, const char *name);

/* Copy a file into files/NN_name, remember its digest.  If expect_sha is not NULL
 * the copy must have exactly that digest, else the file changed under us: -1. */
int archive_copy(Archive *a, const char *src, const char *expect_sha, Err *err);

/* Find the files that `text` pulls in with  use "..."  or  stimulus "..."  and that
 * the files in turn pull in, and copy them (best effort: a note is logged for one
 * that cannot be read).  Returns how many were copied, or -1 on a real error. */
int archive_collect_uses(Archive *a, const char *text, Err *err);

/* One line in log.txt, flushed at once (so a killed run keeps its record). */
void archive_log(Archive *a, const char *fmt, ...);

/* After the data file is closed: remember its digest, write SHA256SUMS, log the
 * digests, close the log.  Returns 0 or -1 with a message. */
int archive_finish(Archive *a, Err *err);

/* The command line as one string that can be pasted into a shell. */
char *archive_quote_args(int argc, char **argv);

/* A growing string, for building JSON and text. */
typedef struct { char *s; size_t n, cap; int failed; } StrBuf;
void sb_add(StrBuf *b, const char *text);
void sb_addf(StrBuf *b, const char *fmt, ...);
void sb_json(StrBuf *b, const char *text);       /* a quoted, escaped JSON string; a byte that is not
                                                    part of UTF-8 is written as \u00XX (as Latin-1) */
void sb_free(StrBuf *b);

#endif /* LCG2_ARCHIVE_H */
