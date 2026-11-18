# 2. Quick start

This page takes about five minutes. You will build the program and run a *dry
run*: LCG2 reads a configuration, checks it, and prints what it would do.

> **Status.** What works today: the configuration is read and checked, the stimulus is
> read **through StimGen**, rendered, mapped to the outputs and checked, and the trials
> are **played on the built-in loopback simulator** (`-n` stops after the checks and prints
> the plan). **A run writes an HDF5 data file and a hidden run folder** in the current folder (milestone M4;
> see [13 Data format](13-data-format.md)).

## What you need

- A C99 compiler (`cc`; any recent clang or gcc), and `make`.
- **HDF5** (the C library, to build `lcg2`): `brew install hdf5` on the Mac, `sudo apt install libhdf5-dev` on
  Ubuntu. The Makefile finds `h5cc` and says what to install if it cannot.
- `python3`, only to run the tests and the example reader; the tests also need `h5py` (`pip install h5py`).
- The [StimGen 2](https://github.com/mgiugliano/StimGen2) sources, in a folder
  next to this one. The default is `../StimGen/src`; set `STIMGEN_SRC=...` if
  yours is elsewhere. LCG2 does not copy StimGen: it is compiled from there,
  after a check that it is the pinned version (see below).

```text
dev/
  lcg2/        this project
  StimGen/     the StimGen 2 repository (src/ is used)
```

## Build

```console
$ make
check-pin: StimGen sources match the pin
...
$ ls lcg2
lcg2
```

`make` does three things: it compares the StimGen sources with the fingerprint in
[`STIMGEN_PIN`](../STIMGEN_PIN) and stops if they differ; it builds the program
with `-std=c99 -pedantic -Wall -Wextra` (it should print no warnings), compiling
the StimGen library from its sources into `build/` and linking it in; and it builds
a few test tools, among them `build/sg`, StimGen's own command-line renderer.

> **If you have changed StimGen.** The pin protects you from a silently
> different renderer. After you change StimGen on purpose, `make` stops with
> "the StimGen sources ... differ from STIMGEN_PIN". Re-run the tests, then
> update `sources_sha256` (and `commit`) in `STIMGEN_PIN` using the command
> written in that file.

> **Building inside a Dropbox folder.** Dropbox can hold freshly written files, and the
> link then fails with `Resource deadlock avoided`. Keep the build folder outside it:
> `make BUILD=/tmp/lcg2-build` (and `LCG2_BUILD=/tmp/lcg2-build` for the tests
> `tests/test_stim.py`; `make test BUILD=...` does it for you).

## Run the tests

```console
$ make test
...
unit tests: ... checks, 0 failed
... tests passed
stimulus tests: ... checks, 0 failed
engine tests: ... checks, 0 failed
documents: ... commands run, ... skipped, 0 failed
```

## The first dry run

The folder [`examples/`](../examples/) has a one-pipette rig and a step
stimulus.

```console
$ ./lcg2 -c examples/single_pipette.ini -n examples/step.sg
LCG2 0.0.1, dry run: the plan

  configuration  examples/single_pipette.ini (profile: none)
  device         sim  (built-in loopback simulator)
  mode           cc
  rate           20 kHz (stated by the stimulus)
  seed           4177202958314570411 (drawn for this check; a run draws its own unless you give -s)
  work           play the stimulus in examples/step.sg
  stimulus       a stimulus with 1 channel(s), 40000 samples (2 s)
  channels       P1 (pA)  ->  [out P1] on ao0, peak 0.5 V of the limit 5 V
  check          1 trial(s) rendered and checked: no sample clips

  outputs
    P1       -> ao0  400 pA per volt, limit 5 V, rest 0 pA

  inputs (all are recorded)
    P1       <- ai0  100 mV per volt, range 10 V, RSE

nothing was played.
```

How to read it:

- **device sim**: the rig is the built-in simulator, not a board.
- **mode cc**: current clamp. Outputs carry current (pA), inputs record
  voltage (mV). Try `-m vc` to switch.
- **P1 -> ao0, 400 pA per volt**: the stimulus channel `P1` goes to analog
  output 0; 400 pA at the amplifier input corresponds to 1 V from the board.
  `limit 5 V` means LCG2 will never ask the board for more than 5 V on this
  line, which is 2000 pA here.
- **P1 <- ai0, 100 mV per volt**: analog input 0 is recorded, and 1 V at the
  board is 100 mV at the cell.
- **rate 20 kHz (stated by the stimulus)**: `step.sg` says `rate 20kHz` itself, and the
  stimulus's own rate always wins.
- **stimulus ... channels ... check**: LCG2 handed the text to StimGen, got the samples
  back, found the output of each channel, and checked every sample against the limits.
  Here the largest command is 0.5 V, well below the limit of 5 V.
- **nothing was played**: a dry run never touches hardware.

## Try the other flags

```console
$ ./lcg2 -c examples/single_pipette.ini -n -m vc examples/step_vc.sg
$ ./lcg2 -c examples/single_pipette.ini -n -s 42 -p cell3 examples/step.sg
$ ./lcg2 -c examples/single_pipette.ini -n -t 30s
$ ./lcg2 -c examples/single_pipette.ini -n -v examples/step.sg
```

`-v` with `-n` also prints the *resolved configuration*: the file after the
profile and the `-m` flag have been applied. This is the text the run archive
will store as "the configuration used".

## Run it on the simulator

Without `-n`, LCG2 plays the trials:

```console
$ mkdir -p ~/lcg2_data && cd ~/lcg2_data           # a real run writes its files in the current folder
$ /path/to/lcg2 -c /path/to/examples/single_pipette.ini /path/to/examples/step.sg
lcg2: running 1 trial(s) on the loopback simulator, in simulated time (-R follows the clock); writing 20261118_164908.h5
lcg2: trial 1/1  stimulus  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: done: 1 trial(s) in 2.000 s (simulated), largest delay 0.000 s; wrote 20261118_164908.h5 and .20261118_164908/
```

(The stamp is the local time of your run.)

- **simulated time**: nothing waits. A two-second trial takes an instant, and every time
  in the report is exact. Add `-R` to follow the real clock instead.
- **the loopback**: each output is wired to the input of the same number (`ao0` to
  `ai0`), plus a little noise.
- **the data**: `20261118_164908.h5` holds the command and the recording of every trial; the hidden
  folder `.20261118_164908/` holds the configuration, the command, the stimulus, a log and checksums.
  Look inside with `python3 examples/read_run.py 20261118_164908.h5` (needs `pip install h5py`), and check
  nothing changed with `shasum -a 256 -c .20261118_164908/SHA256SUMS`. See [13 Data format](13-data-format.md).
- **Ctrl-C** stops the run safely: the outputs go to their rest values first (see
  [Concepts](04-concepts.md)).

## What is next

- [03 Tutorials](03-tutorials.md): the same steps one at a time, with multi-patch,
  profiles, digital lines and safety limits.
- [04 Concepts](04-concepts.md): the logic behind the numbers.
