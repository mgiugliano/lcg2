# Changelog

## Unreleased

Fixes from the audit of 2026-11-18, the pipette and amplifier model of the NEURON backend. Results without electrode keys are unchanged.

- **Fixed: a resistive electrode read one step late.** With `electrode_resistance` and no `electrode_capacitance` the recording used the
  cell's potential from the start of the step; a balanced bridge differed from the ideal recording by up to 7 mV during a step. The
  electrode node is now solved exactly over each step: the balanced recording equals the ideal one to 1e-14 mV, and the neutralised
  transient of tutorial J1 no longer depends on `sim_dt` (0.19 mV, not 0.32 mV).
- **Fixed: the electrode started at −65 mV** whatever `sim_v_init` and the `init` hooks set; it now starts at rest on the cell.
- **Fixed: a pipette that only records** (an `[in]` without `[out]`, such as a `same_as` electrode) ignored its
  `liquid_junction_potential` and `amplifier_bandwidth` unless another pipette had an electrode model.
- **Fixed: `d_lambda` counted segments before the biophysics file set `Ra` and `cm`**, so a morphology could get too few segments.
- **Changed: the amplifier filter is now accurate to 0.25 %.** The bilinear transform was 17 % off at twice the corner at the limit
  allowed (corner × `sim_dt` = 0.1), and the documented gain at twice the corner (0.20) was wrong: the analogue Bessel has 0.2137.
  Within trials the step is now made small enough automatically (corner × step ≤ 0.0125) instead of refusing a corner; above 100 kHz is
  refused.
- **Changed: the noise passes the filter**, as on a rig: with `amplifier_bandwidth` the noise is drawn at every step with the density
  that gives `recording_noise` rms without the filter, and filtered (0.3 mV rms becomes 0.137 mV through 2 kHz at 20 kHz). Without a
  filter nothing changes.
- **Changed: series-resistance compensation is realistic**: it acts through a low-pass, the new key `rs_lag` (default 10 µs; 0 gives an
  ideal compensation, `(1 − α) Rs`), and rings when it is large, as the circuit does (tested against its exact solution). Before, its
  only lag was one NEURON step, so the ringing depended on `sim_dt`. Pauses between trials run at `sim_dt`.
- **Changed:** `vc_steps.sg` holds −70 mV between trials (it held 0 mV); `rig_electrode.ini` has a profile `vc_cp`; `aec.py` calls
  its time constant that of the kernel's slow tail (with a pipette capacitance it is longer than the membrane's); tutorials J and K
  and their figures are from new runs, and their explanations corrected; 13 new checks against independent references.

## 0.3.0 (2026-11-18)

- **Added: seal resistance, liquid junction potential and amplifier bandwidth** on the NEURON backend (`[cell]` keys `seal_resistance`,
  `liquid_junction_potential`, `amplifier_bandwidth`): a seal leak to the bath (a depolarising current and a lower input resistance), a
  constant offset of the recorded potential (and of the clamped one), and a 4-pole Bessel output filter that rounds fast transients.
  Defaults are off, and the results without the keys are unchanged. Tutorial K, a figure, profiles in `rig_electrode.ini`; 9 new checks
  (106 in the NEURON suite).
- **Added: `LCG2_SIM_PYTHON`**, an environment variable that overrides `sim_python`, and the example rigs now say `sim_python = python3` (the
  virtual environment's Python), so the same files work on every machine; `make doctor`; pinned `requirements*.txt`.
- **Added:** the manual as a PDF (`make pdf`, built with Pandoc: `docs/pdf/`), and a detailed installation guide in the README.

## 0.2.1 (2026-11-18)

Found by running the whole `make test` on Linux (Ubuntu 22.04, gcc, HDF5 1.10.7, NEURON 9.0.2), which passes now:

- **Fixed a crash on Linux:** `strdup` has no prototype under `-std=c99` on glibc, so its pointer was cut to 32 bits and every real run on
  `device = neuron` segfaulted in `read_reply`. (The Mac's libc declares it anyway; the compiler had warned.)
- **Fixed a crash at exit after a failed HDF5 write** (a full disk): HDF5 1.10 crashed in its own cleanup after everything had been
  recorded, losing the exit status. After an HDF5 failure the program now leaves without running the exit handlers.
- Tests: a model fixture with a Mac path, a tolerance that NEURON 9's settling drift exceeded, and the example configurations naming the
  Mac's Python now skip instead of failing on another machine.

## 0.2.0 (2026-11-18)

- **Added: the pipette and the amplifier** on the NEURON backend (`[cell]` keys `electrode_resistance`,
  `electrode_capacitance`, `recording_noise`, `noise_seed`, `bridge_balance`, `cap_neutralization`, `rs_compensation`,
  `cp_compensation`): a lumped electrode (Re, Ce) with the amplifier's bridge balance and capacitance neutralisation in
  current clamp, series-resistance and pipette-capacitance compensation in voltage clamp, and reproducible white recording noise.
  Defaults are an ideal electrode, and the results without the keys are unchanged to the last bit.
- **Added: `same_as`**, a second electrode on an existing cell (dual recordings; a ground-truth monitor).
- **Added:** `examples/student_kit/` `rig_electrode.ini`, `rig_aec.ini`, `aec_noise.sg`, `aec.py` (active electrode compensation,
  Brette et al. 2008, offline), tutorial J in `docs/16`, four figures; 19 new checks (96 in the NEURON suite).

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

Not yet: the DAQmx backend (M6), bring-up on the rig (M7).
