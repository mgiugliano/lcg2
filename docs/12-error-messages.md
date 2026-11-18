# 12. Error messages

Every error starts with `lcg2: ` and ends the program with exit status 1 (an
error before the run). Errors that come from the configuration file name the
file, the line, the section and the key:

```text
lcg2: examples/bad_rest.ini:17: [out P1] rest: 2100 pA needs 5.25 V at the amplifier command, above the limit of 5 V
      ^file                ^line ^section ^key  ^what is wrong
```

Below, `...` stands for the part that depends on your file. Messages are grouped by
where they come from. The wording is checked by the test table.

## Command line

| Message (shortened) | What it means | What to do |
|:--|:--|:--|
| `nothing to do: give a stimulus file, -e TEXT or -t DURATION` | no source of work | give one |
| `give only one of: a stimulus file, -e TEXT, -t DURATION` | two sources | keep one |
| `two stimulus files given` | LCG2 plays one | give one |
| `unknown option '-x'` / `'-nv'` / `'--rate'` | not an option, or combined, or long | options are one letter; see `-h` |
| `option -r needs a value` | `-r` was last, or followed by another flag | give the value as the next word |
| `option -r is given twice` | repeated option | give it once |
| `option -c has an empty value` | `-c ''` | give a value |
| `-m: expected cc or vc, got '...'` | bad mode | `-m cc` or `-m vc` |
| `-r: '...' is not a rate` | bad rate | `20kHz`, `10000`, `2.5Hz`; no sign; at most 6 decimals |
| `-t: '...' is not a duration` | bad duration or no unit | `30s`, `500ms`, `2min`; must be above 0 |
| `-s: the seed must be a non-negative integer` | bad seed | digits only, up to 64 bits |
| `-p: the prefix may contain only letters, digits, '_', '-' and '.'` | unsafe prefix | no slash, not starting with `.` |
| `-T ... cannot be used with -t` | a plain waveform needs a stimulus | drop one |
| `-q and -v contradict each other` | | choose one |
| `'-' (standard input) is not supported` | | give a file, `-e` or `-t` |
| `cannot open stimulus file '...'` | the file is missing or unreadable | check the path |

## Reading the configuration file

| Message (shortened) | What it means | What to do |
|:--|:--|:--|
| `cannot open configuration file '...'` | file not found | `-c FILE`, or run in the folder that has `lcg2.ini` |
| `bad section [...]` | not `rig`, `out`, `in`, `cell`, `synapse` or `profile`, or a bad name | `[out P1]`: one space, name of letters, digits and `_` |
| `section [...] appears twice` | duplicate section | merge them |
| `key '...' is given twice` | duplicate key in a section | remove one |
| `key '...' is outside any section` | a key before the first `[...]` | add a header |
| `bad key name '...'` | upper case or odd characters | lower case, digits, `_` |
| `syntax error: expected [section], key = value or a comment` | a line without `=`, an unclosed `[`, or an indented continuation | fix that line |
| `line is too long` / `value is too long` / `section name is too long` | limits: 998 / 399 / 47 characters | shorten |
| `bad key '...'` in a profile | a profile line must be `rig.KEY` or `out.NAME.KEY` | rewrite it |
| `unknown key (allowed: ...)` | a typo, or a key of another section | use one of the listed keys |

## `[rig]`

| Message | Fix |
|:--|:--|
| `there is no [rig] section` | add `[rig]` |
| `device is required (sim, neuron, or the board name, e.g. Dev1)` | add `device =` |
| `'...' is not sim, neuron or a board name like Dev1` | use an identifier such as `Dev1` |
| `the amplifier mode is not set: give -m cc or -m vc, or mode = cc|vc here` | choose; there is no default |
| `expected cc or vc, got '...'` | fix `mode =` |
| `'...' is not a rate (examples: 20kHz, 10000, 2.5Hz)` | fix `rate =` (no space before the unit) |
| `expected PFI0 ... PFI15, got '...'` | fix `trigger =` (upper case) |
| `device = neuron needs sim_script (the model script)` | add `sim_script =` |
| `only used with device = neuron` | remove `sim_script` or `sim_python`, or set `device = neuron` |

## `[out NAME]` and `[in NAME]`

| Message | Fix |
|:--|:--|
| `line is required (ao0 ... ao3, or port0/line0 ...)` | add `line =` |
| `an analog output is ao0 ... ao3 (got '...')` | analog outputs are `ao0` to `ao3`; a digital one needs `digital = yes` |
| `a digital output must be on port 0: port0/line0 ... port0/line7` | only port 0 can be clocked |
| `not allowed on a digital line (it rests at the stimulus value, otherwise 0)` | remove `gain_*`, `unit_*`, `limit`, `rest`, `cal_*` from the digital section |
| `expected yes or no, got '...'` | `digital`, `skip` take `yes` or `no` only |
| `gain_cc and unit_cc must be given together` | give both or neither |
| `expected a number, got '...'` | fix the value (no `nan`, no text) |
| `the gain must be positive (units per volt)` | gain above 0 |
| `unknown unit '...'` | use `fA pA nA uA mA A` or `uV mV V` (case sensitive) |
| `'...' has the wrong dimension: here the unit must be a current / a voltage` | an output carries current in cc and voltage in vc; an input the opposite |
| `no gain_cc and unit_cc, which the cc mode of this run needs (or give skip = yes)` | add the pair for the mode in use |
| `the limit is in volts, above 0 and at most 10` | |
| `cal_scale must not be zero` | |
| `an analog input is ai0 ... ai15 (got '...')` | |
| `the range is 0.2, 1, 5 or 10 (volts)` | |
| `expected RSE, NRSE or DIFF, got '...'` | upper case |
| `line ... is already used by output/input ...` | two active channels on one line; change one, or `skip = yes` |
| `a differential input is ai0 ... ai7 (its pair is ai8 ... ai15)` | |
| `DIFF uses ai8 as its second pin, which input ... already uses` | |

## Output limits

| Message | Meaning |
|:--|:--|
| `rest: 2100 pA needs 5.25 V at the amplifier command, above the limit of 5 V` | the idle value exceeds `limit` |
| `rest: ... needs ... V at the board after calibration, above its range of 10 V` | the idle value exceeds the board range, after the inverse calibration |
| `the default rest of 0 is not possible: ...` | the calibration offset alone pushes the idle value out of range |

Fix: lower `rest`, raise `limit` (at most 10), or check the gain and calibration.
Idle values of skipped outputs are not checked.

## Checks that need the command line and the file together

| Message | Meaning |
|:--|:--|
| `no input to record: give at least one [in NAME] section that is not skipped` | something must be recorded |
| `no output to play on: ...` | every `[out]` is skipped; use `-t` to record only |
| `-T: there is no output named '...' (analog outputs in use: ...)` | wrong name |
| `-T: output '...' is a digital line; a plain waveform needs an analog output` | |
| `-T: output '...' is skipped in this configuration` | the profile dropped it |
| `rate ... with N analog input(s) is above the board's 500 kS/s total` | lower the rate or use fewer inputs |

## The stimulus (read through StimGen)

| Message (shortened) | What it means | What to do |
|:--|:--|:--|
| `FILE:LINE: ...` or `the -e text:LINE: ...` | StimGen found a mistake in your text, at that line | fix the stimulus; `build/sg help` explains the language |
| `FILE (trial 0003, line 5 of that trial's text): ...` | a mistake inside one trial of a protocol; the line counts the trial's own expanded text, not the protocol file | find the construct in the protocol's `stimulus { ... }` block; `build/sg expand` writes each trial as a file so you can look at line 5 |
| `... is a plain waveform: it names no channel, so say which pipette gets it with -T NAME` | the text is one signal with no channel name | add `-T NAME` |
| `-T is only for a plain waveform; ... names its channels itself` | `-T` with a stimulus or a protocol of stimuli | drop `-T` |
| `-T: 'NAME' is not an analog output in use` | wrong name, a skipped or a digital output | choose an analog `[out]` that is in use |
| `... a folder of trials (protocol.sgi) is not supported` | the text is a directory-form index | give the protocol text |
| `... the trials are not all of the same kind` | a protocol mixes plain waveforms and stimuli | make them one kind |
| `-r 10kHz differs from the rate stated in the stimulus` | the stimulus states a rate and `-r` disagrees | drop `-r`, or use the stated rate |
| `no sampling rate: the stimulus does not state one, so give -r RATE or write rate = in [rig]` | no rate anywhere | add one |
| `the stimulus has a channel 'P9', but the configuration has no [out P9] (outputs in use: ...)` | the channel name has no output | add `[out P9]`, or rename the channel |
| `... but [out P2] is skipped in this configuration` | a profile dropped that output | use another profile or drop the channel |
| `channel 'P1' is digital in the stimulus but [out P1] is analog` (and the reverse) | digital and analog mixed up | match the kinds |
| `channel 'P1' is in pA (a current), but [out P1] carries a voltage in vc mode (mV); use -m cc, ...` | the stimulus unit does not fit the amplifier mode | use the other mode or change the unit |
| `channel 'P1' has the unit 'nS', which is not a current ... or a voltage ...` | only currents and voltages can be played | use pA/nA/... or mV/V |
| `the stimulus, channel P1: sample 10000 (0.5 s): 200 pA needs 0.5 V at the amplifier command, above the limit of 0.1 V; N of M samples clip in all` | a sample would exceed the limit | lower the stimulus, raise `limit` (at most 10 V), or check the gain |
| `trial 0002, channel P1: sample 0 ...` | the same, in a protocol: names the trial | as above |
| `...: the rest value 2100 pA: ...` | the stimulus's own rest value exceeds a limit | lower it |
| `sample N is not a finite number` | NaN or infinity in the samples | fix the stimulus |
| `channel 'X': sample N is 0.5, but a digital line takes only 0 and 1` | a digital channel with other values | make it 0 or 1 |
| `cannot open stimulus file`, `... contains a NUL byte, so it is not text`, `... is over 64 MiB` | the file cannot be used as text | check the file |
| `warning: ...: channels have different durations; ...` (not an error) | StimGen's own warning, passed on | `-q` silences it |

## Profiles

| Message | Meaning |
|:--|:--|
| `profile '...' not found in ... (available: a, b)` | wrong `-P` name |
| `profile '...' not found: ... defines no [profile NAME] section` | the file has no profiles |
| `[profile p] out.P9.line: there is no section [out P9] to override` | a profile may only modify existing sections |

## While running (milestone M3)

| Message (shortened) | What it means | What to do |
|:--|:--|:--|
| `device = neuron is not implemented yet (milestone M3b); use device = sim` | the NEURON backend is not built | use `device = sim` for now |
| `device 'Dev1': there is no hardware backend yet (milestone M6, ...)` | no board backend is built | use `device = sim` |
| `no sampling rate: give -r RATE or write rate = in [rig]` | `-t` needs a rate and none is given | add one |
| `-t: the duration is shorter than one sample, or too long` | `-t` rounds to whole samples | give a longer duration, or a higher rate |
| `trial 0003 is not the one that was checked (its samples changed between the check and the run): not played` | the re-rendered trial differs from the one that passed the check | this should never happen; report it with the stimulus |
| `trial 0003 failed its check again before playing: ...` | the same, a limit | as above |
| `unknown timing '...': expected 'period T' or 'gap T'` / `the protocol's start rule is '...'` | a timing statement StimGen wrote that LCG2 does not know | report it: StimGen and LCG2 disagree |
| `start on trigger: no trigger line is configured; press Enter to start` | not an error: the prompt | press Enter, or add `trigger = PFIn` |
| `aborted after N of M trial(s): outputs at rest` | Ctrl-C or kill; exit status 2 | the outputs were put at rest |
| `warning: trial 0003: input P1: N samples beyond the range of 1 V (clamped)` | an input would exceed its range | raise the `range` of that `[in]`, or lower the stimulus |
| `ERROR: the outputs may NOT be at rest: ...` | putting the outputs at rest failed (shown even with `-q`); exit status 2 | check the outputs by hand before touching anything |

## Not an error, but a note

```text
lcg2: done: 1 trial(s) in 2.000 s (simulated), largest delay 0.000 s; no data file was written: recording is milestone M4
```

The end of a successful run. Exit status 0. The inputs were measured and counted, and nothing is
saved until milestone M4.
