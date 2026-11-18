# 8. Simulators

> **Status: designed, not built (milestones M3 and M3b).** This page is the
> design, taken from `SPEC.md` section 11, so that the work can be picked up
> or reviewed. Only the configuration keys of the `[rig]` section are read today;
> the `[cell]` and `[synapse]` sections are counted, not validated.

LCG2 is built **simulator first**: everything above the hardware is written and
tested with simulators before the board is touched. A *backend* replaces the
board. The command line, the stimuli, the configuration and the outputs are the
same for every backend.

## Backend 1: the built-in loopback (`device = sim`)

Each output feeds the input of the same number, with a little noise. It has no
dependencies and is used by the test suite. Example: output `ao0` is played, and
the input `ai0` records the same signal plus noise. Unconnected inputs record
noise only.

It tests the whole path (checks, scheduling, rest values, recording, archive) but
nothing about a cell.

## Backend 2: NEURON (`device = neuron`)

The goal: **pretend** you have real cells on a real amplifier, and never write
NEURON code. The program receives its data from a NEURON simulation instead of
from the board.

### The transparency rule

With `device = neuron` the command line, the stimuli, the `[out]`/`[in]`
channel map, the profiles and the outputs are **exactly those of the real
rig**. Only `device` and the virtual-preparation sections differ. From the user's
side it is a real preparation on a real amplifier.

### Virtual preparation in the INI file

Cells and connections are declared in the configuration, not in code
([`examples/neuron_pair.ini`](../examples/neuron_pair.ini)):

```ini
[rig]
device     = neuron
sim_script = lcg2_neuron.py     ; the generic adapter, shipped with LCG2
mode       = cc

[cell P1]
model = hh_soma                 ; built in, or a path to a standard NEURON model

[cell P2]
model = hh_soma

[synapse P1_P2]
pre    = P1
post   = P2
type   = exp2                   ; exp or exp2
weight = 0.01
delay  = 1ms
```

- `[cell P1]`: the cell on pipette `P1`. `model` is a built-in model (for example
  `hh_soma`) or the path to a standard NEURON model (a `.hoc` template or a Python
  class) made by someone else. `section` and `position` say where the electrode
  sits (default soma, 0.5). Other keys are the model's own parameters.
- `[synapse P1_P2]`: connects cells. `pre`, `post`, `type`, `weight`, `delay`, time
  constants, reversal potential, and where on the post cell.
- Several cells and several synapses make a small network.
- The pipette's `[out P1]` and `[in P1]` lines are attached to that cell.
- Electrodes follow the amplifier mode: `-m cc` injects current and records
  voltage; `-m vc` clamps voltage and records current (series resistance,
  recording noise and electrode properties are optional keys, off by default).

### How it works

1. **A long-lived child process over pipes.** LCG2 starts the script once. For
   each trial it writes one StimGen `.sgb` record (the realised stimulus, in
   physical units) to the child's standard input, and reads one `.sgb`-format
   record of the response from its standard output. The channels of the response
   are named like the `[in]` sections and carry their unit. Records are
   self-delimiting. The child's stderr goes to the run log. The cell keeps its
   state between trials. It works trial by trial, not sample by sample: there is
   no dynamic clamp.
2. **The boundary is after the amplifier.** The script works in physical units
   (pA, mV); gains and calibration are bypassed. LCG2 still does the unit check
   and the clipping check against the volts it would have written.
3. **Virtual time.** No real waiting. The protocol's `period` or `gap` is passed to
   the script with the next trial as the simulated pause, and the model
   integrates through it. A `-realtime` option may slow it to real time. The
   delays recorded are simulated ones, marked as such.
4. **A generic adapter,** `lcg2_neuron.py`, ships with LCG2. It reads the `[cell]`
   and `[synapse]` sections, builds the network, converts units, runs each trial
   and writes the response. A model provider only supplies a standard NEURON
   model; nobody edits the adapter. Models that need compiled `.mod` files need a
   one-time `nrnivmodl`, which the adapter's `--check` option reports.

### Environment on the development Mac

- NEURON 8.2.6 runs under Python 3.11 at `/opt/homebrew/opt/python@3.11/bin/python3.11`
  (verified with a Hodgkin-Huxley soma). The default `python3` has no NEURON, so
  `sim_python` points to the 3.11 one.
- Running `nrniv -nogui -c ...` from a script hangs waiting for standard input;
  the adapter will use the Python module instead.

### Limits

A model slower than real time simply takes longer. A script crash or a closed
pipe aborts the run, as any failure does (exit 2). The simulator does **not** test
the behaviour of DAQmx (clock routing, FIFO size, start latency): that is the
purpose of the hardware spike.

## What the tests will check (M3b and M5)

- a protocol of current steps on an HH soma gives spikes at the expected
  amplitudes;
- with two cells and a synapse, only `P2` is driven while `P1` and `P2` are both
  recorded;
- a killed child aborts the run cleanly;
- `make test` passes with no hardware at all.
