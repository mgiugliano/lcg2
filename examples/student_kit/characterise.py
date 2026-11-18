#!/usr/bin/env python3
"""Measure a virtual cell from an LCG2 recording.  Needs h5py (pip install h5py).

    python3 characterise.py fi  RUN.h5     # spikes per current step, resting potential, input resistance
    python3 characterise.py iv  RUN.h5     # peak inward and steady current per voltage step (voltage clamp)
    python3 characterise.py psp RUN.h5     # the answer of a second cell (P2) to a spike of P1

It reads what `current_steps.sg`, `vc_steps.sg` and `pair_pulse.sg` recorded.  Open the
file to see how little it takes to analyse a recording: this is a model for your own scripts.
"""
import json
import sys

import h5py


def trials(f):
    return sorted(k for k in f if k.startswith("trial_"))


def amp_of(g):
    v = g.attrs["variables"]
    v = v.decode() if isinstance(v, bytes) else v
    return v


def fi(f):
    print("%-16s %9s %9s %12s" % ("step", "spikes", "Hz", "V at end, mV"))
    xs, ys = [], []
    for k in trials(f):
        g = f[k]
        rate = float(g.attrs["rate_hz"])
        v = g["ai/P1"][:]
        a = int(0.2 * rate)                       # the step starts at 200 ms and lasts 800 ms
        b = int(1.0 * rate)
        n = sum(1 for i in range(a, b) if v[i - 1] < 0 <= v[i])
        pa = float(amp_of(g).split("=")[1].replace("pA", ""))
        print("%-16s %9d %9.1f %12.2f" % (amp_of(g), n, n / 0.8, v[b - 1]))
        xs.append(pa)
        ys.append(v[b - 1] - v[a - 1])
    small = [(x, y) for x, y in zip(xs, ys) if -60 <= x <= 0]
    rest = float(f[trials(f)[0]]["ai/P1"][int(0.1 * 20000)])
    print("\nresting potential: %.1f mV" % rest)
    if len(small) >= 2:
        (x1, y1), (x2, y2) = small[0], small[-1]
        if x2 != x1:
            print("input resistance (from the negative steps): %.0f MOhm" % ((y2 - y1) / (x2 - x1) * 1000))


def iv(f):
    print("%-16s %14s %14s" % ("step", "peak inward pA", "end of step pA"))
    for k in trials(f):
        g = f[k]
        rate = float(g.attrs["rate_hz"])
        i = g["ai/P1"][:]
        a = int(0.05 * rate)
        b = a + int(0.03 * rate)
        print("%-16s %14.0f %14.0f" % (amp_of(g), min(i[a + 2:b]), i[b - 1]))


def psp(f):
    g = f[trials(f)[0]]
    rate = float(g.attrs["rate_hz"])
    p1, p2 = g["ai/P1"][:], g["ai/P2"][:]
    spike = next((i for i, v in enumerate(p1) if v > 0), None)
    if spike is None:
        print("P1 did not fire")
        return
    base = float(p2[:spike].mean())
    peak = int(p2.argmax())
    print("P1 fires at %.2f ms" % (spike / rate * 1e3))
    print("P2 rises %.2f mV above its rest, peaking %.2f ms after the spike" % (p2[peak] - base, (peak - spike) / rate * 1e3))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("fi", "iv", "psp"):
        print(__doc__)
        sys.exit(2)
    with h5py.File(sys.argv[2], "r") as f:
        {"fi": fi, "iv": iv, "psp": psp}[sys.argv[1]](f)
