# 11. Notes for AI assistants

Read this page first if you are an AI assistant picking up this project. It is
short on purpose. The authority is `SPEC.md` and the code; this page tells you
where things are and what to avoid.

## In one paragraph

LCG2 is a minimal ISO C99 command-line program that plays a StimGen 2 stimulus
on a data-acquisition board and records inputs. **Only milestones M0 and M1 are
built**: command line, INI configuration, profiles, checks, dry run (`-n`). It
does not call StimGen, play, simulate or record yet. The board (NI USB-6343) is
not supported by NI-DAQmx on Linux, so the **rig host is undecided** and
hardware work is on hold. The next job is M2.

## State at a glance

| Item | State |
|:--|:--|
| Repository | <https://github.com/mgiugliano/lcg2> |
| Built | M0 skeleton, Makefile, StimGen pin; M1 config, profiles, CLI, `-n` |
| Next | **M2** StimGen integration, then M3 engine and loopback, M3b NEURON, M4 HDF5, M5 test suite |
| On hold | S1 hardware spike, M6 DAQmx, M7 rig bring-up (host undecided) |
| Tests | `make test` (about 180 cases plus doc commands), all must pass |
| Build | `make` must print no warnings |

## Files to read, in order

1. `SPEC.md`: decisions (section 3), precedence rules (4), as-built config (4a),
   hardware facts (5), safety (6), HDF5 layout (7), simulators (11), milestones (12).
2. `docs/10-architecture.md`: the code layout and where M2 to M6 plug in.
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
- **The Dropbox folder** may have online-only files, which fail to read ("Resource
  deadlock avoided"). Make files local first.
- **`nrniv -nogui -c ...` hangs** reading stdin; use NEURON through Python 3.11.
- **inih r58** does not allow `MAX_SECTION` and `MAX_NAME` to be changed; a cut
  section name is refused because it is still longer than 47 characters.
- **StimGen** has stale `.o` files in `src/`; never link them.
- The shell hook may block `rm`; `make clean` works.

## What M2 must do (summary of `SPEC.md`)

1. A module that calls `sg_api_kind`, `sg_api_expand`, `sg_api_render_trial`, `sg_api_render`.
2. Combine the rate (stimulus, `-r`, `[rig] rate`) by cross-multiplying, and take the
   rate of a run from StimGen's rendered result; the error rules are in `SPEC.md` 4.1.
3. Map stimulus channels to `[out]` sections; an unknown channel is an error;
   unnamed outputs are idle.
4. Check each channel's unit against the output's by dimension, with rescaling.
5. Check **every sample of every trial** with `out_check_value()`; refuse before
   playing, naming trial, channel and first sample.
6. Add rest samples at the end of every buffer.
7. Tests: samples equal `sg render` on this machine; trial order and seeds equal
   `sg expand`; a clipping trial is refused before anything plays.

## Useful commands

```console
$ make && make test                 # build, run all tests
$ ./lcg2 -c examples/lcg2.ini -n -v -e x      # a dry run with the resolved configuration
$ python3 docs/figures/make_figures.py        # regenerate the figures
$ make check-pin                    # compare StimGen with the pin
```

## Unfinished business a new assistant should know

- StimGen's `parse_rate` bug (malformed exponents) was fixed in StimGen2 `379434b`, and the pin was updated.
- The earlier Python prototype (`lcg2.py`, `config.example.toml`) is superseded.
- The hardware decision (Windows or PCIe) controls S1, M6 and M7.
