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
 * platform.h -- everything that depends on the operating system, in one place.
 *
 * What lives here: the request to abort (Ctrl-C, kill), the clock, sleeping, and
 * waiting for the user to press Enter.  Nothing else in LCG2 may call a POSIX
 * (or Windows) function, so that moving to the machine that has the board
 * (Windows, or Linux with a PCIe card: SPEC 1) means rewriting this one file.
 *
 * It is POSIX only today.  platform.c stops the build with an #error elsewhere,
 * rather than carrying a Windows branch nobody has tested.
 *
 * THE ABORT FLAG.  A signal can arrive at any instant, so the handler does the
 * least a handler can do: it sets a flag.  The code that waits and the code that
 * reads poll the flag and do the real work (outputs to rest, close the file)
 * themselves, at a place where that is safe.  SIGINT (Ctrl-C) and SIGTERM (kill)
 * are caught.  A second Ctrl-C does not restore the default action: that would
 * end the program without putting the outputs at rest.  SIGKILL cannot be caught
 * by any program, so a killed -9 process leaves the outputs where they were: see
 * SPEC 6 and the bring-up tests of M7.
 */
#ifndef LCG2_PLATFORM_H
#define LCG2_PLATFORM_H

#include <stdint.h>

/* Catch SIGINT and SIGTERM and turn them into the abort flag. */
void platform_install_abort_handlers(void);

int  platform_abort_requested(void);     /* 1 once Ctrl-C or kill has arrived  */
void platform_request_abort(void);       /* the same, from code (the tests)    */
void platform_clear_abort(void);

/* Nanoseconds on a clock that never goes back (not the wall clock). */
int64_t platform_monotonic_ns(void);

/* Sleep for ns nanoseconds, but return early (and say so) when an abort is
 * requested.  Returns 1 if it slept the whole time, 0 if it was cut short. */
int platform_sleep_ns(int64_t ns);

/* The wall clock in UTC, "2026-11-18T14:21:00Z", for the record of a trial. */
void platform_utc_now(char buf[32]);

/* Wait for a line on standard input.  Returns 1 for Enter (a line), 0 for end
 * of input (nobody can ever press it), -1 if interrupted by an abort request. */
int platform_wait_enter(void);

/* ------------------------------------------------------------------ */
/* A clock the engine can read and wait on                             */
/* ------------------------------------------------------------------ */

/*
 * The engine schedules trials on a Clock.  The real clock follows time: waiting
 * really waits.  The virtual clock only counts: waiting just moves the count
 * forward, so a protocol of a thousand trials 5 s apart runs in an instant and
 * every time in it is exact.  The simulators use the virtual clock unless -R is
 * given (SPEC 11.3); the tests need it, because a test that depends on real
 * sleeping is a test that sometimes fails.
 */
typedef struct Clock Clock;
struct Clock {
    int      is_virtual;
    int64_t  t;                          /* virtual: the count, in ns           */
    int64_t  origin;                     /* real: the monotonic time at start   */
};

void    clock_init_real(Clock *c);       /* t = 0 now                           */
void    clock_init_virtual(Clock *c);    /* t = 0                               */
int64_t clock_now(Clock *c);             /* ns since the clock was started      */

/* Wait until time t (ns on this clock).  Returns 1 when it got there, 0 if an
 * abort request cut the wait short. */
int     clock_wait_until(Clock *c, int64_t t);

#endif /* LCG2_PLATFORM_H */
