# 9. Hardware

> **Status.** No hardware code exists. This page records what is known about the
> board, what the datasheet allows, and why the rig host is undecided.

## The board

A National Instruments **USB-6343** (X Series, multifunction I/O).

| Resource | What the datasheet says (as used by the design) |
|:--|:--|
| Analog outputs | 4 channels, 16 bit, **fixed ±10 V**, simultaneous update. Output FIFO 8,191 samples, shared between channels. A power-on or power-off **glitch** of 1.5 V lasting 1.2 s. |
| Analog inputs | 16 channels (or 8 differential), 16 bit, **500 kS/s aggregate** (shared by all channels in use), ranges ±0.2, ±1, ±5, ±10 V. Inputs are multiplexed, so channels are skewed by the convert clock. |
| Digital outputs | clocked output only on **port 0**, up to 1 MHz, FIFO 2,047 samples. |
| Triggers | outputs accept an external start trigger and sample clock; triggers come from PFI lines. |

Design rules that follow from it:

- **Never reset or power-cycle the board with a cell attached** (the glitch above).
- Every analog and digital buffer **ends with rest samples**, because a finished
  clocked task probably holds its last sample.
- Only port 0 digital lines are accepted in the configuration.
- The rate must satisfy `rate × inputs in use <= 500 kS/s`; this is checked today.
- `DIFF` inputs use the pair `ai(N)` and `ai(N+8)`; checked today, to be confirmed.

## The driver problem

The board is driven from C by NI's **NI-DAQmx** library. In November 2026 the
driver was installed and tested on the Linux machine `cajal`
(Ubuntu 22.04, kernel 6.8, NI-DAQmx 26.8):

- the packages installed and DKMS built about 50 kernel modules;
- Secure Boot needed the module-signing key to be enrolled (MOK) at the console;
- after enrolment the NI modules loaded, `nidevldu` ran and `nikal` was loaded;
- the USB-6343 was visible on the USB bus as `3923:74f7`, but **no driver claimed
  it**, and `nilsdev` listed no device, and `nidaqmxconfig --self-test Dev1` failed
  with "Device identifier is invalid".

Evidence that this is expected, not a fault:

- NI forum answers say USB DAQmx devices other than USB cDAQ are **not supported
  on Linux**, and that this is not being actively worked on;
- the older NI-DAQmx Base driver, the only Linux driver that covered some USB
  devices, **excludes X Series**;
- NI's own udev rules for X Series cover only PCI/PCIe devices.

This was not confirmed on NI's current compatibility page, which could not be
opened; so it is strong evidence, not proof.

## Options for the rig host

| Option | Notes |
|:--|:--|
| Windows with the USB-6343 | NI-DAQmx on Windows supports it. The code needs only `NIDAQmx.h`; a MinGW or MSVC build is needed. `cajal` has a Windows boot entry. |
| Windows PC or VM with USB pass-through | the board times its own samples in hardware, so a VM's timing matters less. Untested. |
| A PCIe-6343 on Linux | the same X Series API, natively supported on Linux. Costs a purchase. |
| A custom USB driver | reverse-engineering NI's protocol: weeks to months, risky, and a possible licence issue. Not recommended. |
| NI-DAQmx Base | excludes X Series; not an option. |

**Decision pending.** Because of this, LCG2 keeps everything specific to an
operating system in one file (`platform.c`, to be written), parses `argv` by
hand instead of using `getopt`, and sticks to ISO C99. The hardware milestones
(S1, M6, M7) wait.

## The hardware spike (S1), when the host is known

A throwaway C program to answer, in writing, the open questions of `SPEC.md`
section 10. The backend design is fixed from the answers.

| Question |
|:--|
| Can AO and DO tasks use the AI sample clock and start trigger (`/Dev1/ai/SampleClock`, `/Dev1/ai/StartTrigger`)? |
| Can two committed tasks share an AO channel (believed: no)? |
| Does a finished AO task hold its last sample (believed: yes)? |
| What is the rate coercion (believed: 100 MHz divided by an integer)? |
| What are the start latency of a task on USB and the AO-to-AI loopback latency? |
| Does a device reset glitch the outputs? |
| What happens to USB buffering and underflow on long runs? |

It needs a cable from AO0 to AI0 and the owner's agreement **before any output is
driven**.

## Safety checklist for bring-up (M7)

- Start with nothing connected to the outputs; use a scope or a voltmeter.
- Test, with the scope on the output: Ctrl-C, `kill`, and unplugging the USB
  cable. The outputs must be seen at rest in all three cases.
- Record the real numbers (latency, delay, rate obtained) in the spec.
