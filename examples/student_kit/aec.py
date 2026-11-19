#!/usr/bin/env python3
"""Active electrode compensation (AEC), offline.  Needs h5py and numpy.

    python3 aec.py RAW.h5

RAW.h5 is a current-clamp recording of a cell made through a pipette, with a broadband noise current injected
(aec_noise.sg on rig_aec.ini).  The method (Brette et al. 2008, Neuron 59:379) in four steps:

  1. The recorded potential is linear in the injected current:  V = K * I  (a convolution).  Estimate the
     "full kernel" K by deconvolving the recording with the command.
  2. K is the sum of the membrane kernel Km (slow, a decaying exponential for a passive cell) and the electrode
     kernel Ke (fast).  Fit an exponential to the tail of K, where only the membrane is left, and call the rest Ke.
  3. The electrode's contribution to the recording is Ke * I.  Subtract it:  Vm = V - Ke * I.
  4. That works for any later recording through the same electrode, whatever the current: this is the point.

If the file also holds an input V0 (rig_aec.ini records the true membrane potential there with a second, ideal
electrode on the same cell) it prints how close the compensated potential comes to the truth.
"""
import sys

import h5py
import numpy as np


def load(path, name="P1"):
    with h5py.File(path, "r") as f:
        g = f["trial_0000"]
        rate = float(g.attrs["rate_hz"])
        return g["ao/P1"][:], (g["ai/" + name][:] if name in g["ai"] else None), rate


def full_kernel(i, v, n, eps=1e-3):
    """The kernel K (mV per pA, n samples) of v = K * i, by regularised deconvolution."""
    i = i - i.mean()
    v = v - v.mean()
    m = 1 << int(np.ceil(np.log2(2 * len(i))))
    fi, fv = np.fft.rfft(i, m), np.fft.rfft(v, m)
    p = np.abs(fi) ** 2
    k = np.fft.irfft(np.conj(fi) * fv / (p + eps * p.mean()), m)
    return k[:n]


def electrode_kernel(k, rate, t_tail=(3e-3, 15e-3)):
    """Split K into the electrode kernel (returned) and a membrane exponential fitted on the tail."""
    t = np.arange(len(k)) / rate
    sel = (t >= t_tail[0]) & (t <= t_tail[1]) & (k > 0)
    slope, icpt = np.polyfit(t[sel], np.log(k[sel]), 1)
    km = np.exp(icpt + slope * t)
    ke = k - km
    ke[t >= t_tail[0]] = 0.0
    return ke, km, -1.0 / slope


def compensate(i, v, ke):
    return v - np.convolve(i - 0.0, ke)[:len(v)]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    i, v, rate = load(sys.argv[1])
    n = int(0.03 * rate)
    k = full_kernel(i, v, n)
    ke, km, tau = electrode_kernel(k, rate)
    vc = compensate(i, v, ke)
    # the tail is the membrane as seen through the pipette: the pipette capacitance loads the cell and lengthens it
    print("time constant of the slow (membrane) tail of the kernel: %.2f ms" % (tau * 1e3))
    print("electrode kernel: peak %.3f mV/pA at %.2f ms; the electrode contributes %.2f mV rms to the recording" % (
        ke.max(), ke.argmax() / rate * 1e3, np.std(v - vc)))
    _, ideal, _ = load(sys.argv[1], "V0")
    if ideal is not None:
        ideal = ideal - ideal.mean()
        raw, comp = v - v.mean(), vc - vc.mean()
        e_raw, e_comp = np.sqrt(np.mean((raw - ideal) ** 2)), np.sqrt(np.mean((comp - ideal) ** 2))
        print("error against the true membrane potential (V0): %.3f mV rms before, %.3f mV rms after (%.0fx better)" % (e_raw, e_comp, e_raw / e_comp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
