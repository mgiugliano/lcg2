# 10. Architecture (for developers)

This page describes the code as it is today (M0, M1) and the places where the
designed parts will plug in. About 1,900 lines of C, including the licence
headers, and a few hundred lines of Python for the tests.

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
    main.c            glue: read, check, print the plan
  vendor/inih/        inih r58 (BSD), unmodified, with VENDOR.md
  tests/
    run_tests.py      the M1 acceptance test (a table of cases)
    test_docs.py      runs the commands shown in the documents
  examples/           rig configurations and StimGen stimuli
  docs/               this manual, figures/ (SVG), outputs/ (real program output)
  build/              compiled objects (not committed)
```

## Data flow

```text
argv ──cli_parse──> Options
file ──config_load──> Table ──config_apply_profile──> Table ──config_override(-m)──> Table
                                                                            │
                                                          config_build ──> Config
Options + Config ──check_plan──> OK / error ──> print_plan  (-n)   or  "not implemented"
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

### `main.c`

`main()` reads, checks, and either prints the plan (`-n`) or stops with
"not implemented yet". The `Table` and `Config` are `static` (the table is a few
hundred kilobytes).

## How the build works

`make` runs `check-pin` and then compiles `src/*.c` and `vendor/inih/ini.c` into
`build/` with:

```text
-std=c99 -pedantic -Wall -Wextra -O2 -ffp-contract=off
```

the same flags as StimGen's own Makefile, so that the samples will be the same.
inih is configured by macros (`INIFLAGS`); the same set must be used for `ini.c`
and for `config.c`, because `INI_HANDLER_LINENO` changes the handler's
prototype. `make stimgen-lib` compiles the StimGen library into
`build/stimgen/` (not linked yet).

## Tests

| Test | What it checks |
|:--|:--|
| `tests/run_tests.py` | about 180 cases: good and bad command lines and configurations, each with the exit status and a fragment of the message; the round trip of the resolved configuration; the pin check (a faithful copy accepted, a changed or missing file refused); all examples load |
| `tests/test_docs.py` | every `$ ./lcg2 ...` line in `docs/` and `README.md` runs with the expected exit status |

Other checks used during development (not in `make test`): a build with
`-fsanitize=address,undefined` run through the same table, a mutation fuzz of
configs and argument lists, and a comparison of the rate and duration parsers
with StimGen's own.

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
| M2 StimGen link | a new module (`stim.c`) calling `sg_api_*`; the unit and clipping checks reuse `out_check_value()`; `check_plan` gains the stimulus checks; the Makefile links `build/stimgen/*.o` |
| M3 engine, loopback | a `backend` interface (a struct of function pointers: write outputs, read inputs, set rest, close); `engine.c` for scheduling, rest values and abort; `platform.c` for signals, the clock and `mkdir` |
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
