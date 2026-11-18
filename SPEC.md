# LCG2 specification (working draft 5)

Status: interview done; reviewed by an independent Opus 5.5 reviewer on
2026-11-18, and its findings are applied here. Milestones M0 to M4 are built
and tested (section 12); sections 4a to 4d record the choices made while building them. Items under "Decided" are the
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
7. **Relative paths.** *Decided by the owner (M2):* `use` and `file()` are resolved
   against the **current directory**, which is what the `sg_api_*` interface does by
   itself. (`sg render dir/x.sg` uses the stimulus file's own folder instead; the two
   agree when `lcg2` is run from that folder.) LCG2 records the paths and digests of
   what was read.

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

### 4b. The StimGen link as built (M2)

- **Only `sg_api_*`**, by construction: LCG2 does not include `sg.h`; the prototypes it
  uses are in `src/stimgen_api.h`, and every call is in `src/stim.c`. Results are copied
  out at once (they are valid only until the next call).
- **Kinds**: `stimulus` (one trial), `protocol` (`sg_api_expand` once; trials `0000`...),
  `waveform` (one signal that names no channel: needs `-T NAME`; its numbers are in the
  unit of that output in the current mode, handed to StimGen as the waveform's unit),
  `index` (a folder of trials: refused). `-T` with a stimulus or a protocol of stimuli is
  an error. The trials of a protocol must be all of one kind.
- **Rate**: render trial 0 with `-r` (or none); if StimGen says `no sampling rate`, render
  again with the `[rig]` rate. StimGen itself refuses a `-r` that differs from a stated
  rate. The wording of that message is fixed by the pin and tested. The rate of the run
  is read back from the rendered trial as an exact decimal fraction (`stim_rate_fraction`)
  and checked against 500 kS/s over the inputs in use.
- **Seed**: fixed by `stim_open`: `-s` if given, else the seed the first render drew (read
  from the `.sgb` header) and handed to every later render; a protocol's seed is drawn
  once in the single `sg_api_expand` call and each trial's seed is in the list. A trial
  rendered twice is bit for bit the same (tested, with and without `-s`).
- **Decoding**: the `.sgb` header is read with LCG2's JSON reader and the samples are
  decoded byte by byte, so they equal `sg render --text` exactly (tested on plain
  stimuli, every trial of two protocols, a plain waveform, `-e` text, other units).
- **Mapping and units**: a channel `NAME` goes to `[out NAME]`; unknown or skipped
  output, digital against analog, and two channels on one output are errors; units are
  compared by dimension and rescaled (`nA` on `pA`); a unit that is neither current nor
  voltage is an error.
- **Clipping**: every sample must be finite; the stimulus's `rest` and the extreme samples
  are checked with `out_check_value()` (the helper of 3.13), which is enough because the
  map to volts is a straight line; on failure LCG2 finds the first sample and counts them
  all. All trials are checked before anything plays. A digital sample must be 0 or 1.
- **Drive** (`stim_drive`): per output in use, `n` samples then `STIM_REST_PAD` rest samples
  (**2, provisional: S1 decides**): analog written volts `(value/gain - cal_offset)/cal_scale`;
  idle analog outputs at the rest of the configuration; digital 0/1 with the stimulus's
  rest in the padding (0 if unnamed). Packing digital lines into a port word, and timing,
  belong to the backend and the engine (M3).
- **The error trap (SPEC 3.1), checked.** Audit of the pinned StimGen sources: the only
  `exit()` in the library files is inside `die()`, which `longjmp`s to the trap when one
  is set, and every `sg_api_*` function sets one; allocation failures go through `die()`;
  the library writes to stdout only in `write_text` and `write_sgb`, which the API never
  calls; warnings are muted under the API (`g_quiet`) and returned instead. So an error in
  StimGen comes back as an error. `build/unit` proves it: stimuli that StimGen rejects in
  seven different ways (unknown name, syntax error, missing file, duplicate channel, an
  empty sweep, an empty text, garbage), alternating with good ones in one process, all
  return an error and the library still works afterwards. Note: after an error the trap
  pointer is left pointing at a finished stack frame until the next `sg_api_*` call sets
  a new one; harmless as long as nothing calls `die()` outside the API, which LCG2 never does.
- **Messages** name the user's own file (or `the -e text`) and say `-r`, not `--rate`. An error
  inside one trial of a protocol counts lines of the *trial's expanded text*, not of the
  protocol file, so the message says so: `p.sg (trial 0003, line 5 of that trial's text): ...`.
- **Where the seed came from** is reported honestly: `from -s`; `stated by the stimulus` (a
  `seed` line: two renders, or two expansions, give the same seed); or `drawn for this check;
  a run draws its own unless you give -s`. A seed drawn during `-n` is not the one a run will draw.
- **The build folder** is `BUILD` (default `build`); a build inside a Dropbox folder can fail
  at link time with "Resource deadlock avoided", so `make BUILD=/tmp/...` keeps it outside.
- **Plan** (`-n`) shows the rate and its source, the seed, the stimulus (kind, trials,
  samples), each channel's output and peak command against its limit, the idle outputs,
  and the check result.
- **Unit tests** (`build/unit`) cover `out_check_value` (exact boundaries, NaN and infinity,
  a negative calibration scale) and the rate fraction.
- **Profiles**: keys are `rig.KEY` or `KIND.NAME.KEY` (`out.P2.line = ao1`,
  `in.P1.skip = yes`) and must name an existing section. `-m` is written into
  the resolved configuration, marked as set on the command line.
- **Output**: `-h` and `-n` print their answer on stdout; everything else
  goes to stderr (the "nothing on stdout" rule of section 9 is for a run).
- `-t` and the rate follow StimGen's grammar with these differences: a
  duration needs its unit (`-t 30` is refused: seconds or minutes?); no `+`
  sign; malformed exponents (`1e`, `1ee3`, which StimGen's `parse_rate`
  accepts as 1) are refused; a zero duration is refused.

### 4c. The trial engine and the loopback as built (M3)

- **Timing** is StimGen's: `period T` = each trial starts T after the *start* of the previous one (if the
  trial is longer, the next starts when it ends, and the delay is recorded); `gap T` = T after the *end*;
  `start on trigger` = *each* trial waits for a trigger after its period or gap. The default is `gap 0s`,
  `start immediately`; a plain stimulus is one trial. The timing strings are read as StimGen writes them
  (the user's own token: `period 2.5s`, `gap 500ms`, a bare number is seconds, zero is allowed).
- **Delay** = actual start minus scheduled start, including the backend's start latency. All times are
  nanoseconds on the engine's clock; the arithmetic (samples to time, duration to samples) uses a 128-bit
  intermediate, so an hour at 500 kS/s does not overflow. The start time of a trial is also given as UTC to
  the millisecond (the run's wall-clock start plus the exact offset; simulated when the clock is virtual).
- **Order of operations**: prepare trial 0 (re-render; compare the digest of the check pass; re-check every
  sample; build the drive) before touching any output; open; outputs to rest; for each trial prepare, wait for
  its time and trigger, play, rest; at the end, **outputs to rest, then close, on every path**. A trial whose
  samples differ from the checked ones is refused.
- **A failed final rest is never silent**: `ERROR: the outputs may NOT be at rest` is printed even with `-q`,
  and the exit status is 2 even if the run had succeeded.
- **Abort**: SIGINT and SIGTERM set a flag (`sigaction`, no `SA_RESTART`, handler kept so a second Ctrl-C does
  not skip the rest step); waits, the Enter prompt and the block loop poll it. Exit status 2. SIGKILL cannot be
  caught (see section 6 and the M7 scope tests). The end of input at the Enter prompt aborts the run.
- **Exit status**: 0 success; 1 an error before any output was touched; 2 aborted, failed after, or the outputs
  could not be put at rest.
- **Streaming**: `play()` hands the inputs over in blocks of about 0.1 s, already in physical units for the
  sink (gain and calibration applied); an aborted trial is reported to the sink as incomplete with the
  samples it had. `-t DURATION` is one all-rest trial (rate from `-r` or `[rig]`; rounded to the nearest sample).
- **The loopback**: `aoN` to `aiN`; Gaussian noise (2 mV), deterministic per (seed, trial, input); an input
  has its range: voltages beyond it are clamped and counted, with a warning; an injectable start latency.
- **Time**: virtual by default for `device = sim` (nothing waits; exact times; the progress line says
  "simulated time"); `-R` follows the clock.
- **Devices**: `neuron` and board names are refused for now with a message that names the milestone.

**Choices made while building M3, which the owner may veto** (none was blocking):
1. **`-R`** is the flag for real time, and **virtual time is the default** for the simulator.
2. A configured **PFI trigger fires at once** in the simulator (it has no PFI line); without a `trigger` key it
   prompts for Enter.
3. **`period` counts from the previous trial's actual start**, as StimGen's text says. The consequence, measured:
   with a start latency L every trial slips another L (15 ms of latency: a thousand trials end 15 s late). A
   fixed grid (each trial meant to start at `period` times its index) would not drift but behaves differently
   for a trial longer than its period; if the owner prefers it, it is a few lines in `engine.c`.
4. The **loopback noise seed is fixed** (it does not follow `-s`).
5. **End of input at the Enter prompt aborts** the run.
6. The **delay includes the start latency**.

### 4d. Recording and the run archive as built (M4)

- **Files.** `[PREFIX_]YYYYMMDD_HHMMSS.h5` (local time) and the hidden folder `.STEM/`, claimed with an atomic
  `mkdir` (`-2`, `-3` on a clash). The folder holds `config.ini`, `config_original.ini`, `command.txt`,
  `stimulus.sg`, `canonical.sg` (not for protocols), `trials.json`, `provenance.json`, `files/NN_name`, `log.txt`
  and `SHA256SUMS` (data file first; paths relative to the working folder, so `shasum -a 256 -c` works).
- **Layout** of the HDF5 file: see `docs/13-data-format.md` (root attributes, string datasets, one group per trial,
  `ai/`, `ao/`, `markers`, `info`). `float64`, chunked (8192), unlimited, no compression. `incomplete` is 1 when a
  trial begins and 0 only when it ends complete; `H5Fflush` after every trial.
- **Order and exit status.** Free-space check first (nothing created if short: exit 1); folder, file and log
  before the backend is touched (failure: exit 1); whatever the engine does, the record is closed, digested and
  finished; a failed integrity record or a failed final rest is exit 2, loudly.
- **File format range.** `H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_V110)`, macro `LCG2_LIBVER_LOW`.
  Measured on HDF5 2.0.0: `SIGKILL` at random moments left 1 unreadable file in 120 with this range (0 in 164 with the
  default); a full disk left the completed trials readable, only the failing trial may be damaged. The stricter
  alternatives left unopenable files after a full disk. Read back with HDF5 1.10.7 on Ubuntu 22.04.
- **Free space.** Estimate `samples*(inputs+commands)*8 + 128 KiB + 20 KiB per trial`; refused if free space is below
  1.1 times the estimate plus 1 MB.
- **Own SHA-256** (`lcg_sha256_*`; StimGen exports `sha256_hex`, so the names must differ).

**Choices made while building M4, which the owner may veto:**
1. File and folder names use **local time**; the attribute `started_utc` is UTC.
2. Data are **float64, uncompressed**; an idle output has **no dataset**.
3. A **failed integrity record is exit 2**, like a failed rest.
4. Files read by a stimulus (`use`, `file()`) are found by a **best-effort scan** and copied to `files/`; StimGen's
   own provenance does not list them (to report upstream).
5. The **format range** 1.8 to 1.10, and the **free-space refusal** above.
6. `convert_rate` is `null` in the provenance, with a note.

Report upstream: StimGen leaks about 11 KB per rendered trial.

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
- The file format is held between HDF5 1.8 and 1.10 (`H5Pset_libver_bounds`), see 4d.

## 8. Dependencies and pinning

- StimGen is not copied. It is built from `STIMGEN_SRC` (default
  `../StimGen/src`) and checked against `STIMGEN_PIN` (commit and a SHA-256
  fingerprint of the library sources). Samples equal those of `sg render` on the
  same platform (StimGen reproducibility Level B).
- HDF5: serial API. The Makefile finds `h5cc` (on `PATH`, then the Homebrew kegs); a rig host
  uses `libhdf5-dev` on Linux (its `h5cc`, verified on Ubuntu 22.04 with HDF5 1.10.7).
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
| M2 **done** | StimGen integration: kind, expand, render trial, JSON trial list, channel map, unit check, volts, clip check of all trials, rest samples | samples equal `sg render` on this machine; trial order and seeds equal `sg expand`; a clipping trial is refused before anything plays |
| M3 **done** | Trial engine and built-in loopback simulator: backend interface, scheduling, rest values, abort | simulator plays a protocol with correct `period`/`gap` timing and recorded delays; Ctrl-C leaves outputs at rest |
| M3b | NEURON backend (section 11): child process over pipes, `lcg2_neuron.py`, an example HH soma and a two-cell multi-patch model | a protocol of current steps on an HH soma gives spikes at the expected amplitudes; only P2 is driven while P1 and P2 are recorded; a killed child aborts the run cleanly |
| M4 **done** | Recording: HDF5 writer, hidden folder, log, checksums | `h5py` reads a file back; a killed run keeps its completed trials; the hidden folder holds all items of 3.2 |
| M5 | Multi-patch and loopback tests on the simulator (part of M3/M4, kept as a suite) | `make test` passes with no hardware |
| **S1** | **Hardware spike** (on the rig host, throwaway C), moved here at the owner's request so that the simulator comes first; the reviewer advised doing it earlier, because the DAQmx backend design depends on its answers: clock and trigger routing, one task or two, last-sample hold, rate coercion, start and loopback latency, clocked DO line, trigger line | each item in section 10 is answered in writing; the backend design is fixed from the answers |
| M6 | DAQmx backend from the S1 design, built on the Mac against the stub header, built on the rig host against the real one | compiles on both |
| M7 | Bring-up on the rig: loopback, long run, scope test of the outputs on Ctrl-C, kill and USB unplug | numbers recorded; outputs observed at rest on the scope in all three cases |
| M8 | Documentation and v0.1: README, help text, example config and protocol | a new user runs the example on the simulator |

## 13. Open

- Nothing blocking. The profile flag is `-P` and the seed flag is `-s` by
  default (as in StimGen); the owner may change either.
