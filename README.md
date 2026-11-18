# LCG2

A minimal command-line program for cellular electrophysiology, written in ISO C99. It plays a
[StimGen 2](https://github.com/mgiugliano/StimGen2) stimulus on the analog and digital outputs of an acquisition
board and records the analog inputs at the same time. It is a stripped-down successor of the LCG software
(J Neurosci Methods): no dynamic clamp, no Comedi, no real-time kernel.

**Version 0.3.** Everything runs on a **simulator**: a built-in loopback, or **virtual cells in NEURON** that you
describe in the configuration file (the default is a Hodgkin–Huxley soma; bring your own morphology, biophysics and
channels if you like), recorded through a **realistic pipette and amplifier**: electrode resistance and capacitance, bridge
balance, capacitance neutralisation, series-resistance compensation, recording noise, seal leak, junction potential, amplifier
bandwidth, and active electrode compensation. **The board is not supported yet**: no NI-DAQmx backend is written, and nothing in this
repository touches hardware. Each page of the manual says what is built and what is only designed.

![Overview](docs/figures/fig-overview.svg)

## Install

This section installs **exactly what LCG2 was developed and tested with**. The versions below are the ones the whole test suite
(`make test`, about 830 checks) passed on, on two machines. Other versions probably work; these are known to.

### What is needed

| Piece | macOS (development machine) | Ubuntu 22.04 (tested on `cajal`) | Needed for |
|:--|:--|:--|:--|
| Operating system | macOS 27 on Apple silicon (arm64) | Ubuntu 22.04.5, x86-64 | |
| C compiler, `make` | Apple clang 21.0.0, GNU Make 3.81 | gcc 11.4.0, GNU Make 4.3 | building |
| HDF5 (C library) | Homebrew `hdf5-mpi` 2.2.0 (the serial `hdf5` works too) | `libhdf5-dev` 1.10.7 | building |
| StimGen 2 | commit `379434b2e07208c5ffa9812d17dd821f64c8cdbf` (pinned, checked at build time) | same | building |
| Python | 3.11.18 (Homebrew) | 3.10.12 | tests, analysis, NEURON |
| `h5py` | 3.16.0 | 3.16.0 | reading data, the recording tests |
| NEURON | 8.2.6 | 9.0.2 | `device = neuron` only |
| `numpy`, `matplotlib` | 2.4.6, 3.10.0 | numpy only | analysis scripts, figures |
| Pandoc, XeLaTeX, librsvg | 3.14, TeX Live 2026, 2.63 | optional | the PDF manual only |

Everything except the C compiler, `make`, HDF5 and StimGen is optional: without Python the program still builds and runs; without NEURON
only `device = neuron` is unavailable. **`make doctor` checks all of this and tells you what to install.**

### 1a. macOS

```console
$ xcode-select --install                       # Apple's compiler and make (once)
$ brew install hdf5 python@3.11                # HDF5 and a Python (hdf5-mpi, which the developer has, works as well)
$ brew install pandoc librsvg                  # optional: only for the PDF manual
$ brew install --cask mactex-no-gui            # optional: only for the PDF manual (a large download)
```

### 1b. Ubuntu 22.04

```console
$ sudo apt update
$ sudo apt install build-essential git libhdf5-dev python3 python3-venv python3-pip
$ sudo apt install pandoc texlive-xetex texlive-fonts-recommended librsvg2-bin    # optional: only for the PDF manual
```

(Without `sudo` rights: everything except `libhdf5-dev` can be installed in your home folder, and `libhdf5-dev` can be built from
source with `./configure --prefix=$HOME/hdf5 && make install`, then `make H5CC=$HOME/hdf5/bin/h5cc`. If `python3-venv` is not
available, create the environment with `python3 -m venv --without-pip venv` and install pip into it with
`curl -sS https://bootstrap.pypa.io/get-pip.py | venv/bin/python`.)

### 2. Get the code: LCG2 and the exact StimGen it was built against

StimGen 2 is **not** copied into this repository: it is compiled from its own sources, which sit **next to** this folder, and the
build stops if they are not the pinned version (a fingerprint in [`STIMGEN_PIN`](STIMGEN_PIN) is compared).

```console
$ mkdir ~/dev && cd ~/dev
$ git clone https://github.com/mgiugliano/StimGen2.git StimGen
$ git -C StimGen checkout 379434b2e07208c5ffa9812d17dd821f64c8cdbf
$ git clone https://github.com/mgiugliano/lcg2.git
$ git -C lcg2 checkout v0.3.0                  # the released version; main is the development branch
```

The result must look like this (the names `StimGen` and `lcg2` matter; use `make STIMGEN_SRC=/other/path/src` if you keep StimGen
elsewhere):

```text
~/dev/
    StimGen/      src/ is used
    lcg2/         this project
```

### 3. Python environment (for h5py, NEURON and the analysis scripts)

Use a virtual environment so that nothing is installed system-wide. The pinned versions are in `requirements.txt` (macOS, as
developed) and `requirements-linux.txt` (Ubuntu, as tested):

```console
$ cd ~/dev/lcg2
$ python3.11 -m venv ~/lcg2_venv               # on Ubuntu: python3 -m venv ~/lcg2_venv
$ . ~/lcg2_venv/bin/activate
$ pip install -r requirements.txt              # on Ubuntu: pip install -r requirements-linux.txt
```

NEURON is the only large package (a prebuilt wheel, about 20 MB); skip it if you will not use virtual cells. NEURON prints
`Warning: no DISPLAY environment variable` when it starts on a machine without a screen: that is harmless. Remember the path of this
Python (`which python`): it is what `sim_python` in a NEURON rig file must name.

### 4. Build and test

```console
$ make doctor          # what is installed, what is missing, how to install it
$ make                 # checks the StimGen pin and HDF5, builds ./lcg2 and the test tools (about a minute)
$ make test            # every suite; with the virtual environment active, nothing is skipped
$ make sanitize        # optional: the same suites under AddressSanitizer and UBSan
```

`make test` must end with `repository clean: no data file or run folder was left by the tests`. Expected totals on a machine with
everything installed: 185 unit, 183 configuration, 186 stimulus, 85 engine, 100 recording, 106 NEURON and 25 multi-patch checks, and the
commands of the manual. (On Ubuntu, without matplotlib: 98 recording checks, because the macOS-only disk-image check is skipped, and 105 NEURON checks, because
the plotting check is skipped, each with a note. All 76 commands of the manual run there too.)

**If the link step fails with "Resource deadlock avoided"**, the folder is inside Dropbox (or another sync service that holds new
files). Build outside it: `make BUILD=/tmp/lcg2build` (and `make test BUILD=/tmp/lcg2build`).

**If a NEURON rig file does not work**, it cannot find a Python that has NEURON. The example files say `sim_python = python3`, which is
right when the virtual environment of step 3 is active. Otherwise name the Python in the file, or, without editing anything, tell LCG2
once: `export LCG2_SIM_PYTHON=~/lcg2_venv/bin/python` (the environment variable overrides the file; the tests use it too).

### 5. First run

```console
$ mkdir ~/lcg2_data && cd ~/lcg2_data           # a run writes its files in the current folder
$ L=~/dev/lcg2
$ $L/lcg2 -c $L/examples/single_pipette.ini -n $L/examples/step.sg     # check and print the plan, play nothing
$ $L/lcg2 -c $L/examples/single_pipette.ini $L/examples/step.sg        # play it on the built-in simulator
lcg2: done: 1 trial(s) in 2.000 s (simulated), largest delay 0.000 s; wrote 20261118_164908.h5 and .20261118_164908/
$ python3 $L/examples/read_run.py 20261118_164908.h5                   # look inside (needs h5py)
```

A run leaves **two things**: `STEM.h5` (the data, one HDF5 group per trial) and a hidden folder `.STEM/` that holds the exact
configuration, command line, stimulus, log and SHA-256 checksums, so that anyone can repeat the run and check that nothing changed.

**Patch a virtual cell** (needs the NEURON environment above, active, or `LCG2_SIM_PYTHON` set):

```console
$ $L/lcg2 -c $L/examples/student_kit/rig.ini $L/examples/student_kit/current_steps.sg
$ python3 $L/examples/student_kit/characterise.py fi 2026*.h5          # f-I curve, input resistance
```

[`examples/student_kit/`](examples/student_kit/) is a package you can hand to a student: rigs, stimuli, models and exercises.
No NEURON knowledge needed.

### 6. The PDF manual (optional)

```console
$ make pdf             # writes docs/pdf/LCG2-manual.pdf (needs pandoc, xelatex, rsvg-convert)
```

It contains this README and both tutorial collections, with every figure. A built copy is kept in the repository:
[`docs/pdf/LCG2-manual.pdf`](docs/pdf/LCG2-manual.pdf).

### Uninstall

Nothing is installed outside the two folders and the virtual environment: delete `~/dev/lcg2`, `~/dev/StimGen`, `~/lcg2_venv`
(and `~/.cache/lcg2`, where compiled `.mod` channels are kept) and your data folders.

## The idea in three lines

- The **stimulus** (StimGen text) says *what to play*, with channel names and no hardware.
- The **configuration** (INI) says *how the rig is wired*: which output carries `P1`, amplifier gains, safety limits,
  and, for the simulator, the virtual cells.
- The **command line** says *what to do today*: current or voltage clamp, which profile, which rate.

Outputs are never silently clipped, always end at a safe rest value (also on Ctrl-C), and every sample is checked
against the limits before anything is played.

## Documentation

Start at **[`docs/README.md`](docs/README.md)**: quick start, tutorials, concepts, command-line and configuration
references, the data format, virtual cells, testing, an architecture guide for developers and
[a page for AI assistants](docs/11-for-ai-agents.md). The design is in [`SPEC.md`](SPEC.md); what changed between
versions is in [`CHANGELOG.md`](CHANGELOG.md).

## Layout

```text
src/       the program (C99)             examples/  rig configurations, stimuli, models, the student kit
neuron/    the NEURON adapter (Python)   docs/      the manual, its figures, docs/pdf (the PDF)
vendor/    inih (BSD), unmodified        tests/     the test suites
tools/     make doctor                   SPEC.md    the design and the milestones
requirements*.txt  pinned Python packages
```

## Licence

MIT, see [`LICENSE`](LICENSE). `vendor/inih` keeps its own BSD licence.
