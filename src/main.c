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
 * main.c -- LCG2: play a StimGen 2 stimulus and record, from the command line.
 *
 * main() reads and checks everything (session.c), and then either prints the plan
 * (-n) or runs the trials (run.c) on the loopback simulator, writing the data file
 * and the run folder.  It is the only place that prints a message for an error
 * found before the run starts and chooses the exit status; the modules below it
 * return -1 and a message instead.
 */
#include <stdio.h>
#include "platform.h"
#include "record.h"
#include "run.h"
#include "session.h"

/* Static, not on the stack: the session holds the table, a few hundred kilobytes. */
static Session session;

static int fail(const Err *e)
{
    fprintf(stderr, "lcg2: %s\n", e->msg);
    return 1;                                 /* error before the run */
}

int main(int argc, char **argv)
{
    Err err;
    int rc;
    RunParams rp;

    err.msg[0] = 0;
    platform_install_abort_handlers();       /* first: an abort during the checks (-n too) also ends cleanly */
    rc = session_prepare(&session, argc, argv, &err);
    if (rc < 0) return fail(&err);
    if (rc == 1) return 0;                    /* the help was printed */

    if (session.opt.dry) {
        if (session_print_plan(&session, &err) < 0) return fail(&err);
        if (session.opt.verbose) {
            printf("\nresolved configuration:\n\n");
            config_dump(&session.table, stdout);
        }
        return 0;
    }

    run_params_default(&rp, &session);
    rc = run_execute(&rp, &err);
    if (rc != 0 && err.msg[0]) fprintf(stderr, "lcg2: %s\n", err.msg);
    if (record_had_error()) platform_exit_now(rc);   /* HDF5 may crash in its exit handlers after a failed write */
    return rc;
}
