# LCG2

A minimal command-line program for cellular electrophysiology, written in ISO
C99. It plays a [StimGen 2](https://github.com/mgiugliano/StimGen2) stimulus on
the analog and digital outputs of an acquisition board and records the analog
inputs at the same time. It is a stripped-down successor of the LCG software
(J Neurosci Methods): no dynamic clamp, no Comedi, no real-time kernel.

> **Status: early.** Milestones M0, M1 and M2 are built and tested: the program reads
> the command line and the configuration file, applies rig profiles, **reads the
> stimulus through StimGen**, renders every trial, checks units and every sample
> against the limits, and prints a plan (`-n`). **It does not play or record yet.**
> The trial engine and simulators (M3, M3b), HDF5 recording (M4) and the board (M6)
> are designed and documented, not written. Nothing in this repository touches
> hardware.

```console
$ make
$ ./lcg2 -c examples/single_pipette.ini -n examples/step.sg
LCG2 0.0.1, dry run: the plan

  configuration  examples/single_pipette.ini (profile: none)
  device         sim  (built-in loopback simulator)
  mode           cc
  rate           20 kHz (stated by the stimulus)
  ...
  stimulus       a stimulus with 1 channel(s), 40000 samples (2 s)
  channels       P1 (pA)  ->  [out P1] on ao0, peak 0.5 V of the limit 5 V
  check          1 trial(s) rendered and checked: no sample clips

  outputs
    P1       -> ao0  400 pA per volt, limit 5 V, rest 0 pA

  inputs (all are recorded)
    P1       <- ai0  100 mV per volt, range 10 V, RSE

nothing was played.
```

![Overview](docs/figures/fig-overview.svg)

## Idea in three lines

- The **stimulus** (StimGen text) says *what to play*, with channel names and no hardware.
- The **configuration** (INI) says *how the rig is wired*: which output carries `P1`, amplifier gains, safety limits.
- The **command line** says *what to do today*: current or voltage clamp, which profile, which rate.

## Documentation

Start at **[`docs/README.md`](docs/README.md)**: quick start, tutorials with
figures, the concepts, command-line and configuration references, the
simulator design, the hardware notes, an architecture guide for developers and
[a page for AI assistants](docs/11-for-ai-agents.md). The design itself is in
[`SPEC.md`](SPEC.md).

## Build and test

```console
$ make          # check the StimGen pin, build ./lcg2 and the test tools
$ make test     # run the tests (needs python3)
```

You need a C99 compiler and, next to this folder, the StimGen 2 repository
(`../StimGen`, or `make STIMGEN_SRC=path/to/StimGen/src`). StimGen is not copied
here: its version is pinned in [`STIMGEN_PIN`](STIMGEN_PIN) and checked at build time.

## Layout

```text
src/       the program (C99)          examples/  rig configs and StimGen stimuli
vendor/    inih (BSD), unmodified     docs/      the manual and its figures
tests/     acceptance tests           SPEC.md    the design and the milestones
```

## Licence

MIT, see [`LICENSE`](LICENSE). `vendor/inih` keeps its own BSD licence.
