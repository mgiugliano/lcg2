# Student kit: patching virtual cells

This folder is a self-contained **virtual patch-clamp rig**. It runs on the NEURON simulator through LCG2:
you describe a rig in a text file, play stimuli into a cell, record, and analyse, as on a real setup. You do
**not** need to know NEURON. You can copy this folder anywhere: paths in the files are relative to it.

## Set up (once)

1. Build LCG2 (`make` in the project folder) and note the path of the `lcg2` program.
2. A Python that has NEURON and h5py: `pip install neuron h5py`. Open `rig.ini` and set `sim_python` to that Python
   (`which python3`).
3. Make a folder for your data and work from it; every run writes a data file and a hidden folder there:

```console
$ mkdir ~/patching && cd ~/patching
$ LCG2=/path/to/lcg2; KIT=/path/to/examples/student_kit
$ $LCG2 -c $KIT/rig.ini -n $KIT/current_steps.sg        # builds the cell, plays nothing
```

## What is in the folder

| File | What it is |
|:--|:--|
| `rig.ini` | the rig: one pipette `P1` on one cell. Profiles `-P ballstick`, `-P pyramidal`, `-P hoc` swap the cell |
| `rig_pair.ini` | two cells with a synapse (patch both) |
| `rig_morphology.ini` | a cell made from a morphology file, with your own biophysics and initialisation |
| `current_steps.sg`, `vc_steps.sg`, `pair_pulse.sg` | stimuli: current steps, voltage steps (`-m vc`), a pulse |
| `characterise.py` | measures a recording: `fi`, `iv`, `psp`, `rin`, `sag` |
| `plot_run.py` | draws a recording (needs matplotlib) |
| `small_step.sg`, `sag_step.sg`, `current_steps_fine.sg` | more stimuli: input resistance, the sag, the firing threshold |
| `rig_ih.ini`, `soma_ih.py`, `channels/ih.mod` | a cell with your own channel (compiled automatically) |
| `rig_custom.ini`, `two_compartment.py` | a cell written from scratch in Python |
| `pyramidal.py`, `simple_cell.hoc`, `tiny.swc`, `cell_tuning.py`, `start_depolarised.py` | cell models, as examples to copy |

## Exercises

1. **The resting potential and the input resistance.** Run `current_steps.sg` on `rig.ini`, then
   `python3 $KIT/characterise.py fi FILE.h5`. Check the resting potential against the trace before the step
   (`python3 $KIT/../read_run.py FILE.h5`). Is the cell's input resistance what you expect for a 20 µm soma?
2. **The rheobase and the f-I curve.** Between which two currents does the cell start to fire? How does the rate
   grow? Edit the `sweep` line of `current_steps.sg` to look closer.
3. **Voltage clamp.** `$LCG2 -m vc -c $KIT/rig.ini $KIT/vc_steps.sg`, then `characterise.py iv`. At which
   potential is the inward current largest? Why is it negative? Where does it reverse?
4. **A different cell.** Repeat 1 and 2 with `-P ballstick`: where does the soma current go now? Then
   `-P pyramidal`. Open `pyramidal.py` and change `apical_length` in the `[cell P1]` section (add
   `apical_length = 600` to a profile) and see what changes.
5. **Two cells.** `rig_pair.ini` and `pair_pulse.sg`, then `characterise.py psp`. How large and how late is the
   answer? Change `weight`, `delay` and `type` in `[synapse P1_P2]`.
6. **Your own cell.** Copy `tiny.swc`, `cell_tuning.py`, `rig_morphology.ini`; edit the channels in
   `cell_tuning.py` (conductances, which sections have what) and repeat 1 and 2.
7. **A channel of your own.** `rig_ih.ini` and `sag_step.sg`, then `characterise.py sag`; compare `-P noih`; edit
   `channels/ih.mod` (the half-activation voltage) and run again. Tutorial H explains it.
8. **Your own analysis.** `read_run.py` and `characterise.py` are short: write the analysis you need (spike
   threshold, adaptation, the membrane time constant from a small step).

Things to keep in mind, as on a real rig: the **range** of an input clips what is too large (a warning says so);
the current clamp command is in pA, the voltage clamp command is in mV; each trial starts from where the previous
one left the cell, so a short `period` leaves a cell that has not recovered.

Step-by-step tutorials with figures: [`../../docs/16-neuron-tutorials.md`](../../docs/16-neuron-tutorials.md).
Reference: [`../../docs/14-neuron-models.md`](../../docs/14-neuron-models.md).
