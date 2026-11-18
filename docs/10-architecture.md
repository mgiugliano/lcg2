# 10. Architecture (for developers)

This page describes the code as it is today (M0 to M2) and the places where the
designed parts will plug in. About 3,000 lines of C, including the licence
headers and the test drivers, and about a thousand lines of Python for the tests.

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
    main.c            glue: read, check, print the plan
  vendor/inih/        inih r58 (BSD), unmodified, with VENDOR.md
  tests/
    run_tests.py      the M1 acceptance test (a table of cases)
    test_stim.py      the M2 acceptance test (StimGen integration)
    test_docs.py      runs the commands shown in the documents
    unit.c            unit tests of the limit check and the rate fraction
    stimdump.c        driver: dumps what stim.c decoded, to compare with sg
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
all checks passed ──> print_plan  (-n)   or  "not implemented yet"
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

### `main.c`

`main()` reads, checks (the configuration, then the whole stimulus, trial by trial), and
either prints the plan (`-n`) or stops with "not implemented yet". The `Table`,
`Config`, `Stim` and report are `static` (the table is a few hundred kilobytes).

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
| `build/unit` (`tests/unit.c`) | checks of `out_check_value` (exact boundaries, NaN and infinity, a negative calibration scale), of the exact rate fraction, and of the **error trap**: stimuli that StimGen rejects in seven different ways, alternating with good ones in one process, must each come back as an error and never end the program |
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

## Where the designed parts plug in

| Milestone | Where it goes |
|:--|:--|
| M3 engine, loopback | a `backend` interface (a struct of function pointers: write outputs, read inputs, set rest, close) that takes a `Drive`; `engine.c` for scheduling (the timing strings of a protocol are already in `Stim`), rest values and abort; `platform.c` for signals, the clock and `mkdir` |
| M3b NEURON | `backend_neuron.c` (child process and pipes, POSIX only) and `lcg2_neuron.py` |
| M4 recording | `record.c` (HDF5 serial API, calls available since 1.10) and `archive.c` for the hidden folder, the log and the SHA-256; on the Mac HDF5 is the parallel build, so use `mpicc` |
| M6 DAQmx | `backend_daqmx.c`; the Mac build uses a stub `NIDAQmx.h` |

## Style

- ISO C99, heavily commented; comments say *why*.
- No `getopt`, no `strdup`, no `strcasecmp`, no POSIX calls outside `platform.c`
  (when it exists) and the NEURON backend.
- Fixed-size buffers with explicit limits and a clear error when exceeded; no
  silent truncation anywhere.
- Every source file starts with the MIT notice. `vendor/inih` keeps its own BSD licence.
