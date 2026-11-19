# 10. Architecture (for developers)

This page describes the code as it is today (M0 to M3) and the places where the
designed parts will plug in. About 5,000 lines of C, including the licence
headers and the test drivers, and about 1,500 lines of Python for the tests.

## Repository layout

```text
lcg2/
  Makefile            build, pin check, tests
  STIMGEN_PIN         the StimGen version LCG2 was built against (commit + SHA-256)
  SPEC.md             the design: decisions, rules, milestones
  src/
    lcg2.h            shared: Err, err_set(), version
    err.c             err_set()
    units.h units.c   rates, durations, current and voltage units
    config.h config.c INI file: table, profiles, typed Config, limits, rate check
    cli.h cli.c       the command line
    json.h json.c     a small JSON reader (for the .sgb header and the trial list)
    stimgen_api.h     the sg_api_* functions LCG2 may call (sg.h is never included)
    stim.h stim.c     StimGen text -> trials -> unit-checked, limit-checked volts
    platform.h .c     the ONLY file that knows the operating system: abort flag and
                      signals, clocks (real and virtual), sleeping, Enter wait (POSIX only)
    backend.h         the provisional interface of a backend (open, set_rest,
                      wait_trigger, play, close; inputs streamed in blocks)
    backend_sim.h .c  the loopback simulator (device = sim)
    engine.h engine.c the trial engine: timing, rest on every way out, streaming
    main.c            glue: read, check, then print the plan (-n) or run
  vendor/inih/        inih r58 (BSD), unmodified, with VENDOR.md
  tests/
    run_tests.py      the M1 acceptance test (a table of cases)
    test_stim.py      the M2 acceptance test (StimGen integration)
    test_docs.py      runs the commands shown in the documents
    test_engine.py    the M3 acceptance test (engine, simulator, signals)
    unit.c            unit tests: limit check, rates, error trap, time arithmetic, timing parser
    stimdump.c        driver: dumps what stim.c decoded, to compare with sg
    enginetest.c      driver: runs the engine on the virtual clock and prints everything
    jsontool.c        driver: round-trips JSON through json.c
  examples/           rig configurations and StimGen stimuli
  docs/               this manual, figures/ (SVG), outputs/ (real program output)
  build/              compiled objects, the StimGen library, and the test tools
                      (build/sg, build/stimdump, build/jsontool, build/unit); not committed
```

## Data flow

```text
argv ──cli_parse──> Options
file ──config_load──> Table ──config_apply_profile──> Table ──config_override(-m)──> Table
                                                                            │
                                                          config_build ──> Config
Options + Config ──check_plan──> OK / error
text (file or -e) ──stim_open──> Stim ──stim_render (each trial)──> Trial
                    Trial + Config ──stim_map──> Map ──stim_check──> OK / refusal (every trial, before anything plays)
                    Trial + Map + Config ──stim_drive──> Drive      (volts and rest samples; for the backend, M3)
all checks passed ──> print_plan  (-n)   or  run_execute: folder + HDF5 file + engine + digests
```

- `Options` (`cli.h`): what the command line said. Values like the rate are
  already parsed (exact `p/q`, picoseconds).
- `Table` (`config.h`): a flat list of `(section, key, value, line)`. This is the
  "resolved configuration" that `-n -v` prints and that the archive will store.
- `Config` (`config.h`): typed and validated. Arrays of `OutCh` and `InCh`.
  Everything later (rendering, engine, backends) reads this, not the text.
- `check_plan` (`main.c`): the checks that need both the command line and the
  configuration: `-T`, a stimulus file that opens, an output to play on, the rate.

## Module notes

### `lcg2.h`, `err.c`

`Err` holds one message. Every function that can fail returns `-1` and fills an
`Err`; **only `main()` prints errors and chooses the exit status**. This matters
once the outputs are live: an error must return to a place that can put them
back to rest, never `exit()` from deep inside.

### `units.c`

Exact decimal parsing, no floating point. `split_decimal()` turns `"2.5e3"`
into mantissa and power of ten; rates become a reduced fraction `p/q`;
durations become integer picoseconds (like StimGen). Unit table: `fA pA nA uA mA A`,
`uV mV V`, the same names as StimGen's.

### `config.c`

Three steps (see `config.h` for the format and the key list):

1. `config_load` uses inih. The handler `ini_handler_fn` is called for every
   section header and key, with the line number. It checks names, refuses
   duplicates, and appends entries. A custom reader catches lines that are too
   long (inih would cut them silently).
2. `config_apply_profile` copies the entries of `[profile NAME]` over the
   targets. `config_override` does the same for `-m`, marking the entry
   (line 0), so the dump says `set on the command line`.
3. `config_build` reads each section: `build_rig`, `build_out`, `build_in`,
   then `check_lines` (no duplicate lines, DIFF pairs) and the rest-value checks.

The two limits of an output live in three small functions, **shared by the
idle-value check today and by the per-sample check in M2**:

```c
double out_wanted_volts (const OutCh *o, Mode m, double value);   /* value / gain               */
double out_written_volts(const OutCh *o, double wanted);          /* (wanted - off) / scale     */
int    out_check_value  (const OutCh *o, Mode m, double value, Err *err);
```

### `cli.c`

A hand-written parser (no `getopt`): one letter per option, values as separate
words, each option once, no combining. After parsing it checks the combinations
(exactly one of file, `-e`, `-t`; `-T` not with `-t`; `-q` not with `-v`).

### `json.c`

A recursive-descent JSON reader over a byte range (RFC 8259, nothing more). It records
where every node began and ended in the source, because the "info" object of a protocol
trial has to be handed back to StimGen unchanged. Nesting is limited to 64 levels, so a
damaged file cannot overflow the stack; numbers are doubles.

### `stim.c`

The only file that calls StimGen, through the functions declared in `stimgen_api.h`.
Read [07](07-stimgen.md) for the whole story; in short:

- `stim_open` classifies the text, expands a protocol (once), applies the rules for
  `-T`, learns the rate (trying `-r`, then the rate of the configuration) and fixes the seed.
- `stim_render` renders one trial and **decodes the `.sgb` bytes at once**, because the
  result of an `sg_api_*` call is valid only until the next call.
- `stim_map`, `stim_check`, `stim_drive` find the output of each channel and its unit
  factor, check every sample, and build the volts. The limit check uses the same
  `out_check_value()` as the idle values of the configuration.
- `sg_fail` rewrites StimGen's messages so that they name the user's file and speak of `-r`.

### `platform.c`

The only file that may call an operating-system function. POSIX only; any other system stops the
build with an `#error` (the machine that will have the board is not decided). It provides:

- **the abort flag**: SIGINT, SIGTERM and SIGHUP are caught with `sigaction` (a SIGHUP that is ignored, as
  under `nohup`, stays ignored), first thing in `main()` so that the checks and `-n` are covered too, with no
  `SA_RESTART` (so a signal interrupts a sleep or a read) and no `SA_RESETHAND` (so a second Ctrl-C does not
  restore the default and kill the program before the outputs are at rest). The handler only sets a
  `volatile sig_atomic_t`; everyone else polls it;
- **a child on two pipes** (the NEURON adapter): it leads its own process group, and stopping it signals the
  group, so that what it started (`nrnivmodl`, a compiler) stops too. A write to it waits only while the pipe
  has room, in pieces of `PIPE_BUF`, so an abort is heard even if the adapter has stopped reading;
- **clocks**: a monotonic one, and a `Clock` the engine uses, which is either *real* (waiting waits) or
  *virtual* (waiting just moves a counter: a thousand trials 5 s apart run in an instant, and every
  time is exact; tests depend on this);
- `platform_sleep_ns` (sleeps in slices of at most 20 ms and checks the flag), `platform_wait_enter`
  (tells Enter, end of input and an interrupt apart), and the wall clock to the millisecond.

### `backend.h`, `backend_sim.c`

A backend has five operations. `play()` runs one trial: it writes the outputs of a `Drive`, measures the
inputs, and hands them over **in blocks of about 0.1 s** through a callback, as volts. The streaming is
deliberate: the recording (record.c) grows on disk as it arrives, the completed part of an aborted trial is kept,
and a long record-only run never holds its whole record in memory. The interface is **provisional**: the
hardware spike may change it.

**Memory.** An output a trial does not name has no array in the `Drive` (read samples with `drive_ao()` and
`drive_dig()`), so a record-only run (`-t`) holds no trial-length array at all: 26 MB after 2 minutes at 100 kHz,
33 MB after 10 (a slow growth of about 1 MB per minute remains, not yet traced). The engine keeps one trial at a time, plus the rest values of the last one prepared. A stimulus trial
is held whole, twice (its samples and the volts that drive the outputs), and StimGen itself needs about three
copies of it while it renders: a single trial of 60 s at 100 kHz peaks at about 220 MB (measured 2026-11-19).
Computing the volts block by block would remove one copy; it would change what `play()` receives.

The loopback wires `aoN` to `aiN`, adds deterministic noise, clamps and counts samples beyond an
input's range, can be given a start latency, and keeps a log of what it did (`O` open, `R` rest, `T`
trigger, `P` play, `p` a play stopped early, `C` close) and of where every output was last driven.

### `engine.c`

`engine_run()` follows the order of things written at the top of `engine.h`:

1. the first trial is prepared (re-rendered, digest compared, re-checked, drive built) **before any
   output is touched**;
2. the backend is opened and every output is set to rest;
3. for each trial: prepare, wait for its time and its trigger, play, set rest;
4. whatever happens, **the last thing before closing is: outputs to rest**; if that fails, the message is
   loud (even with `-q`) and the exit status is 2.

Ownership is deliberately plain: trial *i* lives in `slot[i & 1]`, so while it is handled trial *i-1* is
still alive in the other slot, and `rest_drive` always points at the drive of the most recent trial that
was prepared (its rest samples are the state the outputs rest at). The sink is told about every trial that
began, finished or not. The list of input arrays given to the sink ends with a `NULL`.

The timing rules are StimGen's: `period T` counts from the previous trial's actual start, `gap T` from its
end, and `start on trigger` waits for a trigger *per trial* after that. All time arithmetic goes through
`units_muldiv` (a 128-bit intermediate), because `n * 10^12` overflows 64 bits for an hour at 500 kS/s.

### `main.c`

`main()` reads, checks (the configuration, then the whole stimulus, trial by trial), and either prints
the plan (`-n`) or runs the trials on the loopback simulator or the NEURON simulator (`run_execute`), refusing
board devices with a message for now. The `Table`, `Config`, `Stim` and report are `static`.

## How the build works

`make` runs `check-pin` and then compiles `src/*.c`, `vendor/inih/ini.c` and the
StimGen library into `build/` with:

```text
-std=c99 -pedantic -Wall -Wextra -O2 -ffp-contract=off
```

the same flags as StimGen's own Makefile, so that the samples will be the same.
inih is configured by macros (`INIFLAGS`); the same set must be used for `ini.c`
and for `config.c`, because `INI_HANDLER_LINENO` changes the handler's
prototype. The StimGen library is compiled into `build/stimgen/` from `STIMGEN_SRC` and
linked into `lcg2`; the same objects, with StimGen's own `main.c`, make `build/sg`, the
reference the tests compare with. Nothing is ever written inside the StimGen folder.

## Tests

| Test | What it checks |
|:--|:--|
| `tests/run_tests.py` | about 180 cases: good and bad command lines and configurations, each with the exit status and a fragment of the message; the round trip of the resolved configuration; the pin check (a faithful copy accepted, a changed or missing file refused; skipped with a note if your StimGen differs from the pin); all examples load |
| `tests/test_stim.py` | about 180 checks of the StimGen integration: samples equal `sg render` bit for bit (plain stimulus, protocols, plain waveform, `-e`, other units); trial order and seeds equal `sg expand`; two renders without a seed are identical; every combination of the rate rules; messages are translated; mapping, unit and clipping refusals; the volts and rest samples; the JSON reader against an independent parser, a table of hostile inputs and 500 mutations |
| `tests/test_engine.py` | about 95 checks of the engine and the loopback: exact timing for period, gap, a trial longer than its period, start latency and a trigger; one hour of simulated time; the order of operations and the final state of every output on **every way out** (normal, abort before, between and during a trial, a refused trial, a failing sink, a failing rest); UTC times to the millisecond; the loopback's numbers, noise, range clamping and calibration; the program's streams and exit statuses; and **real SIGINT and SIGTERM sent to the real program** (blocked on the Enter prompt, so nothing races), with the state of the outputs read back |
| `build/unit` (`tests/unit.c`) | checks of `out_check_value` (exact boundaries, NaN and infinity, a negative calibration scale), of the exact rate fraction, of the 128-bit time arithmetic and the timing parser, and of the **error trap**: stimuli that StimGen rejects in seven different ways, alternating with good ones in one process, must each come back as an error and never end the program |
| `tests/test_docs.py` | every `$ ./lcg2 ...` line in `docs/` and `README.md` runs with the expected exit status |

Other checks used during development (not in `make test`): the whole suite run against
builds made with `-fsanitize=address,undefined` (set `LCG2_BUILD` and `LCG2_BIN` to point
the tests at them), a mutation fuzz of configs and argument lists, the macOS `leaks` tool,
and a comparison of the rate and duration parsers with StimGen's own.

**Rule:** every bug fixed gets a case in `run_tests.py`.

## How to add a configuration key

1. Add it to the allowed list (`rig_keys`, `out_keys` or `in_keys`) in `config.c`.
2. Read and validate it in `build_rig`, `build_out` or `build_in`, with `cfg_err()` so the
   message names file, line and key.
3. Add the field to `Config` / `OutCh` / `InCh` in `config.h` and document it there.
4. Add cases to `tests/run_tests.py` (good and bad).
5. Document it in [06](06-config-reference.md) and in `SPEC.md` section 4a.

## How to add a command-line flag

1. Add a case to the `switch` in `cli_parse` (and to the list of value-taking letters).
2. Add the field to `Options` in `cli.h`, and to `cli_usage`.
3. Check its combinations at the end of `cli_parse`.
4. Add cases to the test table, and document it in [05](05-cli-reference.md).

Tests: see [15 Testing](15-testing.md).

## Where the designed parts plug in

| Milestone | Where it goes |
|:--|:--|
| M4 recording (built) | `record.c` is the `Sink` that writes the HDF5 file as blocks arrive; `archive.c` the hidden folder, log and checksums; `run.c` the order of events; `session.c` what `main.c` did before; `sha256.c`; `platform.c` gained `mkdir`, free space, local stamp |
| M3b NEURON (built) | `backend_neuron.c` (child process and pipes, POSIX only; `platform_spawn`, `platform_read_exact`, `platform_child_stop`) and `neuron/lcg2_neuron.py` (the adapter); see [14](14-neuron-models.md) |
| Patching, stage 1 (built) | the patching pipette in `neuron/lcg2_neuron.py` (`PatchElectrode`, a NEURON `LinearMechanism`); the membrane test in `mtest.c` (pure C, from the recorded data), run by the sink of `run.c` when an `[in]` has `membrane_test = yes`; see [14](14-neuron-models.md#patching-from-the-bath-to-whole-cell) |
| Patching, stage 2: live mode (built) | `-L` (`cli.c`); keys from the terminal (`platform_term_live`, `platform_key` in `platform.c`), read by the sink of `run.c` between blocks and sent through the backend's optional `event` operation (`backend.h`; `nrn_event` in `backend_neuron.c`, the adapter's `event` message); the replay profile written by `run.c` |
| M6 DAQmx | `backend_daqmx.c`; the Mac build uses a stub `NIDAQmx.h` |

## Style

- ISO C99, heavily commented; comments say *why*.
- No `getopt`, no `strdup`, no `strcasecmp`, no POSIX calls outside `platform.c`
  (when it exists) and the NEURON backend.
- Fixed-size buffers with explicit limits and a clear error when exceeded; no
  silent truncation anywhere.
- Every source file starts with the MIT notice. `vendor/inih` keeps its own BSD licence.
