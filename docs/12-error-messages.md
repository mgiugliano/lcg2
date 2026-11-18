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

## Profiles

| Message | Meaning |
|:--|:--|
| `profile '...' not found in ... (available: a, b)` | wrong `-P` name |
| `profile '...' not found: ... defines no [profile NAME] section` | the file has no profiles |
| `[profile p] out.P9.line: there is no section [out P9] to override` | a profile may only modify existing sections |

## Not an error, but a stop

```text
lcg2: playing and recording are not implemented yet in this build (milestones M0 and M1); use -n to check a configuration
```

Exit status 1. Use `-n` until milestones M2 to M4 are built.
