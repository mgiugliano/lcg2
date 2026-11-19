# 15. Testing: what `make test` proves

> **Status: built (milestone M5).** `make test` passes with **no hardware**: the board is never needed, and
> nothing in the program references NI-DAQmx. Counts below are those of the last run on the development Mac.

```console
$ make test          # everything below, in this order
$ make sanitize      # the same suites against a build with AddressSanitizer and UBSan
```

If a build inside Dropbox fails at link time ("Resource deadlock avoided"), build elsewhere:
`make test BUILD=/tmp/lcg2build`. Every real run in a test happens in a scratch folder; the last step checks
that the repository was left clean.

## The suites

| Suite | Command it runs | Checks | What it proves |
|:--|:--|:--|:--|
| unit | `build/unit` | 203 | units and exact time arithmetic, SHA-256 against the FIPS vectors, the membrane test on synthetic transients, small helpers |
| configuration and CLI | `tests/run_tests.py` | 183 | a table of good and bad commands and configurations: the exact message and exit status of each; every example loads; the commands of the manual |
| stimulus | `tests/test_stim.py` | 186 | StimGen integration: samples equal `sg render`, trial order and seeds equal `sg expand`, unit and clip checks, never a silent clip |
| engine | `tests/test_engine.py` | 85 | scheduling (`period`, `gap`, late trials, trigger), rest values on every way out, abort with real signals, loopback wiring, noise, range clamping |
| recording | `tests/test_record.py` | 100 | the HDF5 layout read back with h5py, the run folder and its checksums, abort, `kill -9`, full disk, unwritable folder, free-space refusal |
| NEURON | `tests/test_neuron.py` | 159 | equality with a plain NEURON script, voltage clamp, synapse, state between trials, external models, mistakes, killed adapter, Ctrl-C, the student kit, the pipette and amplifier model against exact solutions and the analogue filter, patching (bath, seal, cell-attached, whole-cell, the membrane test) against the circuit's values, and the numbers and lines the tutorials of docs/16 quote |
| multi-patch | `tests/test_multipatch.py` | 25 | three pipettes and a digital line on the loopback, end to end: gains, calibration, rest values, no crosstalk, voltage clamp, profiles, protocol noise, no hardware |
| documents | `tests/test_docs.py` | 80 commands | every `$ ./lcg2 ...` line of the manual runs with the exit status it states |
| clean | `tests/check_clean.py` | | no data file or run folder was left in the repository |

## Checking the environment

`make doctor` lists what is installed (compiler, HDF5, StimGen at the pin, Python, h5py, NEURON, Pandoc), what is missing and how to install it.
`requirements.txt` and `requirements-linux.txt` pin the Python packages the suites passed with.

## What needs what

| Needs | Used by | If it is missing |
|:--|:--|:--|
| a C99 compiler, HDF5 (`h5cc`), the pinned StimGen sources | the build | `make` says what to install; it stops if StimGen differs from the pin |
| Python 3 and `h5py` | recording, multi-patch (they re-run themselves with a Python that has it) | the test **fails**, loudly: a recorder that cannot be read back is untested |
| a Python with NEURON and h5py | the NEURON suite | it **skips**, loudly, with status 0 and the line "Nothing of M3b was tested". `LCG2_NEURON_PYTHON` names one; `LCG2_NO_NEURON=1` forces the skip (the multi-patch suite uses that to test the skip itself) |
| the StimGen folder next to LCG2 | the pin test | skipped with a note |

## How the tests are built

* **Independent readers.** Recordings are read with h5py, checksums with `hashlib` and `shasum`, samples are
  compared with StimGen's own `sg` program, a cell with plain NEURON. A test that reads its own output with the
  code under test proves little.
* **Exact where it can be.** The loopback has a noise switch in the test drivers (`build/runtest --noise 0`, which
  calls the same `run_execute()` as `lcg2`; `lcg2` itself has no hidden switch), so values are checked exactly.
* **Real signals.** Abort tests send SIGINT, SIGTERM and SIGKILL to a running `lcg2`.
* **Faults on purpose.** A killed adapter, a full disk (a 6 MB disk image on the Mac), an unwritable folder, a bad
  rest value, a failing final rest (the simulator can be told to fail it).
* **Sanitizers.** `make sanitize` runs all the suites against binaries built with `-fsanitize=address,undefined`
  (leak detection is off).

## What is **not** tested (and why)

* **The board.** There is no hardware backend. Everything about DAQmx (clock routing, start latency, the FIFO,
  what the outputs do on a crash) is the work of the hardware spike S1 and bring-up M7.
* **The sanitizer run on Linux**, macOS-only checks (the free-space refusal on a small disk image uses `hdiutil`: the
  test says so and skips it elsewhere), Windows, and `.asc` morphologies.
* **What was run on Linux** (Ubuntu 22.04, gcc, system HDF5 1.10.7, NEURON 9.0.2 from pip, Python 3.10): the whole `make test`
  passes, apart from the checks that need matplotlib or the Mac Python of the example files, which skip with a note. It found
  two real bugs (below). The Mac has HDF5 2.0, NEURON 8.2.6 and Python 3.11: the two sets of versions agree.
* **Long runs.** The tests use seconds of signal; the disk-full and kill tests are the evidence about long ones.
* **Real-time accuracy** beyond what `-R` shows in the engine tests (a few trials, a loaded machine can be late).

## Adding a test

Put it in the suite that owns the behaviour, use a scratch folder (`newdir`), check with `check(name, ok, detail)`
and make the failure message say what was seen. A new `[cell]` key belongs in `tests/test_neuron.py`; a new
configuration message in the table of `tests/run_tests.py`; a new file in the HDF5 layout in `tests/test_record.py`
**and** `docs/13-data-format.md`.
