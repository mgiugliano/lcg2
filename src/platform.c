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
 * platform.c -- everything that depends on the operating system.  See
 * platform.h.  POSIX only.
 */
#define _POSIX_C_SOURCE 200809L              /* sigaction, clock_gettime, nanosleep */

#if !defined(__unix__) && !defined(__APPLE__)
#error "platform.c is POSIX only so far. Port it when the machine that has the board is decided (SPEC 1)."
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "platform.h"

/* ------------------------------------------------------------------ */
/* The abort flag                                                      */
/* ------------------------------------------------------------------ */

/* sig_atomic_t is the one type a handler may safely write.  Static, so it
 * starts at 0. */
static volatile sig_atomic_t abort_flag;

static void on_signal(int sig)
{
    (void)sig;
    abort_flag = 1;                          /* nothing else: see platform.h */
}

void platform_install_abort_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a signal must interrupt a sleep or a read so that the code
     * waiting there sees the flag.  No SA_RESETHAND: the handler stays, so that a
     * second Ctrl-C does not kill the program before the outputs are at rest. */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int  platform_abort_requested(void) { return abort_flag != 0; }
void platform_request_abort(void)   { abort_flag = 1; }
void platform_clear_abort(void)     { abort_flag = 0; }

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

int64_t platform_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int platform_sleep_ns(int64_t ns)
{
    /* Sleep in slices of at most 20 ms, and look at the flag between them, so
     * that an abort is noticed even where a signal does not interrupt the sleep. */
    int64_t until = platform_monotonic_ns() + ns;
    for (;;) {
        int64_t left = until - platform_monotonic_ns();
        struct timespec ts;
        if (abort_flag) return 0;
        if (left <= 0) return 1;
        if (left > 20000000) left = 20000000;
        ts.tv_sec = (time_t)(left / 1000000000);
        ts.tv_nsec = (long)(left % 1000000000);
        nanosleep(&ts, NULL);                /* returns early on a signal: we loop */
    }
}

void platform_utc_now(char buf[32])
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

int platform_wait_enter(void)
{
    char line[256];
    for (;;) {
        if (abort_flag) return -1;
        if (fgets(line, sizeof line, stdin)) {
            if (strchr(line, '\n') || feof(stdin)) return 1;   /* a whole line, or the last one */
            continue;                                          /* a long line: keep reading it */
        }
        if (abort_flag) return -1;
        if (ferror(stdin) && errno == EINTR) { clearerr(stdin); continue; }
        return 0;                                              /* end of input */
    }
}

/* ------------------------------------------------------------------ */
/* The clock the engine uses                                           */
/* ------------------------------------------------------------------ */

void clock_init_real(Clock *c)
{
    c->is_virtual = 0;
    c->t = 0;
    c->origin = platform_monotonic_ns();
}

void clock_init_virtual(Clock *c)
{
    c->is_virtual = 1;
    c->t = 0;
    c->origin = 0;
}

int64_t clock_now(Clock *c)
{
    return c->is_virtual ? c->t : platform_monotonic_ns() - c->origin;
}

int clock_wait_until(Clock *c, int64_t t)
{
    if (abort_flag) return 0;
    if (c->is_virtual) {
        if (t > c->t) c->t = t;              /* time just passes */
        return 1;
    }
    {
        int64_t left = t - clock_now(c);
        if (left <= 0) return 1;
        return platform_sleep_ns(left);
    }
}
