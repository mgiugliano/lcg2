# LCG2 documentation

LCG2 is a small command-line program for cellular electrophysiology. It plays
a stimulus written in [StimGen 2](https://github.com/mgiugliano/StimGen2) on the
analog and digital outputs of an acquisition board, and records the analog
inputs at the same time. It is a stripped-down successor of the LCG software,
written in ISO C99, with no dynamic clamp and no real-time kernel.

> **Where the project stands (read this first).**
> Milestones M0 to M4, M3b and M5 are built and tested: the program reads the command line
> and the configuration file, applies rig profiles, reads your stimulus through
> StimGen, checks every sample, and **plays the trials on a built-in loopback
> simulator**: with exact timing (period, gap, trigger), outputs held at rest
> between trials and on every way out, Ctrl-C handled safely, and the inputs
> recorded: **each run writes an HDF5 data file and a hidden run folder** (M4, see
> [13 Data format](13-data-format.md)). **`device = neuron` plays the stimulus into virtual
> cells you describe in the configuration** (M3b, see [14 Virtual cells](14-neuron-models.md)). The board is M6: designed
> and documented here, but not written. Every page says
> which parts are *built* and which are *designed*. The status of each piece is
> shown with the same marks everywhere:
>
> | Mark | Meaning |
> |:--|:--|
> | **Built** | exists in the code and is covered by `make test` |
> | **Designed** | decided in [`SPEC.md`](../SPEC.md), not written yet |

## Where to start

| You are... | Read, in this order |
|:--|:--|
| A user who wants to try it | [02 Quick start](02-quickstart.md), then [03 Tutorials](03-tutorials.md) |
| A student training on virtual cells | [`examples/student_kit/`](../examples/student_kit/), then [14 Virtual cells](14-neuron-models.md) |
| A user writing a rig configuration | [06 Configuration reference](06-config-reference.md), then [04 Concepts](04-concepts.md) |
| A user with an error message | [12 Error messages](12-error-messages.md) |
| A user who wants to analyse a recording | [13 Data format](13-data-format.md) |
| A developer who will extend the code | [10 Architecture](10-architecture.md), then `SPEC.md` |
| An AI assistant picking up the work | [11 Notes for AI assistants](11-for-ai-agents.md) first, then [10 Architecture](10-architecture.md) |

## The pages

| Page | What it is |
|:--|:--|
| [01 Overview](01-overview.md) | what LCG2 is for, the big picture, the philosophy |
| [02 Quick start](02-quickstart.md) | build it, run the first dry run in five minutes |
| [03 Tutorials](03-tutorials.md) | step by step: one pipette, cc and vc, profiles, multi-patch, digital lines, safety limits |
| [04 Concepts](04-concepts.md) | the logic: channels, units, gain, calibration, limits, rest, trials, the run archive |
| [05 Command-line reference](05-cli-reference.md) | every flag, every rule |
| [06 Configuration reference](06-config-reference.md) | every section and key of the INI file |
| [07 StimGen and LCG2](07-stimgen.md) | how the stimulus language connects to LCG2 |
| [08 Simulators](08-simulators.md) | the built-in loopback and the NEURON backend (designed) |
| [09 Hardware](09-hardware.md) | the USB-6343, what its datasheet allows, why Linux is a problem |
| [10 Architecture](10-architecture.md) | the code: modules, data flow, tests, how to add a key |
| [11 Notes for AI assistants](11-for-ai-agents.md) | state, rules, traps, decisions, next steps |
| [12 Error messages](12-error-messages.md) | each message LCG2 can print, what it means, how to fix it |
| [13 Data format](13-data-format.md) | the HDF5 file, the run folder, how to read them, what a crash leaves |
| [15 Testing](15-testing.md) | what `make test` proves, what each suite needs, what is not tested |
| [14 Virtual cells](14-neuron-models.md) | `device = neuron`: describe cells and synapses in the configuration, bring your own models, train in patching |

## Figures

All figures are in [`figures/`](figures/) as SVG files. They are drawn by
[`figures/make_figures.py`](figures/make_figures.py); edit the script and run
`python3 docs/figures/make_figures.py` to regenerate them. Solid green means
built and tested, dashed grey means designed, dashed amber means waiting for a
decision.

![Overview](figures/fig-overview.svg)

## Keeping the documents true

- The commands shown in the tutorials, in lines that start with `$ ./lcg2`, are
  run by `make test` (see `tests/test_docs.py`). If a command or its exit
  status changes, the test fails until the page is fixed.
- Output samples were produced by the program itself (some are kept in
  [`outputs/`](outputs/)).
- The authoritative design is [`SPEC.md`](../SPEC.md). These pages explain it; if
  they disagree, the spec and the code win, and the page is a bug.
