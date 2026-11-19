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
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
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

void platform_local_stamp(char buf[20])
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, 20, "%Y%m%d_%H%M%S", &tmv);
}

int platform_mkdir(const char *path)
{
    if (mkdir(path, 0777) == 0) return 0;
    return errno == EEXIST ? 1 : -1;
}

int64_t platform_free_bytes(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return -1;
    return (int64_t)v.f_bavail * (int64_t)v.f_frsize;
}

int platform_getcwd(char *buf, size_t n)
{
    return getcwd(buf, n) ? 0 : -1;
}

int64_t platform_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void platform_format_utc(int64_t wall_ns, char buf[40])
{
    time_t secs = (time_t)(wall_ns / 1000000000);
    int ms = (int)((wall_ns % 1000000000) / 1000000);
    struct tm tmv;
    char date[24];
    gmtime_r(&secs, &tmv);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(buf, 40, "%s.%03dZ", date, ms);
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

static struct termios term_saved;
static int term_is_live;

static void term_restore(void)
{
    if (term_is_live) { tcsetattr(0, TCSANOW, &term_saved); term_is_live = 0; }
}

int platform_term_live(int on)
{
    static int registered;
    struct termios t;
    if (!on) { term_restore(); return 1; }
    if (!isatty(0) || tcgetattr(0, &term_saved) < 0) return 0;
    if (!registered) { atexit(term_restore); registered = 1; }
    t = term_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);      /* single keys, not echoed; Ctrl-C still interrupts */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) < 0) return 0;
    term_is_live = 1;
    return 1;
}

int platform_key(void)
{
    struct pollfd p;
    unsigned char c;
    p.fd = 0; p.events = POLLIN; p.revents = 0;
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return -1;
    if (read(0, &c, 1) != 1) return -1;
    return c;
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


/* ------------------------------------------------------------------ */
/* A child process on two pipes                                        */
/* ------------------------------------------------------------------ */

void platform_ignore_sigpipe(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

int platform_spawn(char *const argv[], const char *stderr_path, long *pid, int *to_child, int *from_child,
                   char *msg, size_t n)
{
    int in[2], out[2];
    pid_t p;

    if (pipe(in) < 0) {
        snprintf(msg, n, "cannot make a pipe: %s", strerror(errno));
        return -1;
    }
    if (pipe(out) < 0) {
        snprintf(msg, n, "cannot make a pipe: %s", strerror(errno));
        close(in[0]); close(in[1]);
        return -1;
    }
    p = fork();
    if (p < 0) {
        snprintf(msg, n, "cannot start a process: %s", strerror(errno));
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return -1;
    }
    if (p == 0) {                                   /* the child */
        int fd;
        setpgid(0, 0);                              /* its own group: Ctrl-C is for LCG2 */
        dup2(in[0], 0);
        dup2(out[1], 1);
        fd = open(stderr_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) dup2(fd, 2);
        for (fd = 3; fd < 256; fd++) close(fd);
        execvp(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(p, p);                                  /* (also from the parent, to avoid a race) */
    close(in[0]);
    close(out[1]);
    *pid = (long)p;
    *to_child = in[1];
    *from_child = out[0];
    return 0;
}

int platform_read_exact(int fd, void *buf, size_t n, int64_t timeout_ms)
{
    char *p = (char *)buf;
    size_t got = 0;
    int64_t waited = 0;

    while (got < n) {
        struct pollfd pf;
        int r;
        if (platform_abort_requested()) return -3;
        pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
        r = poll(&pf, 1, 100);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            waited += 100;
            if (timeout_ms >= 0 && waited >= timeout_ms) return -2;
            continue;
        }
        {
            ssize_t k = read(fd, p + got, n - got);
            if (k < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (k == 0) return 0;
            got += (size_t)k;
            waited = 0;
        }
    }
    return 1;
}

int platform_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int platform_child_stop(long pid, int to_child, int from_child, int grace_ms)
{
    int status = 0, waited = 0, done = 0;
    pid_t r;

    if (pid <= 0) return -1;
    if (to_child >= 0) close(to_child);              /* end of input: a well-behaved child ends by itself */
    if (from_child >= 0) close(from_child);
    for (;;) {
        r = waitpid((pid_t)pid, &status, WNOHANG);
        if (r == (pid_t)pid) { done = 1; break; }
        if (r < 0 && errno != EINTR) return -1;
        if (waited >= grace_ms) break;
        if (waited == grace_ms / 2) kill((pid_t)pid, SIGTERM);
        {
            struct timespec ts;                      /* not platform_sleep_ns: an abort must not cut this short */
            ts.tv_sec = 0; ts.tv_nsec = 10 * 1000000L;
            nanosleep(&ts, NULL);
        }
        waited += 10;
    }
    if (!done) {
        kill((pid_t)pid, SIGKILL);
        while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int platform_temp_file(char *path, size_t n)
{
    const char *tmp = getenv("TMPDIR");
    int fd;
    if (snprintf(path, n, "%s/lcg2_XXXXXX", tmp && *tmp ? tmp : "/tmp") >= (int)n) return -1;
    fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

void platform_delete_file(const char *path)
{
    unlink(path);
}

void platform_exit_now(int status)
{
    fflush(NULL);
    _exit(status);
}
