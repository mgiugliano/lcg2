# 13. The data file and the run folder

> **Status: built (milestone M4).** Everything on this page is what `lcg2` writes today, on the
> simulator. A board changes nothing in the layout: only `device` and the values differ.
> The layout is tested by `tests/test_record.py` (100 checks, with h5py), and
> [`examples/read_run.py`](../examples/read_run.py) is a reader that is part of that test.

Every run that plays at least one trial leaves two things in the **current folder**:

```text
20261118_164908.h5          the data: HDF5, one group per trial
.20261118_164908/           the run folder (hidden): everything needed to understand and repeat the run
```

With `-p PREFIX` the names read `PREFIX_20261118_164908.h5`.
The stamp is **local time** (the stamp in the file's attributes, `started_utc`, is UTC). The folder is
claimed first with an atomic `mkdir`; if a folder of that name already exists (two runs in the same second)
the run takes `...-2`, `...-3`, and so on, so **a run never overwrites another**.

A dry run (`-n`) and a refused command write nothing. The program also checks the free space before it
creates anything (see [Disk space](#disk-space)).

## The run folder

| File | What it is |
|:--|:--|
| `config.ini` | the configuration **as resolved**: file, profile and command-line options merged |
| `config_original.ini` | the configuration file exactly as it was read |
| `command.txt` | the command line, quoted so it can be pasted; a second line `folder: ...` says where it ran |
| `stimulus.sg` | the stimulus text as given (absent for `-t`) |
| `canonical.sg` | StimGen's canonical form of the text (absent for protocols) |
| `trials.json` | one entry per trial: name, kind, seed, variables, number of samples, scheduled time |
| `provenance.json` | LCG2 version, StimGen pin and library version, rate and where it came from, seed, warnings, digests (the same JSON is stored in the data file) |
| `files/NN_name` | copies of the files the stimulus read (`use`, `file()`; a best-effort scan, see below) and, with `device = neuron`, every model, morphology, biophysics, init and `.mod` file that was read |
| `network.json`, `neuron.log` | `device = neuron` only: the cells and synapses as they were built, and everything the adapter and NEURON printed (see [14](14-neuron-models.md)) |
| `live_replay.ini` | `-L` only: the events of the live session as a `[profile live_replay]` of timeline keys, to replay it (see [14](14-neuron-models.md#live-mode)); the events are also lines of `log.txt` (`event  <time>ms  <what>  <said>`) |
| `log.txt` | the log of the run, line by line, flushed as it goes |
| `SHA256SUMS` | SHA-256 of the data file and of every item above; the data file comes first |

`SHA256SUMS` paths are relative to the folder the run ran in, so, **from that folder**:

```text
shasum -a 256 -c .20261118_164908/SHA256SUMS      # macOS
sha256sum -c .20261118_164908/SHA256SUMS          # Linux
```

prints `OK` for every item. A changed byte anywhere is reported.

## The HDF5 file

```text
/                                      attributes: format "lcg2-hdf5/1", lcg2_version, stimgen_commit,
|                                      stimgen_sources_sha256, stimgen_library_version, run_name,
|                                      started_utc, mode, device
+-- stimulus_text   (string)           the stimulus text (absent for -t)
+-- config          (string)           the resolved configuration
+-- command_line    (string)
+-- provenance      (string, JSON)
+-- trial_0000/                        attributes: index, name, kind, seed, variables, rate_hz, p, q,
|   |                                  n_samples, scheduled_ns, start_ns, end_ns, delay_ns, latency_ns,
|   |                                  start_utc, virtual_time, recorded, incomplete
|   +-- ai/<name>   float64            the recording, physical units (mV or pA)
|   +-- ao/<name>   float64 | uint8    the command actually played (output unit; digital: 0/1)
|   +-- markers                        compound dataset
|   +-- info        (string, JSON)     only for protocol trials
+-- trial_0001/ ...
```

- **Datasets** `ai/<name>` and `ao/<name>` are 1-D, chunked (8192 samples), extendable, `float64`,
  uncompressed, named like the `[in NAME]` and `[out NAME]` sections. Idle outputs have no dataset.
- **Attributes of an `ai` dataset:** `unit`, `line`, `gain`, `cal_scale`, `cal_offset`, `range_v`,
  `terminal`, `mode`, `overrange_samples` (the samples the board or simulator clamped). With `membrane_test = yes` in its `[in]`
  (voltage clamp): `membrane_test_dv_mV`, `membrane_test_R_MOhm` (infinite if the current did not change) and
  `membrane_test_Ihold_pA`, and when a whole-cell transient was fitted `membrane_test_Ra_MOhm`, `membrane_test_Rm_MOhm`,
  `membrane_test_Cm_pF`, `membrane_test_tau_ms` (float64; see [14](14-neuron-models.md#patching-from-the-bath-to-whole-cell)).
  The same results are lines of `log.txt` (`membrane test  trial ...`).
- **Times** are integers in nanoseconds on the run's clock (`start_ns`, `end_ns`, `scheduled_ns`);
  `delay_ns` is `start_ns - scheduled_ns` and includes `latency_ns`. `virtual_time = 1` marks simulated time.
- **`incomplete`** is set to 1 when a trial *begins* and to 0 only when it ends complete. After an abort or a
  crash, a trial with `incomplete = 1` holds the samples recorded until then; the completed trials before it
  are intact.
- The file is flushed after every trial.

### Reading it (Python)

```python
import h5py
f = h5py.File("20261118_164908.h5", "r")
g = f["trial_0002"]
v = g["ai/P1"][:]                      # numpy array, in mV
t = [k / g.attrs["rate_hz"] for k in range(v.size)]
text = f["stimulus_text"].asstr()[()]  # text datasets come back as bytes without .asstr()
```

`python3 examples/read_run.py FILE.h5` prints a summary of a run (add `--plot 3` to plot trial 3).
Command-line tools work too: `h5dump -n FILE.h5` lists the contents, `h5ls -r FILE.h5` the tree.

## Order of events, and what a failure does

1. The free space is checked. **Nothing is created** if it is short (exit status 1, "nothing was written").
2. The folder, the data file and the log are created *before* the backend is touched; a failure here is
   exit status 1.
3. The engine plays the trials. Whatever happens (completion, Ctrl-C, an error), the data file is closed,
   its digest is taken and the record is finished.
4. If the integrity record cannot be written, or the final rest could not be applied, the run **fails
   loudly with exit status 2**.

## What is and is not guaranteed

Measured on the development Mac (HDF5 2.0.0) with deliberately violent tests:

| Event | Result |
|:--|:--|
| Ctrl-C, errors, abort | the completed trials are closed and readable; the running trial is marked `incomplete` |
| `kill -9` at a random moment | in 120 trials of the test **1 file** was unreadable (0 in 164 with HDF5's default format; see below); otherwise the completed trials were readable |
| full disk | the completed trials stay readable; only the trial being written when the disk filled can be damaged |

So: **completed trials are normally readable after a crash; this is not an absolute guarantee**, because
HDF5 itself does not give one (`H5Fflush` does not either). The remedy is the one every recording system
has: do not run on a nearly full disk, and copy the data off after the day.

**File format range.** LCG2 asks HDF5 for a format between 1.8 and 1.10
(`H5Pset_libver_bounds`, macro `LCG2_LIBVER_LOW` in `src/record.c`). The reasons: any HDF5 from 1.10 on
reads the file (checked with HDF5 1.10.7 on the Linux test machine), and this range survived the full-disk
tests, where the stricter alternatives left unopenable files. The cost is the one unreadable file in 120 for
a `kill -9`, measured above. A different range is a one-line change and a new measurement.

### Disk space

Before anything is created the program estimates the size
(`samples * (inputs + commands) * 8 bytes + 128 KiB + 20 KiB per trial`) and refuses if the free space
is below that estimate times 1.1 plus 1 MB. The log line `disk  the data is expected to take about N bytes; M bytes free`
records it.

## About `use` and `file()` in a stimulus

StimGen's own provenance does not list the files a stimulus reads. LCG2 finds them with a best-effort scan
(the check pass, plus the canonical text) and copies them into `files/`, numbered. A file that is
read in a way the scan cannot see is **not** copied; the log says which files were copied, and
`provenance.json` has `files_read_by_file_function` and `files_archived` so that a difference is visible.
Paths resolve against the **current folder** (the owner's decision).

## Warnings in the record

Warnings (for example, a voltage beyond the input range, clamped) are logged as
`warning  <text>  (N trials, first in trial X)` and listed in `provenance.json`.

## Not recorded (yet)

- Digital inputs, and the converted-rate note: `convert_rate` is `null` in the provenance, with a note.
- Compression: the data are `float64` and uncompressed, on purpose (simple and robust). Changing it is a
  choice for the owner.
