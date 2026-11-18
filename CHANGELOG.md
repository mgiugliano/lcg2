# Changelog

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
