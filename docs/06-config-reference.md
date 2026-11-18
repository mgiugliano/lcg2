# 6. Configuration reference

The configuration file describes the **rig**: what is wired where, and the
amplifier gains. It is an INI file read with inih. Default name: `lcg2.ini` in
the current directory (or `-c FILE`). Complete examples are in
[`examples/`](../examples/).

## Syntax

```ini
; a comment                  (also: # at the start of a line)
[section NAME]               a section header
key = value   ; comment      a key; a ';' after a space starts a comment
```

- Section kinds: `rig`, `out`, `in`, `cell`, `synapse`, `profile`. Only `rig`
  has no name: `[rig]`. The others are `[kind NAME]` with exactly one space.
- `NAME` is an identifier (letter or `_`, then letters, digits, `_`; at most 47 characters).
- Keys are lower case with digits and `_`. `Gain_cc` is an error, not a key.
- A duplicate section, a duplicate key, an unknown key, and a key before any
  section are errors. Every error names `file:line:`.
- No continuation lines: an indented line is a syntax error.
- Lines are at most 998 characters; a value at most 399. Windows line endings (CRLF)
  and a UTF-8 byte-order mark are accepted.
- Blank lines and comments are free.

## `[rig]`

| Key | Required | Values | Meaning |
|:--|:--|:--|:--|
| `device` | yes | `sim`, `neuron`, or a board name such as `Dev1` | the backend |
| `mode` | one of this and `-m` | `cc`, `vc` | amplifier mode (`-m` overrides) |
| `rate` | no | a rate: `20kHz`, `10000` | default sampling rate (a stimulus can state its own) |
| `trigger` | no | `PFI0` ... `PFI15` | the line a `start on trigger` waits on |
| `sim_script` | with `neuron` | a path | the NEURON adapter script; refused with other devices |
| `sim_python` | no (default `python3`) | a path | a Python that has NEURON; refused with other devices |

## `[out NAME]`: one output

An analog output carrying the stimulus channel `NAME`, or one digital line.

| Key | Applies to | Values | Meaning |
|:--|:--|:--|:--|
| `line` | both, required | `ao0` ... `ao3`; or `port0/line0` ... `port0/line7` with `digital = yes` | the physical line |
| `digital` | both | `yes`, `no` (default) | `yes` for a clocked digital line; only port 0 can be clocked |
| `gain_cc`, `unit_cc` | analog | positive number; current unit | gain in cc (units per volt), given together |
| `gain_vc`, `unit_vc` | analog | positive number; voltage unit | gain in vc, given together |
| `limit` | analog | volts, above 0 and at most 10 | safety clamp: `abs(wanted volts) <= limit` |
| `rest` | analog | a number (unit of the mode), default 0 | idle value; checked against the limits at load |
| `cal_offset` | analog | volts, default 0 | bench calibration offset |
| `cal_scale` | analog | not 0, default 1 | bench calibration scale |
| `skip` | both | `yes`, `no` (default) | do not use this output in this run |

The pair for the mode in use is required unless the section is `skip = yes`.
A digital line refuses `gain_*`, `unit_*`, `limit`, `rest`, `cal_*`.

## `[in NAME]`: one recorded input

| Key | Values | Meaning |
|:--|:--|:--|
| `line` (required) | `ai0` ... `ai15` | the physical line |
| `range` | `0.2`, `1`, `5`, `10` (volts), default 10 | input range |
| `terminal` | `RSE` (default), `NRSE`, `DIFF` | wiring of the input |
| `gain_cc`, `unit_cc` | positive number; **voltage** unit | gain in cc, given together |
| `gain_vc`, `unit_vc` | positive number; **current** unit | gain in vc, given together |
| `cal_offset`, `cal_scale` | as for outputs | bench calibration |
| `skip` | `yes`, `no` (default) | do not record this input |

Units by mode, once more: an **output** is current in cc and voltage in vc; an
**input** is voltage in cc and current in vc. Units known: current `fA pA nA uA mA A`,
voltage `uV mV V` (case sensitive).

`DIFF` uses a pair of pins: the input must be `ai0` ... `ai7`, and `ai(N+8)` is
its second pin, so it cannot be used by another input (the standard NI pairing,
to be confirmed on the rig).

## `[cell NAME]` and `[synapse NAME]`: virtual preparation

Only used with `device = neuron` (milestone M3b). Today the sections are
counted and not validated. The intended keys are in [08 Simulators](08-simulators.md)
and `SPEC.md`, section 11.

## `[profile NAME]`: overrides

Selected with `-P NAME`. Each line replaces, or adds, one key of an existing
section. The key is a path:

| Written | Targets |
|:--|:--|
| `rig.KEY = value` | `KEY` of `[rig]` |
| `out.NAME.KEY = value` | `KEY` of `[out NAME]` |
| `in.NAME.KEY = value` | `KEY` of `[in NAME]` |
| `cell.NAME.KEY`, `synapse.NAME.KEY` | the virtual preparation |

`out.P2.skip = yes` drops the output `P2` for the session. A profile can only
modify sections that exist; naming a missing section is an error. The new
value is checked exactly like a value written in the file.

## Checks made when the file is loaded

- every key is known and every value is well formed;
- units have the right dimension for the direction and the mode;
- no two active outputs, or two active inputs, use the same line;
- `DIFF` pairs are free;
- at least one input is in use (something must be recorded);
- the `rest` of every analog output in use is within `limit` and within 10 V after calibration;
- with a rate known, `rate × number of inputs in use <= 500 kS/s`;
- playing needs at least one output in use; `-T` names an analog output in use.

The complete list, with the exact messages, is in [12 Error messages](12-error-messages.md).

## The reference example

[`examples/lcg2.ini`](../examples/lcg2.ini), the two-pipette rig used throughout
the tutorials:

```ini
[rig]
device = sim
mode = cc
rate = 20kHz

[out P1]
line = ao0
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV
limit = 5

[out P2]
line = ao1
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV

[out TTL]
line = port0/line0
digital = yes

[in P1]
line = ai0
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA

[in P2]
line = ai1
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA

[profile single]
out.P2.skip = yes
in.P2.skip = yes

[profile swap]
out.P2.line = ao2
rig.rate = 10kHz
```
