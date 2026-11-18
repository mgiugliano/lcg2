#!/usr/bin/env python3
"""Draw the figures of docs/14 and docs/16 from REAL runs of lcg2 on the NEURON backend.

    python3.11 docs/figures/make_neuron_figures.py        (from the top folder, after `make`)

Needs a Python with NEURON, h5py and matplotlib (the Python of sim_python in the kit's rigs).  Every figure is a
recording made now by ./lcg2 from the files of examples/student_kit/, so the pictures cannot drift from the code.
"""
import glob
import math
import os
import subprocess
import sys
import tempfile

import h5py
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Circle, FancyArrowPatch, Rectangle  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LCG2 = os.path.join(ROOT, "lcg2")
KIT = os.path.join(ROOT, "examples", "student_kit")
WORK = tempfile.mkdtemp(prefix="lcg2_figs_")

INK, GREY, BLUE, ORANGE, GREEN, RED = "#111827", "#6b7280", "#2563eb", "#d97706", "#16a34a", "#dc2626"
plt.rcParams.update({"svg.fonttype": "none", "font.family": "sans-serif", "font.size": 10, "axes.edgecolor": GREY,
                     "axes.labelcolor": INK, "xtick.color": INK, "ytick.color": INK, "text.color": INK,
                     "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.color": "#e5e7eb",
                     "grid.linewidth": 0.6, "figure.dpi": 100, "lines.linewidth": 1.1})
PALETTE = ["#2563eb", "#d97706", "#16a34a", "#dc2626", "#7c3aed", "#0891b2"]


def run(rig, stim, *extra):
    """Run lcg2 in the scratch folder; return the data file of that run."""
    before = set(glob.glob(os.path.join(WORK, "*.h5")))
    r = subprocess.run([LCG2, "-q"] + list(extra) + ["-c", os.path.join(KIT, rig), os.path.join(KIT, stim)], cwd=WORK,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if r.returncode != 0:
        sys.exit("lcg2 failed: %s" % r.stderr)
    new = sorted(set(glob.glob(os.path.join(WORK, "*.h5"))) - before)
    if not new:                                   # two runs in the same second get -2, -3 ...
        new = sorted(glob.glob(os.path.join(WORK, "*.h5")), key=os.path.getmtime)[-1:]
    return new[-1]


def trial(h5, k, kind, name="P1"):
    with h5py.File(h5, "r") as f:
        g = f["trial_%04d" % k]
        return g[kind + "/" + name][:], float(g.attrs["rate_hz"])


def ntrials(h5):
    with h5py.File(h5, "r") as f:
        return len([k for k in f if k.startswith("trial_")])


def tt(n, rate):
    return [i / rate for i in range(n)]


def save(fig, name):
    fig.savefig(os.path.join(HERE, name), format="svg", bbox_inches="tight")
    if os.environ.get("FIG_PNG_DIR"):                      # to look at them while developing
        fig.savefig(os.path.join(os.environ["FIG_PNG_DIR"], name[:-4] + ".png"), dpi=70, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)


def spikes(v, a, b):
    return sum(1 for i in range(a + 1, b) if v[i - 1] < 0 <= v[i])


# ---------------------------------------------------------------------------
def fig_steps():
    h5 = run("rig.ini", "current_steps.sg")
    pick = [0, 2, 3, 4, 8]                      # -100, 0, 50, 100, 300 pA
    fig, axes = plt.subplots(len(pick), 1, sharex=True, sharey=True, figsize=(7.5, 7.0))
    for ax, k in zip(axes, pick):
        cmd, r = trial(h5, k, "ao")
        v, _ = trial(h5, k, "ai")
        amp = cmd[int(0.5 * r)]
        ax.plot(tt(len(v), r), v, color=BLUE, lw=0.9)
        ax.set_ylabel("mV")
        ax.text(0.995, 0.88, "step %d pA" % amp, transform=ax.transAxes, ha="right", va="top", color=ORANGE, fontsize=9,
                weight="bold")
        ax.axvspan(0.2, 1.0, color=ORANGE, alpha=0.08, lw=0)
    axes[-1].set_xlabel("time (s)   (the shaded part is the current step)")
    axes[0].set_title("Current steps on the Hodgkin-Huxley soma (current_steps.sg, rig.ini)", fontsize=10)
    fig.tight_layout()
    save(fig, "fig-nrn-steps.svg")
    return h5


def fig_fi(h5):
    amps, hz, vend = [], [], []
    for k in range(ntrials(h5)):
        v, r = trial(h5, k, "ai")
        cmd, _ = trial(h5, k, "ao")
        amps.append(cmd[int(0.5 * r)])
        hz.append(spikes(v, int(0.2 * r), int(1.0 * r)) / 0.8)
        vend.append(v[int(1.0 * r) - 1])
    fine = run("rig.ini", "current_steps_fine.sg")
    fa, fh = [], []
    for k in range(ntrials(fine)):
        v, r = trial(fine, k, "ai")
        cmd, _ = trial(fine, k, "ao")
        fa.append(cmd[int(0.5 * r)])
        fh.append(spikes(v, int(0.2 * r), int(1.0 * r)) / 0.8)
    fig, (a, b) = plt.subplots(1, 2, figsize=(8.5, 3.4))
    a.plot(amps, hz, "o-", color=BLUE)
    a.set_xlabel("current step (pA)")
    a.set_ylabel("firing rate (Hz)")
    a.set_title("f-I curve", fontsize=10)
    b.plot(fa, fh, "o-", color=ORANGE)
    b.set_xlabel("current step (pA)")
    b.set_title("the threshold, in 10 pA steps", fontsize=10)
    save(fig, "fig-nrn-fi.svg")


def fig_rin():
    h5 = run("rig.ini", "small_step.sg")
    v, r = trial(h5, 0, "ai")
    cmd, _ = trial(h5, 0, "ao")
    a0, a1 = int(0.1 * r), int(0.4 * r)
    base = sum(v[a0 - 1000 // 20:a0]) / (1000 // 20)
    end = sum(v[a1 - 1000 // 20:a1]) / (1000 // 20)
    fig, (c, ax) = plt.subplots(2, 1, sharex=True, figsize=(7.5, 4.6), gridspec_kw={"height_ratios": [1, 2.4]})
    c.plot(tt(len(cmd), r), cmd, color=ORANGE)
    c.set_ylabel("command (pA)")
    ax.plot(tt(len(v), r), v, color=BLUE)
    ax.axhline(base, color=GREY, ls="--", lw=0.8)
    ax.axhline(end, color=GREY, ls="--", lw=0.8)
    ax.annotate("", xy=(0.43, end), xytext=(0.43, base), arrowprops=dict(arrowstyle="<->", color=RED))
    ax.text(0.44, (base + end) / 2, "ΔV = %.2f mV\nR_in = ΔV / ΔI = %.0f MΩ" % (end - base, (end - base) / (cmd[a0 + 5] - cmd[0]) * 1000), color=RED, va="center")
    ax.set_ylabel("membrane potential (mV)")
    ax.set_xlabel("time (s)")
    ax.set_xlim(0, 0.75)
    c.set_title("A small negative step: the response gives the input resistance and the time constant", fontsize=10)
    save(fig, "fig-nrn-rin.svg")


def fig_vc():
    h5 = run("rig.ini", "vc_steps.sg", "-m", "vc")
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(10.5, 3.4), gridspec_kw={"width_ratios": [1, 1.6, 1.3]})
    r = 20000
    vm, peak, end = [], [], []
    for k in range(ntrials(h5)):
        cmd, _ = trial(h5, k, "ao")
        i, _ = trial(h5, k, "ai")
        s0, s1 = int(0.05 * r), int(0.08 * r)
        vm.append(cmd[s0 + 5])
        peak.append(min(i[s0 + 2:s1]) / 1000.0)
        end.append(i[s1 - 1] / 1000.0)
        if k in (0, 1, 2, 3, 4):
            t = [x * 1000 for x in tt(len(i), r)]
            a.plot(t, cmd, color=PALETTE[k % 5])
            b.plot(t, [x / 1000.0 for x in i], color=PALETTE[k % 5], label="%d mV" % cmd[s0 + 5])
    a.set_ylabel("command (mV)")
    a.set_xlabel("time (ms)")
    b.set_ylabel("membrane current (nA)")
    b.set_xlabel("time (ms)")
    b.legend(fontsize=8, frameon=False, ncol=2)
    c.plot(vm, peak, "o-", color=BLUE, label="peak inward")
    c.plot(vm, end, "s-", color=ORANGE, label="end of the step")
    c.axhline(0, color=GREY, lw=0.8)
    c.set_xlabel("step potential (mV)")
    c.set_ylabel("current (nA)")
    c.legend(fontsize=8, frameon=False)
    fig.suptitle("Voltage clamp: held at -70 mV, stepped for 30 ms; the I-V curve on the right", fontsize=10)
    fig.tight_layout()
    save(fig, "fig-nrn-vc.svg")


def fig_pair():
    weak = run("rig_pair.ini", "pair_pulse.sg")
    strong = run("rig_pair.ini", "pair_pulse.sg", "-P", "strong")
    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(7.5, 6.0))
    p1, r = trial(weak, 0, "ai", "P1")
    t = [x * 1000 for x in tt(len(p1), r)]
    axes[0].plot(t, p1, color=BLUE)
    axes[0].set_ylabel("P1 (mV)")
    p2, _ = trial(weak, 0, "ai", "P2")
    axes[1].plot(t, p2, color=GREEN)
    axes[1].set_ylabel("P2, weight 0.3 nS (mV)")
    p2s, _ = trial(strong, 0, "ai", "P2")
    axes[2].plot(t, p2s, color=RED)
    axes[2].set_ylabel("P2, weight 10 nS (mV)")
    axes[2].set_xlabel("time (ms)")
    for ax in axes:
        ax.set_xlim(40, 110)
    spike = next(i for i, v in enumerate(p1) if v > 0) / r * 1000
    for ax in axes:
        ax.axvline(spike, color=GREY, ls=":", lw=0.9)
        ax.axvline(spike + 2, color=GREY, ls=":", lw=0.9)
    axes[1].text(spike + 2.5, min(p2) + 0.2 * (max(p2) - min(p2)), "synaptic delay 2 ms", color=GREY, fontsize=8)
    axes[0].set_title("Two cells and a synapse (rig_pair.ini): drive P1, record P2", fontsize=10)
    save(fig, "fig-nrn-pair.svg")


def fig_models():
    runs = [("hh_soma", ["rig.ini"], ""), ("ball_and_stick", ["rig.ini", "-P", "ballstick"], ""),
            ("pyramidal.py", ["rig.ini", "-P", "pyramidal"], ""), ("simple_cell.hoc", ["rig.ini", "-P", "hoc"], "")]
    fig, axes = plt.subplots(2, 2, sharex=True, sharey=True, figsize=(8, 5))
    for ax, (name, a, _) in zip(axes.flat, runs):
        h5 = run(a[0], "current_steps.sg", *a[1:])
        for c, k in zip([PALETTE[0], PALETTE[1]], (0, 4)):    # -100 and 100 pA
            v, r = trial(h5, k, "ai")
            cmd, _ = trial(h5, k, "ao")
            ax.plot(tt(len(v), r), v, color=c, lw=0.8, label="%d pA" % cmd[int(0.5 * r)])
        ax.set_title(name, fontsize=9)
        ax.set_xlim(0.1, 0.5)
    axes[0, 0].legend(fontsize=7, frameon=False, loc="upper right")
    for ax in axes[1]:
        ax.set_xlabel("time (s)")
    for ax in axes[:, 0]:
        ax.set_ylabel("mV")
    fig.suptitle("The same -100 and +100 pA steps on four cells, chosen with -P (the first 0.3 s of the step)", fontsize=10)
    fig.tight_layout()
    save(fig, "fig-nrn-models.svg")


def fig_location():
    soma = run("rig.ini", "small_step.sg", "-P", "ballstick")
    dend = run("rig.ini", "small_step.sg", "-P", "ballstick_dend")
    fig, ax = plt.subplots(figsize=(7, 3.6))
    for h5, lab, c in ((soma, "electrode at soma(0.5)", BLUE), (dend, "electrode at dend(1), the dendrite tip", ORANGE)):
        v, r = trial(h5, 0, "ai")
        ax.plot([x * 1000 for x in tt(len(v), r)], v, color=c, label=lab)
    ax.set_xlabel("time (ms)")
    ax.set_ylabel("membrane potential (mV)")
    ax.set_xlim(80, 450)
    ax.legend(frameon=False, fontsize=9)
    ax.set_title("The same -20 pA step on ball_and_stick, from two electrode positions", fontsize=10)
    save(fig, "fig-nrn-location.svg")


def fig_sag():
    ih = run("rig_ih.ini", "sag_step.sg")
    no = run("rig_ih.ini", "sag_step.sg", "-P", "noih")
    fig, ax = plt.subplots(figsize=(7.5, 3.8))
    for h5, lab, c in ((ih, "with Ih (gh = 0.0005 S/cm²)", BLUE), (no, "without (gh = 0)", GREY)):
        v, r = trial(h5, 0, "ai")
        ax.plot(tt(len(v), r), v, color=c, label=lab)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("membrane potential (mV)")
    ax.set_ylim(-85, -58)
    ax.set_xlim(0.1, 1.6)
    ax.legend(frameon=False, loc="lower right")
    ax.set_title("Your own channel (channels/ih.mod): a -100 pA step sags back with Ih\n(the spike at the end of the step is the rebound, cut off)", fontsize=10)
    save(fig, "fig-nrn-sag.svg")


def fig_custom():
    fig, ax = plt.subplots(figsize=(7.5, 3.8))
    for prof, lab, c in (("short", "dend_length = 50 µm", PALETTE[0]), (None, "200 µm", PALETTE[2]), ("long", "800 µm", PALETTE[1])):
        h5 = run("rig_custom.ini", "small_step.sg", *(["-P", prof] if prof else []))
        v, r = trial(h5, 0, "ai")
        ax.plot([x * 1000 for x in tt(len(v), r)], v, color=c, label=lab)
    ax.set_xlim(80, 450)
    ax.set_xlabel("time (ms)")
    ax.set_ylabel("membrane potential (mV)")
    ax.legend(frameon=False)
    ax.set_title("two_compartment.py: a longer dendrite is more membrane, so a lower input resistance", fontsize=10)
    save(fig, "fig-nrn-custom.svg")


def fig_morph():
    pts = {}
    for line in open(os.path.join(KIT, "tiny.swc")):
        if line.startswith("#") or not line.strip():
            continue
        i, t, x, y, z, rad, par = line.split()
        pts[int(i)] = (int(t), float(x), float(y), float(rad), int(par))
    h5 = run("rig_morphology.ini", "small_step.sg")
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(10, 3.6), gridspec_kw={"width_ratios": [0.5, 1, 1.4]})
    for i, (t, x, y, rad, par) in pts.items():
        if t == 1:
            a.add_patch(Circle((x, y), rad, color=BLUE, alpha=0.8))
        elif par in pts:
            _, px, py, prad, _ = pts[par]
            a.plot([px, x], [py, y], color=ORANGE, lw=max(1.5, rad * 2))
    a.set_aspect("equal")
    a.set_xlim(-30, 30)
    a.set_ylim(-110, 125)
    a.set_title("tiny.swc", fontsize=9)
    a.set_xlabel("µm")
    a.grid(False)
    v, r = trial(h5, 0, "ai")
    n = int(0.02 * r)
    b.plot([x * 1000 for x in tt(n, r)], v[:n], color=BLUE)
    b.set_xlabel("time (ms)")
    b.set_ylabel("soma potential (mV)")
    b.set_title("start_depolarised.py: the cell\nstarts at -70 mV and relaxes", fontsize=9)
    c.plot([x * 1000 for x in tt(len(v), r)], v, color=BLUE)
    c.set_xlim(50, 450)
    c.set_ylim(-75, -63)
    c.set_xlabel("time (ms)")
    c.set_title("then answers a -20 pA step", fontsize=9)
    fig.tight_layout()
    save(fig, "fig-nrn-morphology.svg")


def fig_cells():
    fig, ax = plt.subplots(figsize=(10, 3.6))
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 36)
    ax.axis("off")
    ax.grid(False)
    # hh_soma
    ax.add_patch(Circle((12, 18), 5, color=BLUE, alpha=0.85))
    ax.text(12, 8, "hh_soma", ha="center", fontsize=10, weight="bold")
    ax.text(12, 4, "one HH compartment", ha="center", fontsize=8, color=GREY)
    # electrode IClamp
    ax.plot([17, 24], [18, 22], color=INK)
    ax.add_patch(FancyArrowPatch((30, 22), (24, 22), arrowstyle="-|>", color=ORANGE, mutation_scale=12))
    ax.text(31, 22, "IClamp: a command in pA\n(records mV)", va="center", fontsize=8)
    # ball and stick
    ax.add_patch(Circle((50, 18), 4, color=BLUE, alpha=0.85))
    ax.add_patch(Rectangle((54, 17.2), 26, 1.6, color=ORANGE, alpha=0.85))
    ax.text(62, 8, "ball_and_stick", ha="center", fontsize=10, weight="bold")
    ax.text(62, 4, "HH soma + passive dendrite", ha="center", fontsize=8, color=GREY)
    ax.plot([50, 50], [22, 28], color=INK)
    ax.text(50, 30, "soma(0.5)", ha="center", fontsize=8)
    ax.plot([80, 80], [19, 28], color=INK)
    ax.text(80, 30, "dend(1)", ha="center", fontsize=8)
    ax.text(50, 33.5, "electrode = ...  chooses the place", ha="center", fontsize=8, color=GREY)
    # SEClamp
    ax.plot([80, 88], [18, 22], color=INK)
    ax.text(92, 22, "SEClamp:\na command in mV\n(records pA)", va="center", ha="center", fontsize=8)
    ax.text(50, 0.5, "the unit of the command decides the clamp; the unit of the input decides what is recorded", ha="center", fontsize=8, color=GREY)
    save(fig, "fig-nrn-cells.svg")


if __name__ == "__main__":
    for tool in (LCG2,):
        if not os.path.exists(tool):
            sys.exit("run `make` first")
    h5 = fig_steps()
    fig_fi(h5)
    fig_rin()
    fig_vc()
    fig_pair()
    fig_models()
    fig_location()
    fig_sag()
    fig_custom()
    fig_morph()
    fig_cells()
