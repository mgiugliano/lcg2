# 3. Tutorials

Each tutorial is short and can be followed in order. Lines that start with
`$ ./lcg2` are run by `make test`, so they stay true; the number after
`# exit` is the exit status the test expects. Run everything from the top
folder of the project.

> **Status.** Most tutorials use the dry run `-n`, which since milestone M2 **reads your stimulus
> through StimGen**, renders every trial, checks the units and every sample, exactly as a
> real run does before it plays anything. Since M3 a run **plays the trials on the built-in
> loopback simulator** (Tutorials 9 and 11), and since M4 **every such run writes an HDF5 file and a
> hidden run folder in the current folder** (see [13 Data format](13-data-format.md)). The examples below run
> from the project folder for brevity, so the files land there (git ignores them); in real use, `cd` into a
> data folder first and give the full path to `lcg2`.

Contents: [1 One pipette](#tutorial-1-one-pipette-in-current-clamp) ·
[2 Voltage clamp](#tutorial-2-the-same-rig-in-voltage-clamp) ·
[3 Two pipettes](#tutorial-3-two-pipettes) ·
[4 Re-addressing](#tutorial-4-sending-a-stimulus-to-another-pipette) ·
[5 Digital line](#tutorial-5-a-digital-trigger-line) ·
[6 Safety limits](#tutorial-6-safety-limits-and-the-rest-value) ·
[7 Record only](#tutorial-7-record-only) ·
[8 Your own rig](#tutorial-8-writing-the-configuration-of-your-own-rig) ·
[9 Run a protocol](#tutorial-9-a-protocol-checked-and-run) ·
[10 Check a stimulus with sg](#tutorial-10-checking-a-stimulus-with-sg) ·
[11 Timing](#tutorial-11-when-trials-start-period-gap-and-late-trials) ·
[12 Virtual cells](#tutorial-12-patching-a-virtual-cell-on-the-neuron-simulator)

---

## Tutorial 1: one pipette in current clamp

**Goal:** read a minimal rig and understand each line of the plan.

The rig is [`examples/single_pipette.ini`](../examples/single_pipette.ini):

```ini
[rig]
device = sim            ; sim = built-in simulator; a board name (Dev1) later
mode   = cc             ; current clamp (or -m vc on the command line)
rate   = 20kHz

[out P1]
line    = ao0           ; analog output 0 of the board
gain_cc = 400           ; 400 pA at the amplifier input per volt of the board
unit_cc = pA
gain_vc = 20            ; in voltage clamp: 20 mV per volt
unit_vc = mV
limit   = 5             ; never ask the board for more than 5 V (safety)

[in P1]
line    = ai0           ; analog input 0 records the pipette
gain_cc = 100           ; the amplifier output: 100 mV per volt
unit_cc = mV
gain_vc = 1000          ; voltage clamp: 1000 pA per volt
unit_vc = pA
```

and the stimulus is [`examples/step.sg`](../examples/step.sg): 500 ms at 0 pA,
one second at 200 pA, 500 ms at 0 pA, on a channel named `P1`:

```text
sg 2 stimulus
rate 20kHz

channel P1 unit=pA {
    500ms  dc(0)
    @step
    1s     dc(200)
    500ms  dc(0)
}
```

The names must agree: the stimulus channel `P1` is played on the output
`[out P1]`, and the recording `[in P1]` is the pipette's voltage.

```console
$ ./lcg2 -c examples/single_pipette.ini -n examples/step.sg
```

```text
  rate           20 kHz (stated by the stimulus)
  seed           ... (drawn for this check; a run draws its own unless you give -s)
  work           play the stimulus in examples/step.sg
  stimulus       a stimulus with 1 channel(s), 40000 samples (2 s)
  channels       P1 (pA)  ->  [out P1] on ao0, peak 0.5 V of the limit 5 V
  check          1 trial(s) rendered and checked: no sample clips
```

Read it from the top: the rate comes from the stimulus (it says `rate 20kHz`); a
seed was drawn (give your own with `-s`); the stimulus has one channel, `P1`, in
pA, which will be played on `[out P1]`, analog output 0; its largest command is
0.5 V (200 pA at 400 pA per volt), well below the 5 V limit; and every sample was
checked. The rest of the plan lists the outputs and inputs, as in the
[Quick start](02-quickstart.md).

**Try the safety check.** [`examples/low_limit.ini`](../examples/low_limit.ini) is the
same rig with `limit = 0.1` instead of 5. The step needs 0.5 V, so:

```console
$ ./lcg2 -c examples/low_limit.ini -n examples/step.sg   # exit 1
lcg2: the stimulus, channel P1: sample 10000 (0.5 s): 200 pA needs 0.5 V at the amplifier command, above the limit of 0.1 V; 20000 of 40000 samples clip in all
```

LCG2 refused before anything could be played, and told you the channel, the first
sample (10000, which is 0.5 s: the start of the step), what it needs, the limit, and
how many samples are affected. The same happens for a trial in the middle of a
protocol of a thousand: all trials are checked before the first is played.

---

## Tutorial 2: the same rig in voltage clamp

**Goal:** see how one flag changes the gains and units, and how the stimulus unit is checked.

In voltage clamp the command is a voltage, so the stimulus must be in volts.
[`examples/step_vc.sg`](../examples/step_vc.sg) is the twin of `step.sg`: a 20 mV step.

```console
$ ./lcg2 -c examples/single_pipette.ini -n -m vc examples/step_vc.sg
```

The plan now reads:

```text
  mode           vc
  channels       P1 (mV)  ->  [out P1] on ao0, peak 1 V of the limit 5 V
  ...
    P1       -> ao0  20 mV per volt, limit 5 V, rest 0 mV
    P1       <- ai0  1000 pA per volt, range 10 V, RSE
```

The command is in mV (20 mV per volt: the 20 mV step is 1 V), and the recording is a
current (pA). Both sets of gains live in the file; `-m cc|vc` only chooses.

**The unit is checked by dimension.** The current-clamp stimulus does not fit a
voltage-clamp rig, and LCG2 says so, and says what to do:

```console
$ ./lcg2 -c examples/single_pipette.ini -n -m vc examples/step.sg   # exit 1
lcg2: channel 'P1' is in pA (a current), but [out P1] carries a voltage in vc mode (mV); use -m cc, or change the unit of the stimulus
```

Only the *dimension* matters, not the spelling: a stimulus in `nA` on an output
that works in `pA` is fine, and LCG2 rescales it (0.2 nA is 200 pA is 0.5 V).

If the file has no `mode =` line and you give no `-m`, LCG2 stops with `the amplifier
mode is not set` (exit status 1): a wrong guess would apply the wrong gains.

---

## Tutorial 3: two pipettes

**Goal:** a multi-patch rig, driving one cell while recording both.

[`examples/lcg2.ini`](../examples/lcg2.ini) has two pipettes and a digital line.
The stimulus [`examples/pair.sg`](../examples/pair.sg) injects pulses into `P1`
and holds `P2` at a small negative current:

```text
channel P1 unit=pA { 1s dc(0)  @train  500ms pulses(amp=2000, rate=20Hz, width=1ms)  1s dc(0) }
channel P2 unit=pA { 2.5s dc(-50) }
```

```console
$ ./lcg2 -c examples/lcg2.ini -n examples/pair.sg
```

```text
  channels       P1 (pA)  ->  [out P1] on ao0, peak 5 V of the limit 5 V
                 P2 (pA)  ->  [out P2] on ao1, peak 0.125 V
  idle           TTL (not named: they hold their rest value)
  check          1 trial(s) rendered and checked: no sample clips
```

Notice `peak 5 V of the limit 5 V`: the pulses of 2000 pA at 400 pA per volt are
*exactly* at the limit. A sample exactly at the limit is allowed; one hair above is
not.

![Multi-patch](figures/fig-multipatch.svg)

The rules, which the plan shows:

- A stimulus can name **any subset** of the outputs. Named outputs are driven.
- An output the stimulus does not name is **idle**: it holds its `rest` value (here
  `TTL`).
- **Every `[in]` is recorded** in every trial, driven or not.
- A stimulus that names a channel with no `[out]` section is an error:

```console
$ ./lcg2 -c examples/single_pipette.ini -n examples/pair.sg   # exit 1
lcg2: the stimulus has a channel 'P2', but the configuration has no [out P2] (outputs in use: P1)
```

---

## Tutorial 4: sending a stimulus to another pipette

**Goal:** re-address a stimulus without editing it.

Two tools, both working today.

**Profiles** change the wiring for a whole session. `lcg2.ini` has two:

```ini
[profile single]        ; only pipette P1 is used
out.P2.skip = yes
in.P2.skip  = yes

[profile swap]          ; P2 is wired to analog output 2, and a slower rate
out.P2.line = ao2
rig.rate    = 10kHz
```

```console
$ ./lcg2 -c examples/lcg2.ini -n -P single examples/step.sg
$ ./lcg2 -c examples/lcg2.ini -n -P swap examples/pair.sg
```

The first plan shows `P2 skipped` among the outputs and the inputs. The second shows
`P2 -> ao2`. A profile line is written `section.key = value`: `rig.KEY` for `[rig]`, and
`out.NAME.KEY`, `in.NAME.KEY` for the others.

Two things to learn from these runs. First, the profile `swap` sets `rig.rate = 10kHz`,
but `pair.sg` states `rate 20kHz` itself, and **the stimulus's own rate wins**: the plan
says `20 kHz (stated by the stimulus)`. The rate of the configuration is only a default
for stimuli that state none. Second, a profile that skips an output makes a stimulus
that names it an error:

```console
$ ./lcg2 -c examples/lcg2.ini -n -P single examples/pair.sg   # exit 1
lcg2: the stimulus has a channel 'P2', but [out P2] is skipped in this configuration
```

**`-T NAME`** sends a *plain waveform* (a single signal without a channel name) to the
pipette `NAME`. [`examples/ramp.sg`](../examples/ramp.sg) is one line:

```text
500ms dc(0); 1s dc(300); 500ms dc(0)
```

```console
$ ./lcg2 -c examples/lcg2.ini -n -T P2 examples/ramp.sg
```

```text
  rate           20 kHz (from the configuration; the stimulus states none)
  target         plain waveform to P2 (-T)
  stimulus       a plain waveform with 1 channel(s), 40000 samples (2 s)
  channels       waveform (pA)  ->  [out P2] on ao1, peak 0.75 V
  idle           P1, TTL (not named: they hold their rest value)
```

The numbers of a plain waveform are in the unit of the pipette's output in the current
mode (here pA; in `-m vc` they would be mV). `-T` is only for plain waveforms:

```console
$ ./lcg2 -c examples/lcg2.ini -n -T P2 examples/step.sg   # exit 1
lcg2: -T is only for a plain waveform; examples/step.sg names its channels itself
$ ./lcg2 -c examples/lcg2.ini -n examples/ramp.sg   # exit 1
lcg2: examples/ramp.sg is a plain waveform: it names no channel, so say which pipette gets it with -T NAME
$ ./lcg2 -c examples/lcg2.ini -n -T TTL examples/ramp.sg   # exit 1
lcg2: -T: output 'TTL' is a digital line; a plain waveform needs an analog output
```

---

## Tutorial 5: a digital trigger line

**Goal:** a camera or shutter trigger played together with the current.

A digital line is declared with `digital = yes` and a port-0 line:

```ini
[out TTL]
line    = port0/line0
digital = yes
```

Only **port 0** can be clocked on the USB-6343, so any other port is refused:

```console
$ ./lcg2 -c examples/lcg2.ini -n examples/camera.sg
```

```text
  channels       P1 (pA)  ->  [out P1] on ao0, peak 0.9165 V of the limit 5 V
                 TTL (digital)  ->  [out TTL] on port0/line0
  idle           P2 (not named: they hold their rest value)
```

A digital line takes no gain, unit, limit, calibration or rest: it rests at the
value the stimulus gives it, otherwise 0. The stimulus
[`examples/camera.sg`](../examples/camera.sg) has a `digital TTL { ... }` block
whose values are 0 and 1 only.

---

## Tutorial 6: safety limits and the rest value

**Goal:** understand the two limits, and what is checked before anything is played.

![Signal chain](figures/fig-signal-chain.svg)

An output has two limits:

1. `limit` (volts, you set it): the largest voltage that may appear at the
   amplifier's command input. It protects the amplifier and the cell.
2. 10 V: the range of the board itself, fixed.

Two kinds of value are checked against both limits, never clipped silently:

- every **sample** of the stimulus (Tutorial 1 showed the refusal);
- the idle value `rest` (in pA for cc, mV for vc, default 0), which is held before,
  between and after trials, and is checked as soon as the configuration is read. With `gain_cc = 400` and
`limit = 5`, a `rest` of 2100 pA would need 5.25 V:

[`examples/bad_rest.ini`](../examples/bad_rest.ini) is the single-pipette rig with
`rest = 2100` added on purpose:

```console
$ ./lcg2 -c examples/bad_rest.ini -n examples/step.sg   # exit 1
lcg2: examples/bad_rest.ini:17: [out P1] rest: 2100 pA needs 5.25 V at the amplifier command, above the limit of 5 V
```

The message names the file, the line, the section and the key, and gives the
numbers. A `rest` that goes past 10 V is refused too, with a message that says the
board's range would be exceeded "after calibration", because the calibration is
applied to the voltage before it is written.

Nothing is clipped silently anywhere in LCG2: a value that cannot be produced is
an error that names the file, the line and the numbers.

Rate is checked too. The board's analog input limit is 500 kS/s, shared by all
inputs in use:

```console
$ ./lcg2 -c examples/lcg2.ini -n -r 300kHz examples/ramp.sg   # exit 1
```

```text
lcg2: rate 300 kHz with 2 analog input(s) is above the board's 500 kS/s total (2 channel(s) at most 250 kHz each)
```

---

## Tutorial 7: record only

**Goal:** record without playing anything (to look at spontaneous activity).

```console
$ ./lcg2 -c examples/lcg2.ini -n -t 30s
```

`-t DURATION` replaces the stimulus: the outputs stay at rest and the inputs are
recorded for that long. The unit is required (`30s`, `500ms`, `2min`); a bare
`-t 30` is refused because it could mean seconds or minutes:

```console
$ ./lcg2 -c examples/lcg2.ini -n -t 30   # exit 1
```

---

## Tutorial 8: writing the configuration of your own rig

**Goal:** a checklist for a new rig. Start from `examples/single_pipette.ini`.

1. **Name your pipettes.** The name is a short identifier (`P1`, `cellA`). It will
   be the channel name in stimuli and the dataset name in the data file.
2. **Wire the outputs.** For each pipette make an `[out NAME]` with `line = ao0`
   to `ao3`. One analog output carries one pipette; two outputs cannot share a line.
3. **Wire the inputs.** For each recording make an `[in NAME]` with `line = ai0`
   to `ai15`. Choose `range` (0.2, 1, 5 or 10 V) and `terminal` (`RSE`, `NRSE`,
   `DIFF`) as the amplifier needs.
4. **Enter the gains.** They are *channel units per volt*, one pair per mode.
   Read them from your amplifier's manual. Example: a command sensitivity of
   400 pA/V is `gain_cc = 400`, `unit_cc = pA`; an output scale of 100 mV/V is
   `gain_cc = 100`, `unit_cc = mV` on the input.
5. **Set a `limit`.** Choose the largest command voltage you would accept. It
   costs nothing and prevents a typo from damaging a preparation.
6. **Check it:** `./lcg2 -c your.ini -n your_stimulus.sg`, once per mode (`-m cc` with a
   stimulus in pA, `-m vc` with one in mV).
7. **Calibrate (optional).** Measure the board with a voltmeter and enter
   `cal_scale` and `cal_offset` per channel (see [Concepts](04-concepts.md)).
8. **Add profiles** for the variations you use (single cell, swapped pipettes).

Every mistake gives a message with the file and the line; see
[12 Error messages](12-error-messages.md).

---

## Tutorial 9: a protocol, checked and run

**Goal:** check a whole protocol, then run it on the simulator.

[`examples/steps_protocol.sg`](../examples/steps_protocol.sg) is a protocol: a family of current steps
from -300 to +300 pA in steps of 50 pA, three repetitions, in shuffled blocks, one trial
every 5 s. Check it on the single-cell profile:

```console
$ ./lcg2 -c examples/lcg2.ini -n -P single examples/steps_protocol.sg
```

```text
  rate           20 kHz (from the configuration; the stimulus states none)
  seed           protocol seed 14272848968180529564 (drawn for this check; a run draws its own unless you give -s)
  stimulus       a protocol of 39 trials: period 5s, start immediately; each trial 40000 samples (2 s)
  channels       P1 (pA)  ->  [out P1] on ao0, peak 0.75 V of the limit 5 V
  idle           TTL (not named: they hold their rest value)
  check          39 trial(s) rendered and checked: no sample clips
```

StimGen expanded the protocol into 39 trials and LCG2 rendered and checked **each one**;
the largest command anywhere is 0.75 V (300 pA). The order of the trials and the seed
of each are fixed by the protocol seed: give `-s 7` and you get the same ones every time.

Now run it, with a seed so that the order is the same every time:

```console
$ ./lcg2 -c examples/lcg2.ini -P single -s 7 examples/steps_protocol.sg
```

```text
lcg2: running 39 trial(s) on the loopback simulator, in simulated time (-R follows the clock)
lcg2: trial 1/39  0000  amp=-200pA  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: trial 2/39  0001  amp=100pA  start 5.000 s  delay 0.000 s  (simulated time)
lcg2: trial 3/39  0002  amp=300pA  start 10.000 s  delay 0.000 s  (simulated time)
...
lcg2: trial 39/39  0038  amp=50pA  start 190.000 s  delay 0.000 s  (simulated time)
lcg2: done: 39 trial(s) in 192.000 s (simulated), largest delay 0.000 s; wrote 20261118_164908.h5 and .20261118_164908/
```

Each line is one trial: its number, its name, the values of the protocol's variables
(`amp=-200pA`), when it started, and its delay. The protocol says `period 5s`, so the trials
start 5 s apart, and the whole run is 192 simulated seconds (190 s to the last start, plus its
2 s). It took a moment of real time, because simulated time does not wait. `-R` makes it
follow the clock.

> **Built (M4): the data file.** The run wrote `YYYYMMDD_HHMMSS.h5` (one group per trial) and a hidden
> folder `.YYYYMMDD_HHMMSS/` in the folder you ran it from: the resolved configuration, the command, the
> stimulus, the trials with their seeds, a log and `SHA256SUMS`. Run real runs from a data folder, not from
> the project folder. [13 Data format](13-data-format.md) describes every item; to look at the recording:
>
> ```console
> $ python3 examples/read_run.py 20261118_164908.h5
> ```

**Ctrl-C** stops a run safely. LCG2 stops, drives every output to its rest value, and exits with
status 2:

```text
lcg2: trial 12/39  0011  ...
^C
lcg2: aborted after 12 of 39 trial(s): outputs at rest
```

(A mock-up of the keystroke; the behaviour is tested with real signals.) `kill -9` cannot be caught by
any program; see [Concepts](04-concepts.md).

**Record only.** `-t DURATION` is one trial with every output at rest:

```console
$ ./lcg2 -c examples/lcg2.ini -t 30s
```

```text
lcg2: trial 1/1  record  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: done: 1 trial(s) in 30.000 s (simulated), largest delay 0.000 s; wrote 20261118_164908.h5 and .20261118_164908/
```

---

## Tutorial 10: checking a stimulus with `sg`

StimGen has its own command-line tool, `sg`. `make` builds a copy of it from the
StimGen sources into `build/sg` (nothing is written inside the StimGen folder), and
LCG2's tests use it as the reference: the samples LCG2 decodes are compared, bit for
bit, with what `sg render` writes. You can use it to write and look at stimuli:

```console
$ build/sg check examples/pair.sg
examples/pair.sg: ok, 2 channel(s), 50000 samples at 20000 Hz (2.5 s)
$ build/sg check examples/steps_protocol.sg
examples/steps_protocol.sg: ok, 39 trial(s), protocol seed 13884263320379133600
$ build/sg expand -s 7 examples/steps_protocol.sg /tmp/steps    # every trial as a file, with its seed
```

All the `.sg` files in `examples/` pass `sg check`. See [07 StimGen and
LCG2](07-stimgen.md) for exactly how LCG2 uses the same engine.

---

## Tutorial 11: when trials start (period, gap and late trials)

**Goal:** see StimGen's three timing rules in the simulator, with exact times.

Three small protocols, each of four trials of 2 s:
[`timing_period.sg`](../examples/timing_period.sg) (`period 5s`),
[`timing_gap.sg`](../examples/timing_gap.sg) (`gap 2s`) and
[`timing_late.sg`](../examples/timing_late.sg) (`period 1s`, shorter than the trial).

```console
$ ./lcg2 -c examples/lcg2.ini -r 20kHz examples/timing_period.sg
```

```text
lcg2: trial 1/4  0000  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: trial 2/4  0001  start 5.000 s  delay 0.000 s  (simulated time)
lcg2: trial 3/4  0002  start 10.000 s  delay 0.000 s  (simulated time)
lcg2: trial 4/4  0003  start 15.000 s  delay 0.000 s  (simulated time)
```

`period 5s`: each trial starts 5 s after the **start** of the previous one.

```console
$ ./lcg2 -c examples/lcg2.ini -r 20kHz examples/timing_gap.sg
```

```text
lcg2: trial 1/4  0000  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: trial 2/4  0001  start 4.000 s  delay 0.000 s  (simulated time)
lcg2: trial 3/4  0002  start 8.000 s  delay 0.000 s  (simulated time)
lcg2: trial 4/4  0003  start 12.000 s  delay 0.000 s  (simulated time)
```

`gap 2s`: each trial starts 2 s after the **end** of the previous one (2 s of trial, then 2 s of
pause: 4 s apart).

```console
$ ./lcg2 -c examples/lcg2.ini -r 20kHz examples/timing_late.sg
```

```text
lcg2: trial 1/4  0000  start 0.000 s  delay 0.000 s  (simulated time)
lcg2: trial 2/4  0001  start 2.000 s  delay 1.000 s  (simulated time)
lcg2: trial 3/4  0002  start 4.000 s  delay 1.000 s  (simulated time)
lcg2: trial 4/4  0003  start 6.000 s  delay 1.000 s  (simulated time)
```

`period 1s` with trials of 2 s: the next trial was meant to start 1 s after the previous start,
but the previous trial is still running, so it starts as soon as it ends. The **delay** (1 s) is
recorded and printed. Without a timing statement a protocol is `gap 0s`: trials back to back.

The three forms `period T`, `gap T` and the optional `start on trigger` are all StimGen's. With
`start on trigger` **each** trial waits for a trigger after its period or gap has passed. The
simulator has no PFI line, so with `trigger = PFI0` in `[rig]` the edge is simulated at once;
without a `trigger` key it prompts and waits for Enter:

```text
lcg2: start on trigger: no trigger line is configured; press Enter to start
```

If the input ends instead (nobody can press Enter), the trial is not started and the run
stops at rest with status 2.

---

## Tutorial 12: patching a virtual cell on the NEURON simulator

**Goal:** run the experiments you would run on a real cell, on a cell that exists only in NEURON, without
knowing NEURON. You need a Python that has NEURON (`pip install neuron`); the kit
[`examples/student_kit/`](../examples/student_kit/) names the one of the development Mac in `sim_python`, so
change that line first.

The rig is the configuration of a real rig, plus `[cell P1]`: the pipette `P1` is on the cell `P1`.

```ini
[cell P1]
model = hh_soma          ; the default: a Hodgkin-Huxley soma (20 um)
```

**Look before you play.** `-n` builds the cell and prints it (nothing is played):

```console
$ ./lcg2 -c examples/student_kit/rig.ini -n examples/student_kit/current_steps.sg
```

```text
  virtual preparation (NEURON 8.2.6, Python 3.11.18): 1 cell(s), 0 synapse(s)
    cell P1       hh_soma                    1 section(s),    1 segment(s), electrode soma(0.5), rests at -65.0 mV
```

**Current steps** (a protocol: nine steps from -100 to +300 pA, one every 2 s), then the measurements:

```console
$ ./lcg2 -c examples/student_kit/rig.ini examples/student_kit/current_steps.sg
```

```console
$ python3 examples/student_kit/characterise.py fi 20261118_172433.h5
step                spikes        Hz V at end, mV
amp=-100pA               0       0.0       -80.71
amp=-50pA                0       0.0       -69.83
amp=0pA                  0       0.0       -64.97
amp=50pA                 1       1.2       -62.26
amp=100pA               50      62.5       -65.21
...
resting potential: -65.0 mV
input resistance (from the negative steps): 97 MOhm
```

(The file name is the one your run printed.) **Voltage clamp** is the same rig with `-m vc`, where the command is
in mV and the recording in pA:

```console
$ ./lcg2 -m vc -c examples/student_kit/rig.ini examples/student_kit/vc_steps.sg
```

**Another cell, nothing else changed:** a profile swaps the model.

```console
$ ./lcg2 -P ballstick -c examples/student_kit/rig.ini examples/student_kit/current_steps.sg
```

**Two cells and a synapse:** patch both, drive one:

```console
$ ./lcg2 -c examples/student_kit/rig_pair.ini examples/student_kit/pair_pulse.sg
```

**Your own cell:** a morphology, biophysics and initialisation code you wrote, as files next to the
configuration (see `rig_morphology.ini`):

```console
$ ./lcg2 -c examples/student_kit/rig_morphology.ini examples/student_kit/current_steps.sg
```

The keys, the model contract (Python, hoc, SWC, `.mod`), the units rule and the limits are in
[14 Virtual cells](14-neuron-models.md). **Nine longer tutorials with figures** (excitability, voltage clamp, synapses,
writing your own cell, a morphology, your own channel) are in [16 Tutorials with virtual cells](16-neuron-tutorials.md);
the exercises are in the kit's `README.md`.
