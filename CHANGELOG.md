# Changelog

## 0.1.1 (2026-11-18)

- **Fixed:** with a morphology file, the built-in `biophysics = hh_soma` (and the kit's `cell_tuning.py`) compared
  sections with `is`, which is never true for NEURON sections, so the soma got no Hodgkin-Huxley channels and the cell was
  purely passive. Found while drawing the tutorial figures; now tested (the morphology cell fires).
- **Changed:** the parameters `L` and `Ra` of `hh_soma` are now `length` and `ra` (and `dend_L`, `dend_Ra` of
  `ball_and_stick` are `dend_length`, `dend_ra`): configuration keys are lower case, so the old names could not be
  written in a file at all.
- **Added:** `docs/16-neuron-tutorials.md`, nine tutorials with figures drawn from real runs
  (`docs/figures/make_neuron_figures.py`); more kit files (`small_step.sg`, `sag_step.sg`, `current_steps_fine.sg`,
  an Ih channel and cell, a from-scratch Python cell, profiles `big`, `hot`, `ballstick_dend`, `strong`, `noih`,
  `short`, `long`); `characterise.py rin` and `sag`; `plot_run.py`. The tests check every number the tutorials quote.

## 0.1.0 (2026-11-18)

The first complete release of the software side. **No hardware backend**: the board (NI USB-6343) waits for the
rig host to be decided.

- **Configuration and command line** (M0, M1): INI file, rig profiles, `-m cc|vc`, units, safety limits, `-n` dry run.
- **StimGen 2 link** (M2): stimuli and protocols rendered through StimGen's `sg_api_*` only (pinned commit); every
  sample checked before anything is played; nothing is silently clipped.
- **Trial engine and loopback simulator** (M3): exact `period` / `gap` / trigger timing, virtual or real time (`-R`),
  rest values on every way out, Ctrl-C and `kill` handled safely, inputs streamed in blocks.
- **Recording** (M4): one HDF5 file per run and a hidden run folder with the configuration, command, stimulus, trials,
  provenance, log and SHA-256 checksums; completed trials survive abort, `kill -9` (almost always) and a full disk;
  free-space check before the run.
- **NEURON simulator** (M3b): `device = neuron`, virtual cells and synapses from the configuration; built-in Hodgkin–Huxley
  soma and ball-and-stick, or your own Python, hoc, SWC, biophysics, init code and `.mod` channels; a student kit.
- **Test suites** (M5): about 830 checks, no hardware needed, also under AddressSanitizer and UBSan.
- **Documentation** (M8): manual, tutorials, figures, references, a page for AI assistants.

Not yet: the DAQmx backend (M6), bring-up on the rig (M7), Linux testing of the NEURON backend.
