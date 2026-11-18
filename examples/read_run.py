#!/usr/bin/env python3
"""Summarise an LCG2 data file, and optionally plot a trial.

    python3 examples/read_run.py RUN.h5            # a summary of the run and of every trial
    python3 examples/read_run.py RUN.h5 --plot 3   # also plot trial_0003 (needs matplotlib)

It needs h5py (pip install h5py). It is also the model for your own analysis: every
idiom you need to read an LCG2 file is in the few lines below, and tests/test_record.py
runs this script, so it is known to work.

How h5py hands back the contents of an LCG2 file:
  * numbers are numpy values: group.attrs["rate_hz"] is a float, ["n_samples"] an integer;
  * the text ATTRIBUTES (seed, unit, kind, ...) come back as str;
  * the text DATASETS (stimulus_text, config, provenance, trial_0000/info) are scalar
    datasets that come back as bytes: use ds.asstr()[()] or ds[()].decode("utf-8");
  * a recording is an ordinary 1-D dataset: ds[:] is a numpy array in physical units.
"""
import json
import sys

import h5py


def text(x):
    """A text value as str, whether h5py gave str or bytes."""
    return x.decode("utf-8") if isinstance(x, bytes) else x


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    f = h5py.File(sys.argv[1], "r")

    print("run        %s" % text(f.attrs["run_name"]))
    print("LCG2       %s, StimGen %s (library %s)" % (text(f.attrs["lcg2_version"]),
          text(f.attrs["stimgen_commit"])[:10], text(f.attrs["stimgen_library_version"])))
    print("started    %s, mode %s, device %s" % (text(f.attrs["started_utc"]), text(f.attrs["mode"]),
                                                  text(f.attrs["device"])))
    prov = json.loads(f["provenance"].asstr()[()])
    print("rate       %.6g Hz (%s)" % (prov["rate"]["hz"], prov["rate"]["source"]))
    if prov["seed"]:
        print("seed       %s (%s)" % (prov["seed"]["value"], prov["seed"]["source"]))
    if "stimulus_text" in f:
        print("stimulus   %d lines" % len(f["stimulus_text"].asstr()[()].splitlines()))

    trials = sorted(k for k in f if k.startswith("trial_"))
    print("trials     %d\n" % len(trials))
    for name in trials:
        g = f[name]
        a = g.attrs
        flag = "  INCOMPLETE" if a["incomplete"] else ""
        extra = ("  " + text(a["variables"])) if text(a["variables"]) else ""
        print("%s  %s%s  start %.3f s  delay %.3f s%s" % (name, text(a["name"]), extra,
              a["start_ns"] / 1e9, a["delay_ns"] / 1e9, flag))
        for out in sorted(g["ao"]) if "ao" in g else []:
            d = g["ao"][out]
            x = d[:]
            print("    command %-6s %s  min %9.4g  max %9.4g" % (out, text(d.attrs["unit"]) or "0/1", x.min(), x.max()))
        for inp in sorted(g["ai"]):
            d = g["ai"][inp]
            x = d[:]
            print("    input   %-6s %s  %d samples  mean %9.4g  min %9.4g  max %9.4g" %
                  (inp, text(d.attrs["unit"]), x.size, x.mean(), x.min(), x.max()))

    if "--plot" in sys.argv:
        import matplotlib.pyplot as plt
        k = int(sys.argv[sys.argv.index("--plot") + 1])
        g = f["trial_%04d" % k]
        rate = float(g.attrs["rate_hz"])
        fig, axes = plt.subplots(len(g["ai"]) + len(g["ao"] if "ao" in g else []), 1, sharex=True)
        axes = axes if hasattr(axes, "__len__") else [axes]
        i = 0
        for out in sorted(g["ao"]) if "ao" in g else []:
            d = g["ao"][out]
            axes[i].plot([s / rate for s in range(d.shape[0])], d[:], lw=0.8)
            axes[i].set_ylabel("%s (%s)" % (out, text(d.attrs["unit"]) or "0/1"))
            i += 1
        for inp in sorted(g["ai"]):
            d = g["ai"][inp]
            axes[i].plot([s / rate for s in range(d.shape[0])], d[:], lw=0.8)
            axes[i].set_ylabel("%s (%s)" % (inp, text(d.attrs["unit"])))
            i += 1
        axes[-1].set_xlabel("time (s)")
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
