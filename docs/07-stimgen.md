# 7. StimGen and LCG2

[StimGen 2](https://github.com/mgiugliano/StimGen2) is a separate project: a
language and a reference renderer for electrophysiology stimuli. LCG2 does not
re-implement it. LCG2 **calls the StimGen C library** to turn the text of a
stimulus or protocol into samples.

> **Status: built (milestone M2).** The link works: LCG2 reads StimGen text, asks
> StimGen to render every trial, decodes the samples, finds the output of every
> channel, checks units and every sample against the limits, and builds the volts
> that a backend will receive. It is tested against StimGen's own tool `sg`, bit
> for bit. What is *not* built yet is the engine that plays the trials (M3).

![From protocol to run](figures/fig-stimgen-flow.svg)

## Why a library and not a pipe

LCG2 takes StimGen **text**, not `.sgb` files, for three reasons:

1. A **protocol** is not one file: expanded, it is a directory of trials. Text
   avoids that.
2. The **sampling rate** is decided by the rig, and the stimulus may state it. With
   text, LCG2 passes the rate to StimGen and gets the samples it needs, with no
   resampling.
3. LCG2 needs the **trial list** with seeds and timing, and one trial at a time.

A folder of trials (`protocol.sgi`, what StimGen calls the *index* kind) is refused
with a clear message; `.sgb` input may be added later.

## Only `sg_api_*`, by construction

LCG2 calls **only** the functions named `sg_api_*` (from `src/sg_api.c` in
StimGen). They are also what the WebAssembly build uses. Their key property: they
set an error trap, so a StimGen error **returns** a message instead of calling
`exit()`. That matters when the outputs of a board are live.

To make sure nothing else can be called, LCG2 does **not** include StimGen's
`sg.h`. It declares the few functions it uses in its own header,
[`src/stimgen_api.h`](../src/stimgen_api.h), and all calls are in one file,
[`src/stim.c`](../src/stim.c).

| Function | What LCG2 uses it for |
|:--|:--|
| `sg_api_kind(text)` | is this a plain waveform, a stimulus, a protocol, or an index? |
| `sg_api_expand(text, seed)` | the ordered trial list of a protocol, as JSON, with seeds and timing |
| `sg_api_render_trial(text, rate, seed, unit, trial_json, protocol_text)` | one trial, as a complete `.sgb` byte string |
| `sg_api_error()` | the message of the last error |
| `sg_api_warnings()` | warnings (a JSON array), which LCG2 prints |
| `sg_api_result()`, `sg_api_result_size()`, `sg_api_text()` | the results |

Every function returns 0 on success and -1 on error. **Results are valid only until
the next call**, so LCG2 copies each result out at once: the `.sgb` bytes are decoded
into a `Trial`, the trial list is copied into the `Stim`.

## The path of a stimulus through LCG2

```text
text (file or -e)
  │  stim_open()     what is it?  which rate and seed?  the trial list of a protocol
  ▼
Stim
  │  stim_render()   one trial: StimGen renders it, LCG2 decodes the .sgb
  ▼
Trial                (samples in the stimulus's own units, markers, seed, provenance)
  │  stim_map()      which [out] carries each channel; the unit factor
  ▼
Map
  │  stim_check()    every sample against both limits, before anything plays
  ▼
  │  stim_drive()    volts for each analog output, 0/1 for each digital line,
  ▼                  rest samples added
Drive                (what the backend of M3 will receive)
```

### What kind of text is it? (`stim_open`)

| StimGen says | LCG2 does |
|:--|:--|
| `stimulus` | one trial, named `stimulus` |
| `protocol` | calls `sg_api_expand` **once**; the trials are named `0000`, `0001`, ... with their seeds |
| `waveform` | a *plain waveform*: one signal that names no channel. It needs `-T NAME` (which pipette gets it); its numbers are in the unit of that output in the current mode, and its channel is called `out` inside StimGen |
| `index` | refused: a folder of trials is not supported |

Rules that follow, each with its message and its test:

- a plain waveform without `-T` is an error (`... is a plain waveform: it names no channel, so say which pipette gets it with -T NAME`);
- `-T` with a stimulus or a protocol of stimuli is an error, not silently ignored;
- the trials of a protocol must all be of one kind.

### The sampling rate

The rule (SPEC 4.1): **a rate stated in the stimulus is used**; an explicit `-r` that
differs is an error; with neither, the `rate` of `[rig]` is the default.

StimGen itself refuses a `-r` that differs from the stated rate, and refuses to
render with no rate at all. So LCG2 first renders trial 0 with `-r` (or with none).
If StimGen answers `no sampling rate`, it renders again with the rate of the
configuration. The wording of that one message is fixed by the pin, and a test
fails if it ever changes. The rate of the run is then read from the rendered
trial, as an exact fraction, and checked against the board (500 kS/s shared by all
inputs).

| The stimulus | `-r` | `[rig] rate` | Result |
|:--|:--|:--|:--|
| states 20 kHz | none | 10 kHz | 20 kHz, stated by the stimulus |
| states 20 kHz | 20kHz | any | 20 kHz |
| states 20 kHz | 10kHz | any | **error**: `-r 10kHz differs from the rate stated in the stimulus` |
| states none | 5kHz | 10 kHz | 5 kHz, from `-r` |
| states none | none | 10 kHz | 10 kHz, from the configuration |
| states none | none | none | **error**: `no sampling rate: ... give -r RATE or write rate = in [rig]` |

### The seed, and why every render is the same

A trial is rendered **twice** in a real run: once to check it before anything plays, and
once to play it. Without care the second render would draw a new random seed and give
different noise. So `stim_open` fixes every seed:

- a plain stimulus: `-s` if given, else the seed StimGen drew on the first render,
  read back from the `.sgb` header and handed explicitly to every later render;
- a protocol: the protocol seed is drawn (or given with `-s`) once, in the single call
  to `sg_api_expand`; the seed of each trial is in the trial list.

A test renders every example twice with no seed and demands identical samples, and
another gives a drawn seed back with `-s` and demands the same samples again.

### Decoding the samples

The `.sgb` file is a small header, a JSON object, then the samples as little-endian
IEEE 754 binary64 (see StimGen's `docs/access/sgb-access.md`). LCG2 reads the
header with its own JSON reader ([`src/json.c`](../src/json.c)) and decodes the samples
byte by byte, with no rounding, so what LCG2 holds is **bit for bit what `sg render`
writes**. Everything is checked: the magic bytes, the header length, the rate, the
channel list, and that the file has exactly the size the header promises.

### Unit and channel mapping (`stim_map`)

- A stimulus channel `NAME` goes to `[out NAME]`. No such section, or a skipped one, is an error that lists the outputs in use.
- A digital channel needs a digital output, an analog channel an analog one.
- Units are compared **by dimension**. A stimulus in `nA` on a `pA` output is rescaled; `pA` against `mV` is an error that says which mode to use; a unit that is neither current nor voltage (a conductance such as `nS`) is an error.

### A channel's rest value: write it without a unit

A channel's `rest=` is the value it holds before and after the stimulus (and, in LCG2, between trials). Write it as a plain
number, in the unit of the channel: `channel P1 unit=mV rest=-70 { ... }`. StimGen at the pinned commit reads `rest=-70mV`, with
a unit, in volts (−0.07) while the samples stay in mV, so the output would rest at −0.07 mV. This was found on 2026-11-19 and is
to be reported upstream; until it is fixed, the kit's stimuli write `rest` without a unit, and so must protocols that substitute
a variable into it (`sweep hold = [0, -70]`, `rest=$hold`).

### The clipping check (`stim_check`)

For every named analog channel: every sample must be a finite number; the
stimulus's `rest` value and the smallest and largest samples are checked with
`out_check_value()` (the same function that checks idle values). Because the
conversion to volts is a straight line (a positive gain, a calibration scale of
either sign), if the extremes are allowed, every sample between them is. If not, LCG2
scans for the first sample and counts them all, and refuses:

```text
lcg2: the stimulus, channel P1: sample 10000 (0.5 s): 200 pA needs 0.5 V at the amplifier command, above the limit of 0.1 V; 20000 of 40000 samples clip in all
```

A protocol names the trial (`trial 0002, channel P1: sample 0 ...`). Nothing is
played until **all** trials have passed.

### The volts for the backend (`stim_drive`)

For every output in use, one array of the trial's `n` samples followed by
`STIM_REST_PAD` (2) rest samples:

- an analog channel the stimulus names: `written = (value/gain - cal_offset) / cal_scale`, volts;
- an analog output it does not name: its rest value from the configuration, as volts;
- a digital line: 0 or 1 from the stimulus, and its rest value in the padding (0 if the stimulus does not name it).

The number of rest samples is provisional: the hardware spike decides how many a
clocked task needs. Packing the digital lines into a port word and the timing are
the job of the backend and the engine (M3).

## Messages are in the user's words

StimGen reads every text as a file called `editor`, and its rate error names its own
flag, `--rate`. LCG2 translates: the error names your file (`step.sg:3: ...`) or `the
-e text`, and says `-r`. A test covers it, and checks that neither `editor` nor
`--rate` reaches the user.

## Where `use` and `file()` look for files

In the current folder. This is what the library interface does by itself, and it was
the owner's choice: run `lcg2` from the folder that holds the files the stimulus
uses. (`sg render dir/x.sg` looks in the stimulus file's own folder instead; the two
agree when you run from that folder.) The run archive will record the path and the
SHA-256 of every file read.

## Reproducibility

StimGen gives *reproducibility level B*: the same text, rate and seed give the same
samples on a given platform. LCG2 keeps the stimulus text, the rate and the seed of
every trial, so a run can be repeated.

## The pin

LCG2 was built and tested against **one specific version** of StimGen. It never
copies StimGen. Instead [`STIMGEN_PIN`](../STIMGEN_PIN) records:

- the git commit (now `379434b`, "Reject malformed exponents in decimal literals"),
- the StimGen version string,
- a **SHA-256 fingerprint** of the library sources (`util.c help.c render.c rng.c
  parse.c prims.c eval.c protocol.c sgb.c sg_api.c sg.h`, concatenated in that order).

`make` recomputes the fingerprint from `../StimGen/src` (or `STIMGEN_SRC`) and
**stops** if it differs, so a different renderer is never used by accident. The
pin has a test: a faithful copy is accepted, a changed or missing file is refused.

**When you change StimGen on purpose** (a bug fix, a new feature):

```console
$ cd ../StimGen/src
$ cat util.c help.c render.c rng.c parse.c prims.c eval.c protocol.c sgb.c sg_api.c sg.h | shasum -a 256
```

then re-run `make test` in LCG2, and write the new hash and commit into
`STIMGEN_PIN`. The point is that the update is a decision, not an accident.

## What LCG2 never does to StimGen

- never edits it (it is a separate repository);
- never writes inside its folder (it has object files of its own; LCG2 compiles
  into its own `build/stimgen/`);
- never calls anything but `sg_api_*`.

## Notes found while building

- **A fixed bug.** StimGen's rate parser used to accept malformed exponents (`1ee3Hz`,
  `1e+Hz`, `1eHz` all read as 1 Hz). LCG2 found it by comparing parsers; it was fixed
  in StimGen2 `379434b` and the pin updated. LCG2 still keeps its own, stricter parser
  for `-r` and `-t`; the two agree on every spelling tried, apart from a few
  documented differences (a zero duration; extra decimals).
- **A small leak in the library.** The library interface allocates about 11 KB per
  rendered trial that it never frees (parse trees; found with the macOS `leaks` tool,
  all from StimGen's own allocators). It is harmless for runs of thousands of trials
  (a megabyte per hundred), and LCG2 itself frees everything it allocates. It is
  worth fixing in StimGen if the library is ever used in a long-lived program.

## The language in one page

Look at the StimGen documentation for the full language. The examples used in
LCG2 are in [`examples/`](../examples/):

| File | What it shows |
|:--|:--|
| [`step.sg`](../examples/step.sg), [`step_vc.sg`](../examples/step_vc.sg) | one step on one channel, in pA (current clamp) and in mV (voltage clamp) |
| [`pair.sg`](../examples/pair.sg) | two channels, a pulse train and a held current |
| [`camera.sg`](../examples/camera.sg) | an analog channel with a digital trigger line |
| [`steps_protocol.sg`](../examples/steps_protocol.sg) | a *protocol*: a sweep of amplitudes, repeated, shuffled |
| [`frozen_noise.sg`](../examples/frozen_noise.sg) | a protocol that repeats the same noise realisation 50 times |
| [`ramp.sg`](../examples/ramp.sg) | a *plain waveform*, played with `-T NAME` |

All of them pass `sg check`.
