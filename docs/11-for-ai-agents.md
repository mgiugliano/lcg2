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
sink in physical units, and **every run writes an HDF5 file and a hidden run folder** (M4:
`record.c`, `archive.c`, `run.c`, `sha256.c`; format in `docs/13-data-format.md`). The board
(NI USB-6343) is not supported by NI-DAQmx on Linux, so the **rig host is undecided** and hardware work
is on hold. The next job is M3b (NEURON) or M5 (the test suite as a whole); ask the owner first.

## State at a glance

| Item | State |
|:--|:--|
| Repository | <https://github.com/mgiugliano/lcg2> |
| Built | M0 skeleton and pin; M1 config, profiles, CLI; M2 StimGen link; M3 engine, platform layer, loopback; M4 HDF5 recording, run folder, checksums |
| Next | M3b NEURON, then M5 test suite (ask the owner before starting) |
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
- **HDF5:** the Makefile finds `h5cc` (on `PATH`, then the Homebrew kegs) and `make` prints an install hint
  if there is none (`libhdf5-dev` on Ubuntu provides it). Tests need `h5py`. The file format is held
  between HDF5 1.8 and 1.10 on purpose (`LCG2_LIBVER_LOW`, see 13); do not change it without re-measuring.
- **A real run writes a data file and a hidden folder in the current folder.** In tests, run in a scratch folder;
  `tests/check_clean.py` fails if the repository is dirty. Run `make sanitize` (ASan/UBSan) after changes.
- **Do not include `sg.h`; StimGen exports `sha256_hex`,** so LCG2's own digest functions are `lcg_sha256_*`.
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

## What M4 does (built; the design is in `SPEC.md` 3, 4d and 7)

1. `record.c` is a `Sink` that writes one HDF5 file per run (layout in `docs/13-data-format.md`), flushed
   after every trial, with the `incomplete` flag set at the start of a trial and cleared when it ends complete.
2. `archive.c` makes the hidden folder (atomic `mkdir`), the line-by-line log and `SHA256SUMS`; `sha256.c` is
   the own SHA-256; `run.c` orders it all (free-space check, folder, engine, close, digests) and sets the
   exit status (2 if the integrity record or the final rest fails).
3. `session.c` holds what `main.c` used to do (command line, configuration, checks); `main.c` is thin.
4. Tests: `tests/test_record.py` (100 checks, h5py), `examples/read_run.py` is the reference reader.

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
