#!/usr/bin/env python3
"""Plot a recording from an LCG2 data file.  Needs h5py and matplotlib (pip install h5py matplotlib).

    python3 plot_run.py RUN.h5               # every trial, every input, on the screen
    python3 plot_run.py RUN.h5 3             # only trial 3
    python3 plot_run.py RUN.h5 3 out.png     # save to a file instead (.png, .pdf, .svg)

Each trial gets a panel per recorded input and one per command (the stimulus as it was played).
"""
import sys

import h5py


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    import matplotlib
    if len(sys.argv) > 3:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    f = h5py.File(sys.argv[1], "r")
    names = sorted(k for k in f if k.startswith("trial_"))
    if len(sys.argv) > 2:
        names = ["trial_%04d" % int(sys.argv[2])]
    for name in names:
        g = f[name]
        rate = float(g.attrs["rate_hz"])
        chans = [("ao", c) for c in sorted(g["ao"])] + [("ai", c) for c in sorted(g["ai"])]
        fig, axes = plt.subplots(len(chans), 1, sharex=True, figsize=(8, 1.8 * len(chans) + 0.6), squeeze=False)
        for ax, (kind, c) in zip(axes[:, 0], chans):
            d = g[kind + "/" + c]
            y = d[:]
            ax.plot([i / rate for i in range(len(y))], y, lw=0.8)
            u = d.attrs["unit"]
            u = u.decode() if isinstance(u, bytes) else u
            ax.set_ylabel("%s (%s)" % (("command " if kind == "ao" else "") + c, u or "0/1"))
        axes[-1, 0].set_xlabel("time (s)")
        v = g.attrs["variables"]
        v = v.decode() if isinstance(v, bytes) else v
        fig.suptitle("%s %s" % (name, v))
        fig.tight_layout()
        if len(sys.argv) > 3:
            fig.savefig(sys.argv[3])
            print("wrote", sys.argv[3])
            return 0
    plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
