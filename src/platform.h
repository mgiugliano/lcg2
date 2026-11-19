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

#include <stddef.h>
#include <stdint.h>

/* Catch SIGINT, SIGTERM and SIGHUP (a closed terminal, a dropped ssh session) and turn them into the abort
 * flag.  A SIGHUP that is ignored (nohup) stays ignored.  main() installs them first thing, so that an abort
 * during the checks (-n included) also ends cleanly. */
void platform_install_abort_handlers(void);

int  platform_abort_requested(void);     /* 1 once Ctrl-C or kill has arrived  */
void platform_request_abort(void);       /* the same, from code (the tests)    */
void platform_clear_abort(void);

/* Nanoseconds on a clock that never goes back (not the wall clock). */
int64_t platform_monotonic_ns(void);

/* Sleep for ns nanoseconds, but return early (and say so) when an abort is
 * requested.  Returns 1 if it slept the whole time, 0 if it was cut short. */
int platform_sleep_ns(int64_t ns);

/* The wall clock: nanoseconds since 1970-01-01 UTC. */
int64_t platform_wall_ns(void);

/* The current time in the machine's local time as "20261118_142100": the stem of
 * the names of the files of a run. */
void platform_local_stamp(char buf[20]);

/* Make a folder.  Returns 0 if it was made, 1 if it already exists, -1 on any other
 * error (errno then says why).  Making a folder is atomic, which is what makes it a
 * safe way to claim a name: of two runs that start in the same second, one gets 1. */
int platform_mkdir(const char *path);

/* The bytes free for an ordinary user on the file system that holds `path`; -1 if unknown. */
int64_t platform_free_bytes(const char *path);

/* How many more bytes the file `path` can grow by: the free space of its file system, or less if the
 * process has a limit on the size of a file (RLIMIT_FSIZE); -1 if unknown. */
int64_t platform_file_room(const char *path);

/* The current working folder; 0 on success. */
int platform_getcwd(char *buf, size_t n);

/* A wall-clock time in nanoseconds as "2026-11-18T14:21:00.123Z" (milliseconds). */
void platform_format_utc(int64_t wall_ns, char buf[40]);

/* Wait for a line on standard input.  Returns 1 for Enter (a line), 0 for end
 * of input (nobody can ever press it), -1 if interrupted by an abort request. */
int platform_wait_enter(void);

/* Live mode (-L): the terminal on standard input gives single keys, not lines, and does not echo them.
 * platform_term_live(1) switches it (0 if standard input is not a terminal: then nothing changed),
 * platform_term_live(0) puts it back; it is also put back at exit, whatever ends the program.
 * platform_key() returns the next key pressed, or -1 if none is waiting (it never waits). */
int  platform_term_live(int on);
int  platform_key(void);

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

/* ------------------------------------------------------------------ */
/* A child process on two pipes (the NEURON adapter, device = neuron)  */
/* ------------------------------------------------------------------ */

/* A new empty file in the temporary folder ($TMPDIR or /tmp): its path goes to path.  0, or -1. */
int platform_temp_file(char *path, size_t n);

/* Delete a file (ignores a missing one). */
void platform_delete_file(const char *path);

/* Flush every stream and leave at once, without the exit handlers (a library's own cleanup may crash). */
void platform_exit_now(int status);

/* Ignore SIGPIPE, so that writing to a child that has died fails with EPIPE (which the caller
 * reports) instead of killing the program before it can put the outputs at rest. */
void platform_ignore_sigpipe(void);

/* Start argv[0] with the arguments argv (NULL-terminated; searched in PATH).  Its standard input and
 * output are two pipes (to_child, from_child); its standard error is appended to the file stderr_path.
 * The child is put in its own process group, so that Ctrl-C reaches only LCG2, which stops the child in
 * an orderly way.  Returns 0, or -1 with a message in msg.  If the program cannot be run the child
 * writes why to stderr_path and exits with status 127. */
int platform_spawn(char *const argv[], const char *stderr_path, long *pid, int *to_child, int *from_child,
                   char *msg, size_t n);

/* Read exactly n bytes, giving up after timeout_ms without any byte.  Returns 1 when all n came, 0 at
 * end of input (the child closed the pipe) , -1 on an error, -2 on a timeout, -3 if an abort was requested
 * while waiting (checked every 100 ms). */
int platform_read_exact(int fd, void *buf, size_t n, int64_t timeout_ms);

/* Write all n bytes to a pipe.  Returns 0, -1 on an error (EPIPE included), -2 if the reader took nothing for
 * timeout_ms (< 0: no limit), or -3 on an abort while the pipe was full: a reader that keeps reading is
 * written to even after an abort, so that the outputs can still be put at rest. */
int platform_write_all(int fd, const void *buf, size_t n, int64_t timeout_ms);

/* Close the pipes, ask the child to end (SIGTERM), wait up to grace_ms, then kill it, and reap it.  The
 * signals go to the child's process group (platform_spawn makes one), so that what it started stops too.
 * Returns the exit status, or 128 + signal, or -1 if there was nothing to wait for. */
int platform_child_stop(long pid, int to_child, int from_child, int grace_ms);

#endif /* LCG2_PLATFORM_H */
