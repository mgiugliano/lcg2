# Examples

Rig configurations (`*.ini`) and StimGen 2 stimuli (`*.sg`). All of them are
used by the tutorials in [`../docs/03-tutorials.md`](../docs/03-tutorials.md), and
`make test` loads every `.ini` file here.

| File | What it is |
|:--|:--|
| `single_pipette.ini` | the smallest rig: one pipette, both modes |
| `lcg2.ini` | a two-pipette rig with a digital line and two profiles (`single`, `swap`) |
| `neuron_single.ini` | one virtual HH cell on the NEURON simulator (set `sim_python` to a Python with NEURON) |
| `neuron_pair.ini` | two virtual cells and a synapse on the NEURON simulator |
| `student_kit/` | a self-contained package for training in patching on virtual cells: rigs, stimuli, models, an analysis script and exercises |
| `models/` | model files for the NEURON backend: Python, hoc, SWC, biophysics, init, `.mod` |
| `read_run.py` | reads and summarises a data file (needs h5py) |
| `bad_rest.ini` | a deliberately wrong configuration (idle value above the limit); LCG2 refuses it at load |
| `low_limit.ini` | a deliberately tight limit (0.1 V): LCG2 refuses `step.sg` because its samples clip |
| `step.sg` | one current step (200 pA) on channel `P1`, for current clamp |
| `step_vc.sg` | the voltage-clamp twin: a 20 mV step on `P1`, for `-m vc` |
| `pair.sg` | pulses on `P1`, a held current on `P2` |
| `camera.sg` | a current command with a digital trigger line `TTL` |
| `steps_protocol.sg` | a protocol: a family of current steps, repeated and shuffled |
| `frozen_noise.sg` | a protocol: the same noise realisation 50 times |
| `ramp.sg` | a plain waveform, to be sent with `-T NAME` |
| `timing_period.sg`, `timing_gap.sg`, `timing_late.sg` | four 2 s trials with `period 5s`, `gap 2s`, and `period 1s` (a trial longer than its period): Tutorial 11 |

Try them with the dry run, for example:

```console
$ ./lcg2 -c examples/lcg2.ini -n examples/pair.sg
```

Check a stimulus with StimGen's own tool (`make` builds it into `build/sg`):

```console
$ build/sg check examples/pair.sg
```
