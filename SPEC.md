# LCG2 specification (working draft 3)

Status: interview done; reviewed by an independent Opus 5.5 reviewer on
2026-11-18, and its findings are applied here. Milestones M0 and M1 are built
and tested (section 12); section 4a records the choices made while building M1. Items under "Decided" are the
owner's choices. Items under "Defaults" are assumptions the owner may veto.
Items under "Verify on hardware" are untested beliefs about the board and driver.

## 1. Philosophy

- Command line, minimal, one job done well: render a StimGen 2 stimulus, play
  it on the outputs of a National Instruments USB-6343 and record the inputs at
  the same time. With no stimulus, record only.
- Out of scope: dynamic clamp, Comedi, real-time kernels, multitasking,
  graphics, directory-form protocols, `.sgb` input (a later addition).
- Language: ISO C99, heavily commented. Dependencies: NI-DAQmx C API, libm,
  serial HDF5, inih (bundled), StimGen's library (not bundled, see 8).
- Target: the rig host is **undecided** (2026-11-18). NI-DAQmx does not
  support the USB-6343 on Linux (cajal: driver installed and loaded, the board
  is seen on the bus but `nilsdev` lists nothing; NI forums say the same). The
  options are Windows with the USB-6343, or Linux with a PCIe-6343. The code
  therefore keeps everything OS-specific in one file (`platform.c`), parses
  `argv` by hand (no `getopt`) and uses ISO C99 elsewhere. Development is on
  macOS with simulator backends: a built-in loopback and the NEURON simulator
  driven by a user script (section 11). The hardware work (S1, M6, M7) waits
  for the rig host.
- New code base. Geko (../geko_contributors/geko) supplies ideas and parts
  (inih, run folder), not a base to extend.
- Licence: MIT, like StimGen; every source file carries the notice.

## 2. Command line

```
lcg2 [options] STIMULUS.sg          # play a stimulus or a protocol and record
lcg2 [options] -e 'TEXT'            # same, text given on the command line
lcg2 [options] -t DURATION          # record only (outputs at rest)
```

| Flag | Meaning |
|:--|:--|
| `-c FILE` | configuration file (default `lcg2.ini` in the current directory) |
| `-P NAME` | rig profile (section `[profile NAME]`) |
| `-m cc\|vc` | amplifier mode, selects the gain set |
| `-r RATE` | sampling rate (`20kHz`, `10000` = Hz); no sign, no malformed exponent |
| `-s SEED` | master (or protocol) seed, a non-negative integer; without it one is drawn and recorded |
| `-T NAME` | pipette that receives a plain waveform (channel `out`) |
| `-t DURATION` | record only, for that long (`30s`, `500ms`, `2min`); the unit is required, the value above 0 |
| `-e TEXT` | stimulus text inline, `;` separates segments (as in `sg`) |
| `-p PREFIX` | prefix of the output names |
| `-n` | dry run: check everything, print the plan, play nothing |
| `-q`, `-v` | quieter, more verbose |
| `-h` | help |

`-r`, `-s`, `-e` mean what they mean in StimGen's `sg` (the rate and duration
spellings were compared with `sg`'s own parser; where LCG2 is stricter, see 4a,
it is on purpose). `-t` does **not**: in `sg`, `-t` is `--text`, while here it
is the record-only duration. Options take their value as a separate word and cannot be
combined (`-n -v`, not `-nv`): `argv` is parsed by hand, not with `getopt`. Flags override the stimulus, which overrides the config, as noted in 4.

## 3. Decided

1. **Input.** LCG2 takes StimGen 2 *text* (stimulus, protocol or plain waveform)
   and renders it by calling the StimGen C library. Only `sg_api_*` entry points
   are used (they set the error trap, so an error returns instead of exiting
   with the outputs live; checked in M2). Trials of a protocol are rendered one
   at a time. StimGen is a separate repository and is never edited by LCG2.
2. **Run archive.** Every run writes a uniquely named data file and a hidden
   folder with the same stem: `[PREFIX_]YYYYMMDD_HHMMSS.h5` and
   `.[PREFIX_]YYYYMMDD_HHMMSS/`. The folder holds: the exact config used with
   the profile resolved; the exact command line; the stimulus text as given,
   every file it `use`s or reads with `file()`, and the expanded text and seed
   of each trial; and a text log (trial starts, delays, warnings) with the
   SHA-256 of the data file and of every input.
3. **Assignments** (stimulus channel to physical line, gain, calibration,
   limits) are in the config file. A stimulus never names hardware.
4. **Config format.** INI, parsed with inih.
5. **Recording format.** HDF5, serial API, see 7.
6. **Units.** Recorded signals are stored in physical units (pA, mV). The
   amplifier gain and the calibration factors are stored in the file.
7. **Re-addressing.** With rig profiles (`-P`), and `-T` for a plain waveform.
8. **Recording mode.** Episodic: only while a trial plays, one block per trial.
9. **Recorded inputs.** Every `[in]` section of the active profile. A profile
   can drop one with `skip = yes`.
10. **Multi-patch.** A stimulus may name any subset of the `[out]` sections.
    Named ones are driven. Every other output is idle (see 4, rule 3). All
    inputs are recorded either way. A stimulus naming a channel that has no
    `[out]` section is an error.
11. **cc/vc.** Each channel has `gain_cc`/`unit_cc` and `gain_vc`/`unit_vc`;
    `-m` picks the set. The stimulus channel's unit is checked against the unit
    of the chosen mode by *dimension* (current, voltage), not by spelling, so
    `nA` against `pA` is fine and `pA` against `mV` is an error.
12. **Calibration, Geko style.** The amplifier gain and a separate bench
    calibration `cal_offset`/`cal_scale` per channel (defaults 0 and 1). Geko
    defines these keys in `geko.ini` but never applies them (its code does not
    read them), so only the names and defaults are Geko's; the formula in 4a is
    LCG2's own, **to be confirmed by the owner**.
13. **Clipping.** Never silent. Before the run starts, every trial is rendered
    and checked against its line's limit (see 5). If any sample would clip,
    nothing is played and the error names the trial, channel and first sample.
    Rendering is deterministic, so trials are rendered again on demand.
14. **Console.** One progress line per trial on stderr (index, condition,
    delay). `-n` as in 2. `device = sim` in the config selects the simulator.
15. **Abort.** The signal handler only sets a flag. It is polled by the read
    and wait loops. On Ctrl-C or a failure: stop, drive every output to its
    rest value, close the HDF5 file with the completed trials (the running one
    marked `incomplete`), log it, exit with status 2.
16. **Trigger.** `start on trigger` waits for a rising edge on the PFI line
    named by `trigger` in `[rig]` (a start trigger or a polled edge, decided in
    the hardware spike). With no `trigger` key it waits for Enter.
17. **Licence.** MIT.

## 4. Precedence and addressing rules

1. **Rate.** A rate stated in the stimulus must be used (StimGen). An explicit
   `-r` that differs is an error. With neither, the config's `rate` is used.
   The rate must be feasible: the AI limit is 500 kS/s shared by all inputs,
   and AO is 840 kS/s per channel with two channels. The driver may coerce the
   rate; LCG2 reads back the rate obtained and refuses unless it equals the
   requested one exactly.
2. **Seed.** Only `-s` overrides a seed written in a stimulus or protocol. The
   config has no seed. With no seed anywhere one is drawn and recorded.
3. **Rest value.** For a channel the stimulus names, the stimulus's own `rest`
   holds before, after and between trials. For an `[out]` the stimulus does not
   name, the config's `rest` (in the unit of the active mode, default 0) holds.
   Digital lines rest at the stimulus value, otherwise 0.
4. **Output limits.** The AO range is fixed at +/-10 V by the board. An
   `[out]` section may set a narrower `limit` (volts) as a safety clamp, used
   by the clipping check. Analog inputs have a `range` (0.2, 1, 5 or 10 V).
5. **Digital outputs.** A StimGen `digital` channel needs an `[out NAME]` with
   `line = port0/lineN` and `digital = yes`, and no gain, unit or range. Only
   port 0 lines can be clocked (datasheet); other lines are refused.
6. **Profiles.** `[profile NAME]` overrides keys of other sections, written
   `section.key = value` (for example `out.P2.line = ao1`), and may set
   `skip = yes` on a section.
7. **Relative paths.** StimGen resolves `use` and `file()` against the current
   directory. LCG2 does the same, and records the absolute paths and digests
   of what was read (the rule may become "relative to the stimulus file" in M2).

## 4a. The configuration file as built (M1)

Sections: `[rig]`, `[out NAME]`, `[in NAME]`, `[cell NAME]`, `[synapse NAME]`,
`[profile NAME]`. NAME is an identifier (letters, digits, `_`, at most 47
characters); keys are lower case. A duplicate section or key, an unknown key,
a key outside a section, a line over 999 characters: all errors, with
`file:line:` in the message. The full key list is in `src/config.h`.

- `[rig]`: `device` (required: `sim`, `neuron` or a board name such as `Dev1`),
  `mode` (`cc`|`vc`; `-m` overrides it, and one of the two is **required**: a
  wrong guess would apply the wrong gains), `rate`, `trigger` (`PFI0`..`PFI15`),
  `sim_script` (required with `neuron`, refused otherwise), `sim_python`.
- `[out NAME]`: `line` (`ao0`..`ao3`, or `port0/line0`..`port0/line7` with
  `digital = yes`), `gain_cc`/`unit_cc`, `gain_vc`/`unit_vc`, `limit` (volts,
  0 < limit <= 10), `rest` (in the unit of the mode), `cal_offset`,
  `cal_scale` (not zero), `skip`. A digital line takes no gain, unit, limit,
  calibration or rest.
- `[in NAME]`: `line` (`ai0`..`ai15`), `range` (0.2, 1, 5, 10), `terminal`
  (`RSE` default, `NRSE`, `DIFF`), gains, units, calibration, `skip`.
- **Gain** is "channel units per volt", one number per channel and mode, for
  outputs and inputs alike. **Units** must have the right dimension for the
  direction and mode: an output carries current in `cc` and voltage in `vc`; an
  input the opposite. A gain and its unit are given together; the pair of the
  mode in use is required unless the channel is skipped.
- **Calibration**: `v_true = cal_scale * v_nominal + cal_offset` (volts). An
  input applies it to the volts read; an output applies the inverse to the
  volts wanted. (Applied from M2.)
- **Output limits** (one pair of helpers, used now for the idle values and in
  M2 for every sample): `wanted = value / gain` must satisfy `|wanted| <=
  limit` when a `limit` is set (it protects the amplifier), and `written =
  (wanted - cal_offset) / cal_scale` must satisfy `|written| <= 10 V` (the
  board). Each active analog output's `rest` (default 0) is checked at load,
  so an idle value that would clip is refused, never clipped.
- **Checks at load**: no two active outputs or inputs on one line; `DIFF`
  inputs are `ai0`..`ai7` and use `ai(N+8)` as the second pin (standard NI
  pairing, to be confirmed on the rig); at least one input is in use; the rate
  times the number of inputs in use is at most 500 kS/s; `-T` names an analog
  output that is in use; playing needs an output in use; a stimulus file must
  open (its content is read by StimGen from M2 on).
- **Choices made while building M1, which the owner may veto**: `mode` is
  required, with no default; `-t` needs a unit; `+` signs are refused; options
  cannot be combined (`-nv`); names are identifiers of at most 47 characters;
  `-n` prints on stdout; `DIFF` pairs `ai(N)` with `ai(N+8)`.
- **Notes for M2**: compare rates by cross-multiplying (LCG2 reduces `p/q` by
  the gcd, StimGen does not), and take the rate of a run from StimGen's
  rendered result, not from LCG2's own parse of the text.
- **Profiles**: keys are `rig.KEY` or `KIND.NAME.KEY` (`out.P2.line = ao1`,
  `in.P1.skip = yes`) and must name an existing section. `-m` is written into
  the resolved configuration, marked as set on the command line.
- **Output**: `-h` and `-n` print their answer on stdout; everything else
  goes to stderr (the "nothing on stdout" rule of section 9 is for a run).
- `-t` and the rate follow StimGen's grammar with these differences: a
  duration needs its unit (`-t 30` is refused: seconds or minutes?); no `+`
  sign; malformed exponents (`1e`, `1ee3`, which StimGen's `parse_rate`
  accepts as 1) are refused; a zero duration is refused.

## 5. Hardware facts used (USB-6343 datasheet)

- 4 AO, 16 bit, +/-10 V, simultaneous update; output FIFO 8,191 samples shared
  among channels; power-on/off glitch 1.5 V for 1.2 s.
- 16 AI (500 kS/s aggregate), ranges +/-0.2, 1, 5, 10 V; inputs are multiplexed,
  so channels are skewed by the convert clock, which is recorded.
- Clocked digital output: port 0 only, up to 1 MHz, FIFO 2,047, "system and
  bus activity dependent".
- AO and DO accept an external Start Trigger and Sample Clock; triggers come
  from PFI lines.
- **Never reset or power-cycle the board with a cell attached.**

## 6. Output safety (design rule)

- Every AO and DO buffer ends with rest samples, because a finished clocked
  task probably holds its last sample, not the rest value.
- Between trials the outputs hold the rest values. Whether this uses one
  long-lived output task or a task per trial, and how to avoid two tasks
  claiming the same AO line, is settled in the hardware spike (S1).
- The USB start of a task takes time, so `gap 0s` cannot be exact: the real
  start time and delay of every trial are recorded.
- A trial that cannot start at its `period` starts when the previous one ends;
  the delay is recorded and printed.

## 7. HDF5 layout and provenance

- One file per run, one group per trial (`trial_0000`, ...). Datasets
  `ai/<name>`, `ao/<name>` (realised command, channel units), `markers`.
- Chunked, extendable datasets, written as data arrives, so long `-t` runs do
  not need the whole record in memory.
- Rate, gains, calibration, seed, trial indices, delay, start time (UTC),
  latency, convert rate, AI over-range count and an `incomplete` flag are
  attributes. The stimulus text and the provenance record (which can be large)
  are string datasets, not attributes.
- LCG2 adds its own provenance: LCG2 version, StimGen pin, config digest,
  command line, rate obtained, and the checked clipping count (zero).
- The file is flushed after every trial. `H5Fflush` does not guarantee a
  readable file after a crash; the guarantee is only "completed trials are
  normally readable".
- Only calls available since HDF5 1.10 are used.

## 8. Dependencies and pinning

- StimGen is not copied. It is built from `STIMGEN_SRC` (default
  `../StimGen/src`) and checked against `STIMGEN_PIN` (commit and a SHA-256
  fingerprint of the library sources). Samples equal those of `sg render` on the
  same platform (StimGen reproducibility Level B).
- HDF5: serial API. The Mac has Homebrew `hdf5-mpi` 2.2.0 (parallel): its
  `hdf5.h` includes `mpi.h`, so on the Mac the build uses `mpicc`. A rig host
  uses a serial HDF5 (`libhdf5-dev` on Linux). The HDF5 compiler choice is made
  in M4, not before.
- The Mac build uses a stub `NIDAQmx.h`; the real header only exists on the
  rig host (Windows or Linux, see 1).

## 9. Defaults (the owner may veto)

- Analog input terminal mode `RSE`, range +/-10 V.
- Exit status: 0 success, 1 error before the run, 2 aborted or failed during it.
- Progress goes to stderr, data to files, nothing on stdout.

## 10. Verify on hardware (untested)

- AO and DO tasks can use the AI sample clock and start trigger
  (`/Dev1/ai/SampleClock`, `/Dev1/ai/StartTrigger`).
- Whether two committed tasks can share an AO channel (believed: no).
- Whether a finished AO task holds its last sample (believed: yes).
- Rate coercion (believed: 100 MHz divided by an integer).
- Start latency of a task on USB, and AO-to-AI loopback latency.
- Whether a device reset glitches the outputs.
- Differential inputs pair `ai(N)` with `ai(N+8)` (assumed from NI's X Series
  convention; M1 enforces it).
- USB buffering and underflow on long runs.

## 11. Simulator backends (decided)

Two backends replace the board when the config says so. Both run on the Mac.

- `device = sim`: built-in loopback (AO channel to AI channel with noise). Used
  by the test suite. No external dependency.
- `device = neuron`: the NEURON simulator drives the "cells". The config gives
  `sim_script` (the model) and `sim_python` (a Python that has NEURON; on this
  Mac `/opt/homebrew/opt/python@3.11/bin/python3.11`, NEURON 8.2.6, verified by
  running an HH soma; the default `python3` 3.15 has no NEURON).

How `device = neuron` works:

1. **Long-lived child over pipes.** LCG2 starts the script once. For each trial
   it writes one StimGen `.sgb` record (the realised stimulus, physical units)
   to the child's stdin, and reads one `.sgb`-format response record from its
   stdout. The channels of the response are named like the `[in]` sections and
   carry their unit. Records are self-delimiting, so no extra framing. The
   child's stderr goes to the hidden folder's log. The cell keeps its state
   between trials. Trial by trial, not sample by sample (no dynamic clamp).
2. **Boundary after the amplifier.** The script works in physical units (pA,
   mV); gains and calibration are bypassed. LCG2 still does the unit check and
   the clipping check against the volts it would have written.
3. **Virtual time.** No real waiting. The protocol's `period` or `gap` is
   passed to the script with the next trial as the simulated pause, and the
   model integrates through it (`advance(duration)`). A `-realtime` flag may
   slow it to real time. The delays recorded are simulated ones, marked as such.
4. **Transparency rule.** The user never writes NEURON code. With
   `device = neuron` the command line, the stimuli, the `[out]`/`[in]` channel
   map, the profiles and the outputs are exactly those of the real rig; only
   `device` and the virtual-preparation sections below differ. From the user's
   side it is a real preparation on a real amplifier.
5. **Virtual preparation in the INI.** Cells and connections are declared in
   the config, not in code:
   - `[cell P1]` names the cell on the pipette P1: `model` is a built-in model
     (for example `hh_soma`) or a path to a standard NEURON model supplied by
     someone else (a `.hoc` template or a Python class), with `section` and
     `position` saying where the electrode sits (default soma, 0.5) and the
     model's own parameters as keys. The pipette's `[out P1]`/`[in P1]` lines
     are attached to that cell.
   - `[synapse P1_P2]` connects cells: `pre`, `post`, `type` (`exp`, `exp2`),
     `weight`, `delay`, time constants, reversal potential, and where on the
     post cell. Several cells and several synapses make a small network.
   - Electrodes follow the amplifier mode: `-m cc` injects current and records
     voltage; `-m vc` clamps voltage and records current (series resistance,
     optional recording noise and electrode properties as keys, off by
     default).
6. **Adapter.** A generic script, `lcg2_neuron.py`, shipped with LCG2, reads the
   `[cell]` and `[synapse]` sections, builds the network, converts units (pA to
   nA, `Vector.play`), runs each trial and writes the response. A model
   provider only supplies a standard NEURON model; nobody edits the adapter.
   Models that need compiled `.mod` files need a one-time `nrnivmodl`, which
   the adapter's `--check` option reports.
7. **Limits.** A model slower than real time just takes longer. A script crash
   or a closed pipe aborts the run as in 3.15. The simulator does not test
   DAQmx behaviour (clock routing, FIFO, latency): see section 10.

## 12. Milestones (tests are written inside each milestone)

| # | Milestone | Acceptance test |
|:--|:--|:--|
| M0 **done** | Skeleton: layout, Makefile (`-std=c99 -pedantic -Wall -Wextra -ffp-contract=off`, `mpicc` on the Mac), pin check, inih, MIT headers | builds with no warnings; fails if the StimGen sources differ from the pin |
| M1 **done** | Config and CLI: INI, profiles, flags, validation, `-n` | a table of good and bad configs gives the expected messages and exit codes |
| M2 | StimGen integration: kind, expand, render trial, JSON trial list, channel map, unit check, volts, clip check of all trials, rest samples | samples equal `sg render` on this machine; trial order and seeds equal `sg expand`; a clipping trial is refused before anything plays |
| M3 | Trial engine and built-in loopback simulator: backend interface, scheduling, rest values, abort | simulator plays a protocol with correct `period`/`gap` timing and recorded delays; Ctrl-C leaves outputs at rest |
| M3b | NEURON backend (section 11): child process over pipes, `lcg2_neuron.py`, an example HH soma and a two-cell multi-patch model | a protocol of current steps on an HH soma gives spikes at the expected amplitudes; only P2 is driven while P1 and P2 are recorded; a killed child aborts the run cleanly |
| M4 | Recording: HDF5 writer, hidden folder, log, checksums | `h5py` reads a file back; a killed run keeps its completed trials; the hidden folder holds all items of 3.2 |
| M5 | Multi-patch and loopback tests on the simulator (part of M3/M4, kept as a suite) | `make test` passes with no hardware |
| **S1** | **Hardware spike** (on the rig host, throwaway C), moved here at the owner's request so that the simulator comes first; the reviewer advised doing it earlier, because the DAQmx backend design depends on its answers: clock and trigger routing, one task or two, last-sample hold, rate coercion, start and loopback latency, clocked DO line, trigger line | each item in section 10 is answered in writing; the backend design is fixed from the answers |
| M6 | DAQmx backend from the S1 design, built on the Mac against the stub header, built on the rig host against the real one | compiles on both |
| M7 | Bring-up on the rig: loopback, long run, scope test of the outputs on Ctrl-C, kill and USB unplug | numbers recorded; outputs observed at rest on the scope in all three cases |
| M8 | Documentation and v0.1: README, help text, example config and protocol | a new user runs the example on the simulator |

## 13. Open

- Nothing blocking. The profile flag is `-P` and the seed flag is `-s` by
  default (as in StimGen); the owner may change either.
