# 11. Notes for AI assistants

Read this page first if you are an AI assistant picking up this project. It is
short on purpose. The authority is `SPEC.md` and the code; this page tells you
where things are and what to avoid.

## In one paragraph

LCG2 is a minimal ISO C99 command-line program that plays a StimGen 2 stimulus
on a data-acquisition board and records inputs. **Milestones M0 to M3 are built**: command
line, INI configuration, profiles, the StimGen link (`stim.c`), and the **trial engine** with a
built-in **loopback simulator**: a run plays the trials on `device = sim` with StimGen's timing rules,
outputs held at rest between trials and on every way out, safe Ctrl-C, and the inputs streamed to a
sink in physical units. **No data file is written yet** (the sink only counts): that is M4. The board
(NI USB-6343) is not supported by NI-DAQmx on Linux, so the **rig host is undecided** and hardware work
is on hold. The next job is M4 (HDF5 recording and the run folder).

## State at a glance

| Item | State |
|:--|:--|
| Repository | <https://github.com/mgiugliano/lcg2> |
| Built | M0 skeleton and pin; M1 config, profiles, CLI; M2 StimGen link; M3 engine, platform layer, loopback |
| Next | **M4** HDF5 + run folder (a `Sink`), then M3b NEURON, M5 test suite |
| On hold | S1 hardware spike, M6 DAQmx, M7 rig bring-up (host undecided) |
| StimGen pin | StimGen2 `379434b` (`STIMGEN_PIN`); `make` stops if `../StimGen/src` differs |
| Tests | `make test`: unit tests, M1 table, M2 suite, M3 suite, doc commands; all must pass |
| Build | `make` must print no warnings |

## Files to read, in order

1. `SPEC.md`: decisions (section 3), precedence rules (4), as-built config (4a),
   hardware facts (5), safety (6), HDF5 layout (7), simulators (11), milestones (12).
2. `docs/10-architecture.md`: the code layout and where M3 to M6 plug in. Then `docs/07-stimgen.md`, which tells the whole StimGen story.
3. `src/config.h`: the file format, the types, the output-limit helpers.
4. `tests/run_tests.py`: the behaviour as a table of examples.

## Rules (do not break these)

- **Never edit StimGen** and never write inside `../StimGen`. LCG2 only calls
  functions named `sg_api_*`. The StimGen version is pinned in `STIMGEN_PIN`.
- **Never drive hardware** without the owner's agreement. The S1 spike needs an
  AO0-to-AI0 cable and a go-ahead.
- **Nothing silent.** No clipping, no truncation, no guessed defaults for safety
  items (that is why `mode` has no default).
- **Only `main()` prints errors and exits.** Everything else returns `-1` and an `Err`.
- **ISO C99**, `-std=c99 -pedantic -Wall -Wextra`, zero warnings; same flags as
  StimGen (`-ffp-contract=off`) so rendered samples match.
- **Every bug fixed gets a test** in `tests/run_tests.py`.
- **Do not claim more than exists.** Documents must say *built* or *designed*.
- **Ask the owner before starting each milestone**; they supervise the work.
- Do not delete `lcg2.py` or `config.example.toml` (an earlier Python prototype, kept
  out of git) unless the owner says so.

## Decisions already made (do not re-open without a reason)

| Decision | Detail |
|:--|:--|
| Input is StimGen **text** | via the library; `.sgb` input is out of scope for now |
| Config format | INI, parsed with inih (bundled, r58) |
| Recording format | HDF5, serial API, physical units |
| Run archive | `[PREFIX_]YYYYMMDD_HHMMSS.h5` plus hidden `.[PREFIX_]YYYYMMDD_HHMMSS/` |
| Re-addressing | profiles (`-P`) and `-T` for plain waveforms |
| Multi-patch | a stimulus may name any subset of outputs; idle ones hold rest; all inputs are recorded |
| Gain | channel units per volt, per mode |
| Clipping | checked for every sample of every trial *before* anything plays |
| Abort | handler sets a flag only; outputs to rest; exit 2 |
| Simulator first | built-in loopback, then NEURON through a long-lived child over pipes in virtual time |
| NEURON use | transparent: the user never writes NEURON code |

## Choices still waiting for the owner's confirmation

- the calibration formula `v_true = cal_scale * v_nominal + cal_offset` (Geko
  defined the keys but never applied them);
- `mode` required with no default; `-t` needs a unit; `+` signs refused; flags
  cannot be combined; names are identifiers of at most 47 characters; `-n` prints
  to stdout; `DIFF` pairs `ai(N)` with `ai(N+8)`;
- where the board will run (Windows, or a PCIe-6343 on Linux).

## Traps found in this environment

- **The shell is zsh.** Variables are not word-split: use `bash -c` for loops over
  flag lists.
- **macOS has no `timeout`.** Use Python's `subprocess` timeout.
- **`sed -i` needs a suffix argument on macOS.** Prefer Python for edits.
- **HDF5 on the Mac is `hdf5-mpi`:** its `hdf5.h` includes `mpi.h`, so build with `mpicc`.
- **Linking in the Dropbox folder fails** with "Resource deadlock avoided" because Dropbox holds
  new files: build with `make BUILD=/some/folder/outside/Dropbox` and run `tests/test_stim.py`
  with `LCG2_BUILD` set to it (`make test BUILD=...` does both).
- **The Dropbox folder** may have online-only files, which fail to read ("Resource
  deadlock avoided"). Make files local first.
- **`nrniv -nogui -c ...` hangs** reading stdin; use NEURON through Python 3.11.
- **inih r58** does not allow `MAX_SECTION` and `MAX_NAME` to be changed; a cut
  section name is refused because it is still longer than 47 characters.
- **StimGen** has stale `.o` files in `src/`; never link them. Building `sg` is done into `build/`.
- **`sg_api_*` results are valid only until the next call**: copy them out at once.
- **The simulator's state is observable**: `-v` prints where it left the outputs; `build/enginetest`
  prints its event log (`O R P R ... R C`) and the final value of every output. Use them.
- **Virtual time is the default** for `device = sim`; tests that need real time use `-R` with loose bounds
  and never depend on sleeping for correctness. Real-signal tests block on the Enter prompt (`start on
  trigger` with no `trigger` key) instead of racing a clock.
- **A list of arrays handed to a callback must end with NULL** (an uninitialised tail crashed only the
  optimised build once).
- **A stimulus rendered without a seed draws a new one each time**: `stim_open` fixes the seed; do
  not call `sg_api_render_trial` yourself with an empty seed.
- The shell hook may block `rm`; `make clean` works.

## What M4 must do (summary of `SPEC.md` sections 3 and 7)

1. A `Sink` (`engine.h`: `begin`, `block`, `end`) that writes one HDF5 file per run, one group per
   trial, extendable datasets filled as the blocks arrive, flushed after every trial; physical units; rate,
   gains, calibration, seed, start time (`TrialRecord.start_utc`, to the millisecond, simulated in virtual
   time), delay, latency, over-range counts and an `incomplete` flag as attributes.
2. The hidden folder `.[PREFIX_]YYYYMMDD_HHMMSS/` with the resolved configuration (`config_dump`), the command
   line, the stimulus text and the files it used (`Trial.file_path`, `file_sha`), the expanded trials, a log,
   and the SHA-256 of the data file and of every input. Own SHA-256 (no dependency).
3. `mkdir` and friends go into `platform.c`. On the Mac HDF5 is the parallel build: use `mpicc`; use only
   calls available since HDF5 1.10; the choice of compiler is made here, not before.
4. An aborted trial is kept and marked `incomplete`; a killed run keeps its completed trials.
5. Tests: `h5py` reads a file back (`pip install h5py`); a killed run keeps its completed trials; the hidden
   folder holds everything of SPEC 3.2.

## Useful commands

```console
$ make && make test                 # build, run all tests
$ ./lcg2 -c examples/lcg2.ini -n -v examples/step.sg   # a dry run with the resolved configuration
$ build/stimdump list /tmp -c examples/lcg2.ini -s 7 -r 20kHz examples/steps_protocol.sg   # the trial list
$ python3 docs/figures/make_figures.py        # regenerate the figures
$ make check-pin                    # compare StimGen with the pin
```

## Unfinished business a new assistant should know

- StimGen's `parse_rate` bug (malformed exponents) was fixed in StimGen2 `379434b`, and the pin was updated.
- The StimGen library leaks about 11 KB per rendered trial (all from its own allocators; found with
  `leaks`). LCG2 frees all it allocates. Worth reporting upstream.
- The earlier Python prototype (`lcg2.py`, `config.example.toml`) is superseded.
- The hardware decision (Windows or PCIe) controls S1, M6 and M7.
