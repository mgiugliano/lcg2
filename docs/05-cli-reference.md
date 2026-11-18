# 5. Command-line reference

```text
lcg2 [options] STIMULUS.sg      play a stimulus or protocol, record
lcg2 [options] -e 'TEXT'        the same, with the text on the command line
lcg2 [options] -t DURATION      record only (outputs stay at rest)
```

Exactly one of these three must be given: a stimulus file, `-e`, or `-t`.

**Rules of the parser** (written by hand, because `getopt` is POSIX and not ISO C):

- Every option is **one letter**, and its value is a **separate word**:
  `-r 20kHz`, not `-r20kHz`.
- Short options **cannot be combined**: `-n -v`, not `-nv`.
- An option may be given **once**.
- An option that takes a value must have one: `-c ''` is an error (except `-e`).
- `-` (standard input) is not a stimulus file.
- `-h` anywhere prints the help and stops (exit status 0), before anything else is checked.

## Options

| Flag | Value | Meaning |
|:--|:--|:--|
| `-c` | `FILE` | configuration file. Default: `lcg2.ini` in the current directory. |
| `-P` | `NAME` | rig profile: apply the section `[profile NAME]`. |
| `-m` | `cc` or `vc` | amplifier mode (current clamp or voltage clamp). Overrides `mode` in `[rig]`. One of the two is required. |
| `-r` | `RATE` | sampling rate: `20kHz`, `10000` (Hz), `2.5Hz`, `1e4`, `0.5MHz`. A rate written in the stimulus wins; a different `-r` is an error. |
| `-s` | `SEED` | seed of the random generators: a non-negative integer. Without it one is drawn and recorded. |
| `-T` | `NAME` | send a *plain waveform* (a signal that names no channel) to the pipette `NAME`, which must be an analog `[out NAME]` in use. Required for a plain waveform; an error with a stimulus. |
| `-t` | `DURATION` | record only, for that long: `30s`, `500ms`, `250us`, `2min`. The unit is required; the value must be above 0. It is one trial with every output at rest, so it needs a rate (`-r` or `[rig]`). |
| `-e` | `TEXT` | the stimulus text given inline instead of in a file. A full stimulus needs newlines (`-e $'sg 2 stimulus\nchannel P1 unit=pA { 1s dc(0) }'` in bash); a plain waveform may be one line, `;` separating segments, as in StimGen's `sg`. |
| `-p` | `PREFIX` | prefix of the output file names. Letters, digits, `_`, `-`, `.`; must not start with `.` or contain a slash. |
| `-n` | | dry run: check everything, print the plan on stdout, play nothing. |
| `-R` | | real time: the simulator waits as a real board would. The default is virtual time (as fast as possible, with exact simulated times). |
| `-q` | | quieter (progress will be reduced). Cannot be used with `-v`. |
| `-v` | | more verbose. With `-n` it also prints the resolved configuration. |
| `-h` | | print the help. |

### Which flags mean the same as in StimGen's `sg`

`-r` (rate), `-s` (seed) and `-e` (inline text) mean the same. **`-t` does not**:
in `sg` it is `--text`, while in LCG2 it is the record-only duration.

### Number and duration spellings

- **Rate:** a number, optionally followed by `Hz`, `kHz` or `MHz`; no unit means Hz.
  No sign. The exact value is kept as a fraction. At most 6 decimals.
  Examples: `20kHz`, `10000`, `2.5Hz`, `1e4`.
- **Duration:** a number followed by `ns`, `us`, `ms`, `s` or `min`. The unit is
  required: `-t 30` could mean seconds or minutes, so it is refused. The value
  must be a whole number of picoseconds and above 0.
- LCG2 is a little stricter than StimGen on purpose: it refuses `+5Hz`,
  malformed exponents such as `1e`, `1e+` or `1ee3` (which StimGen's rate parser
  reads as 1), and a bare `-t 30`.

## Combinations that are refused

| Combination | Why |
|:--|:--|
| no file, no `-e`, no `-t` | nothing to do |
| two of: a file, `-e`, `-t` | one job at a time |
| two stimulus files | LCG2 plays one |
| `-T` with `-t` | `-T` names the pipette that receives a stimulus; `-t` has none |
| `-q` with `-v` | they contradict each other |
| `-T NAME` where `NAME` is unknown, skipped or digital | a plain waveform needs an analog output in use |
| playing with no `[out]` in use | nowhere to play (use `-t` to record) |
| a stimulus file that cannot be opened | checked up front |
| a plain waveform without `-T`, or `-T` with a stimulus | `-T` is for plain waveforms only |
| `-r` that differs from the rate the stimulus states | the stimulus's rate wins |
| a stimulus that clips, names an unknown channel, or has the wrong unit | refused before anything plays |

## Output streams and exit status

| Stream | What goes there |
|:--|:--|
| stdout | the answer of `-h` and of `-n` (and `-n -v`) |
| stderr | every error (`lcg2: ...`) and, later, progress |
| files | the data file and the run folder, in the current folder (M4; see [13 Data format](13-data-format.md)) |

| Status | Meaning |
|:--|:--|
| 0 | success |
| 1 | an error before the run |
| 2 | aborted (Ctrl-C, kill), or failed after the outputs were touched, or the outputs could not be put at rest |

## Examples

```console
$ ./lcg2 -c rig.ini stimulus.sg                  # play, in the mode written in rig.ini
$ ./lcg2 -c rig.ini -m vc -P pair stimulus.sg    # voltage clamp, profile "pair"
$ ./lcg2 -c rig.ini -r 10kHz -s 42 stimulus.sg   # fixed rate and seed
$ ./lcg2 -c rig.ini -T P2 ramp.sg                # plain waveform to pipette P2
$ ./lcg2 -c rig.ini -t 30s                       # record 30 s, outputs at rest
$ ./lcg2 -c rig.ini -n -v stimulus.sg            # check, print the plan and the resolved config
```

A run without `-n` reads the stimulus and does **all** the checks, and then plays the
trials on the loopback simulator (`device = sim`) or the NEURON simulator (`device = neuron`; with `-n` it
builds the cells and prints them, see [14](14-neuron-models.md)). A board name is refused until the hardware backend (M6). A run writes
`[PREFIX_]YYYYMMDD_HHMMSS.h5` and the hidden folder `.[PREFIX_]YYYYMMDD_HHMMSS/` in the current folder
(see [13 Data format](13-data-format.md)); before it creates anything it checks the free space (exit
status 1, "nothing was written", if short). If the integrity record cannot be written the status is 2.
