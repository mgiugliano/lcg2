# 8. Simulators

> **Status.** Both backends are **built**: the built-in loopback (backend 1, milestone M3) and the NEURON
> simulator (backend 2, milestone M3b). The NEURON backend has its own page, with every key, the model
> contract and the limits: [14 Virtual cells](14-neuron-models.md). This page keeps the overview.

LCG2 is built **simulator first**: everything above the hardware is written and
tested with simulators before the board is touched. A *backend* replaces the
board. The command line, the stimuli, the configuration and the outputs are the
same for every backend. The interface (`src/backend.h`) is small and **provisional**: five
operations (`open`, `set_rest`, `wait_trigger`, `play`, `close`), which the hardware spike may refine.

## Backend 1: the built-in loopback (`device = sim`) — built

Every analog output `aoN` is wired to the analog input `aiN`, with a little noise. An input with no
output of its number hears only noise (or the rest value of an idle output of that number). It has
no dependencies and is used by the test suite; it tests the whole path (checks, scheduling, rest
values, abort, the streaming of the inputs) but nothing about a cell, so a recording from it is a
copy of the command, not a neuron.

What it does, as a board would, where that matters to a test of LCG2:

| Behaviour | Detail |
|:--|:--|
| **Wiring** | `ao0` to `ai0`, `ao1` to `ai1`, and so on; the volts are the volts that `stim_drive` made, so the unit conversions, the calibration and the gains are all exercised |
| **Noise** | Gaussian, 2 mV by default, on every input; **deterministic**: a function of (seed, trial, input), so the same command gives the same recording, and each trial has its own noise |
| **Input range** | an input has a range (0.2, 1, 5 or 10 V). A voltage beyond it is **clamped** and **counted**, per input and per trial, and the run prints a warning: the simulator never passes an out-of-range voltage on as if it were fine |
| **Start latency** | starting a trial can take time (zero by default; the tests inject some), which shows up as the *delay* of the trial |
| **Time** | virtual by default: nothing waits; every time is exact. `-R` makes it follow the clock |
| **Trigger** | with a `trigger = PFIn` in `[rig]` the edge is *simulated at once* (the simulator has no PFI line); without one, the run prompts and waits for Enter |
| **Memory of what it did** | the order of operations (open, rest, play, ...) and the last value of every output: the tests use it to prove that every way out ends at rest; with `-v` it prints where it left the outputs |

## Backend 2: NEURON (`device = neuron`) — built

The goal: **pretend** you have real cells on a real amplifier, and never write NEURON code. The program
receives its data from a NEURON simulation instead of from the board. With `device = neuron` the command
line, the stimuli, the `[out]`/`[in]` channel map, the profiles and the outputs are **exactly those of the
real rig**; only `device` and the `[cell]`/`[synapse]` sections differ.

* The cells and synapses are declared in the configuration (`[cell P1]`, `[synapse P1_P2]`), not in code.
  The default is a Hodgkin–Huxley soma (`model = hh_soma`); your own morphology (SWC), biophysics, initialisation
  code and `.mod` channels in NEURON Python or hoc can be supplied as files, with paths relative to the
  configuration file.
* A command in pA drives a current clamp, a command in mV a voltage clamp; an input in mV records the potential,
  an input in pA the electrode current. So `-m cc` and `-m vc` behave as on the rig.
* LCG2 starts the adapter `neuron/lcg2_neuron.py` once, in the Python of `sim_python`, and talks to it
  through two pipes, block by block (0.1 s of signal at a time). Time is virtual by default; between trials the
  cells keep running at the rest values, so their state carries over.
* The recording equals a plain NEURON script to 1e-9 mV (`tests/test_neuron.py`).
* `-n` builds the cells (and prints them) without playing anything.

Everything is on [14 Virtual cells](14-neuron-models.md), including the *Limits*: one electrode per cell, no
electrode capacitance or recording noise, not dynamic clamp, and nothing tested on Linux yet.

### Environment on the development Mac

- NEURON 8.2.6 runs under Python 3.11 at `/opt/homebrew/opt/python@3.11/bin/python3.11`
  (also h5py there). The default `python3` has no NEURON, so `sim_python` points to the 3.11 one.
- Running `nrniv -nogui -c ...` from a script hangs waiting for standard input; the adapter uses the Python
  module instead.

## What the tests check

`tests/test_neuron.py` (skipped, loudly, if no Python has both NEURON and h5py): equality with a plain NEURON
script; a negative (inward) transient in voltage clamp; a synaptic answer after its delay and none without
the synapse; state kept across a pause; Python, hoc, SWC + biophysics + init and `.mod` models, from another
folder; every mistake message; a killed adapter (exit status 2, completed trials readable); Ctrl-C (clean,
no adapter left behind); the archive (`network.json`, copied model files, `neuron.log`, all in `SHA256SUMS`).
