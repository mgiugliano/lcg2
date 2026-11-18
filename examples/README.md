# Examples

Rig configurations (`*.ini`) and StimGen 2 stimuli (`*.sg`). All of them are
used by the tutorials in [`../docs/03-tutorials.md`](../docs/03-tutorials.md), and
`make test` loads every `.ini` file here.

| File | What it is |
|:--|:--|
| `single_pipette.ini` | the smallest rig: one pipette, both modes |
| `lcg2.ini` | a two-pipette rig with a digital line and two profiles (`single`, `swap`) |
| `neuron_pair.ini` | two virtual cells and a synapse: a *design preview* for the NEURON backend (M3b) |
| `bad_rest.ini` | a deliberately wrong configuration (idle value above the limit); LCG2 refuses it |
| `step.sg` | one current step on channel `P1` |
| `pair.sg` | pulses on `P1`, a held current on `P2` |
| `camera.sg` | a current command with a digital trigger line `TTL` |
| `steps_protocol.sg` | a protocol: a family of current steps, repeated and shuffled |
| `frozen_noise.sg` | a protocol: the same noise realisation 50 times |
| `ramp.sg` | a plain waveform, to be sent with `-T NAME` |

Try them with the dry run, for example:

```console
$ ./lcg2 -c examples/lcg2.ini -n examples/pair.sg
```

Check a stimulus with StimGen's own tool (build it in the StimGen folder):

```console
$ ../StimGen/src/sg check examples/pair.sg
```
