#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M3b acceptance test: device = neuron, the NEURON simulator as a backend.
#
#     python3 tests/test_neuron.py        (after `make`)
#
# It needs a Python that has both NEURON and h5py: this script finds one (the one running it,
# $LCG2_NEURON_PYTHON, or a Homebrew Python) and runs itself with it.  If there is none it SKIPS, loudly,
# with exit status 0, so that `make test` stays usable on a machine without NEURON (the checks that need
# no NEURON still run).
#
# What it proves:
#   * the recording equals a plain NEURON script (an IClamp, h.fadvance, the same dt) to 1e-9 mV: the
#     units, the gains, the sample timing and the plumbing through the pipe are right;
#   * voltage clamp: a step from -70 to 0 mV shows an inward (negative) transient, as on a real cell;
#   * two cells and a synapse: driving P1 only, P2 answers after the synaptic delay;
#   * the cell keeps its state across the pause between trials;
#   * external models (Python, hoc, SWC + biophysics + init, compiled .mod files) all build and run, with
#     paths relative to the configuration file, from any folder;
#   * mistakes are explained: unknown parameter, missing file, no NEURON in sim_python, a pipette
#     without a cell, a synapse between cells that do not exist;
#   * a killed adapter ends the run with status 2 and the completed trials are readable; Ctrl-C ends it
#     cleanly and leaves no adapter behind; the run folder holds network.json, the model files and
#     neuron.log, all in SHA256SUMS.
# Every real run happens in a scratch folder, never in the repository.
import glob
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("LCG2_BUILD", os.path.join(ROOT, "build"))
LCG2 = os.environ.get("LCG2_BIN", os.path.join(ROOT, "lcg2"))
EX = lambda name: os.path.join(ROOT, "examples", name)           # noqa: E731
MODELS = os.path.join(ROOT, "examples", "models")

# ---- find an interpreter with NEURON and h5py, and re-run with it ----------------------------
def has(py, mods):
    return subprocess.run([py, "-I", "-c", "import " + ", ".join(mods)], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def find_python():
    cands = [sys.executable]
    if os.environ.get("LCG2_NEURON_PYTHON"):
        cands.insert(0, os.environ["LCG2_NEURON_PYTHON"])
    cands += ["/opt/homebrew/opt/python@3.11/bin/python3.11", "/opt/homebrew/opt/python@3.12/bin/python3.12"]
    for n in ("python3.11", "python3.12", "python3.10", "python3"):
        if shutil.which(n):
            cands.append(shutil.which(n))
    for c in cands:
        if os.path.exists(c) and has(c, ["neuron", "h5py"]):
            return c
    return None


os.environ.pop("LCG2_SIM_PYTHON", None)        # the tests name their Python in the files they write
PY = None if os.environ.get("LCG2_NO_NEURON") else find_python()
if PY is None:
    print("SKIPPED  device = neuron: no Python with both NEURON and h5py was found.\n"
          "         Install them (pip install neuron h5py) or set LCG2_NEURON_PYTHON.  Nothing of M3b was tested.")
    sys.exit(0)
if os.path.realpath(PY) != os.path.realpath(sys.executable):
    os.execv(PY, [PY] + sys.argv)

import array   # noqa: E402
import re      # noqa: E402
import h5py    # noqa: E402

failures = []
count = 0


def check(name, ok, detail=""):
    global count
    count += 1
    if not ok:
        failures.append(name)
        print("FAIL  %s" % name)
        for line in str(detail).splitlines()[:12]:
            print("      " + line)


def go(cmd, cwd, **kw):
    kw.setdefault("timeout", 240)
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
                          errors="replace", **kw)


def newdir(tmp, name):
    d = os.path.join(tmp, name)
    os.makedirs(d)
    return d


def h5_of(cwd):
    f = sorted(glob.glob(os.path.join(cwd, "*.h5")))
    return f[-1] if f else None


def folder_of(cwd):
    d = sorted(glob.glob(os.path.join(cwd, ".2*")))
    return d[-1] if d else None


def fix_py(ini_text):
    """The kit names `python3`: make the copy name the Python that runs this test."""
    return re.sub(r"^sim_python\s*=[^\n]*$", "sim_python = " + PY, ini_text, flags=re.M)


def text(x):
    return x.decode() if isinstance(x, bytes) else x


RIG = """[rig]
device = neuron
sim_python = %(py)s
mode = cc
rate = 20kHz
%(rig)s
[out P1]
line = ao0
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV
[in P1]
line = ai0
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA
range = 10
%(p2)s
%(cells)s
"""
P2 = """[out P2]
line = ao1
gain_cc = 400
unit_cc = pA
[in P2]
line = ai1
gain_cc = 100
unit_cc = mV
"""


def write_ini(cwd, cells="[cell P1]\nmodel = hh_soma\n", rig="", p2=False, name="rig.ini"):
    path = os.path.join(cwd, name)
    with open(path, "w") as f:
        f.write(RIG % dict(py=PY, rig=rig, p2=P2 if p2 else "", cells=cells))
    return path


def write(path, s):
    with open(path, "w") as f:
        f.write(s)
    return path


def lcg(args, cwd, **kw):
    return go([LCG2] + args, cwd, **kw)


def trace(h5, trial, name):
    with h5py.File(h5, "r") as f:
        return f["trial_%04d/ai/%s" % (trial, name)][:]


STEP = """sg 2 stimulus
rate 20kHz
channel P1 unit=pA {
    500ms  dc(0)
    1s     dc(200)
    500ms  dc(0)
}
"""

REF = """
import sys, array
from neuron import h
h.load_file("stdrun.hoc")
s = h.Section(name="soma"); s.L = s.diam = 20; s.nseg = 1; s.insert("hh")
h.celsius = 6.3
ic = h.IClamp(s(0.5)); ic.delay = 500; ic.dur = 1000; ic.amp = 0.2
h.dt = 0.025; h.finitialize(-65.0)
v = []
for i in range(40000):
    h.fadvance(); h.fadvance(); v.append(s(0.5).v)
open(sys.argv[1], "wb").write(array.array("d", v).tobytes())
"""


# ---------------------------------------------------------------------------------------------

def test_reference(tmp):
    d = newdir(tmp, "ref")
    ini = write_ini(d)
    stim = write(os.path.join(d, "step.sg"), STEP)
    r = lcg(["-q", "-c", ini, stim], d)
    check("a run on device = neuron succeeds", r.returncode == 0, r.stderr)
    refpath = os.path.join(d, "ref.py")
    write(refpath, REF)
    go([PY, refpath, os.path.join(d, "ref.bin")], d)
    a = array.array("d")
    with open(os.path.join(d, "ref.bin"), "rb") as f:
        a.frombytes(f.read())
    x = trace(h5_of(d), 0, "P1")
    diff = max(abs(p - q) for p, q in zip(a, x))
    check("the recording equals a plain NEURON IClamp script (max difference %.2e mV)" % diff,
          len(x) == 40000 and diff < 1e-9, "n=%d diff=%g" % (len(x), diff))
    check("the cell fired: the step of 200 pA made a spike", x.max() > 20 and x[:9000].max() < -60,
          "max %g, rest max %g" % (x.max(), x[:9000].max()))
    with h5py.File(h5_of(d), "r") as f:
        cmd = f["trial_0000/ao/P1"][:]
        g = f["trial_0000"]
        check("the command is recorded as played (pA)", abs(cmd[9999]) < 1e-9 and abs(cmd[10000] - 200) < 1e-9 and abs(cmd[29999] - 200) < 1e-9 and abs(cmd[30000]) < 1e-9)
        check("the trial says it was simulated time", g.attrs["virtual_time"] == 1)
        check("device is neuron in the file", text(f.attrs["device"]) == "neuron")
        prov = json.loads(f["provenance"].asstr()[()])
        check("the provenance names the adapter and its digest", prov["neuron"]["adapter_sha256"] != "unreadable"
              and prov["neuron"]["adapter"].endswith("lcg2_neuron.py"), json.dumps(prov.get("neuron")))
    # calibration and gain are exercised: a gain of 200 mV/V gives different volts, same millivolts
    ini2 = write_ini(d, name="rig2.ini")
    s = open(ini2).read().replace("gain_cc = 100", "gain_cc = 200", 1).replace("gain_cc = 400", "gain_cc = 800", 1)
    s = s.replace("[in P1]\n", "[in P1]\ncal_scale = 0.9\ncal_offset = 0.01\n", 1)
    write(ini2, s)
    d2 = newdir(tmp, "ref2")
    r = lcg(["-q", "-c", ini2, os.path.join(d, "step.sg")], d2)
    y = trace(h5_of(d2), 0, "P1")
    check("gain and calibration cancel out on the way back (same millivolts)",
          r.returncode == 0 and max(abs(p - q) for p, q in zip(x, y)) < 1e-9)


def test_overrange(tmp):
    d = newdir(tmp, "over")
    ini = write_ini(d)
    s = open(ini).read().replace("range = 10", "range = 1")       # 1 V at 100 mV/V = +-100 mV: the spike peaks near 40 mV
    s = s.replace("gain_cc = 100", "gain_cc = 20")                 # now 20 mV/V: +-20 mV, the spike is clipped
    write(ini, s)
    stim = write(os.path.join(d, "step.sg"), STEP)
    r = lcg(["-c", ini, stim], d)
    with h5py.File(h5_of(d), "r") as f:
        n = f["trial_0000/ai/P1"].attrs["overrange_samples"]
    check("a voltage beyond the input range is clamped and counted, never silent",
          r.returncode == 0 and n > 0 and "beyond the range" in open(os.path.join(folder_of(d), "log.txt")).read(),
          r.stderr + str(n))


VC = """sg 2 stimulus
rate 20kHz
channel P1 unit=mV {
    100ms  dc(-70)
    @step
    50ms   dc(0)
    100ms  dc(-70)
}
"""


def test_vc(tmp):
    d = newdir(tmp, "vc")
    ini = write_ini(d)
    stim = write(os.path.join(d, "vc.sg"), VC)
    r = lcg(["-q", "-m", "vc", "-c", ini, stim], d)
    check("voltage clamp runs", r.returncode == 0, r.stderr)
    x = trace(h5_of(d), 0, "P1")                  # pA
    i0 = 2000                                     # the step starts at sample 2000 (100 ms)
    early = x[i0 + 20:i0 + 400]                   # 1 to 20 ms into the step: the sodium current
    check("voltage clamp: a step from -70 to 0 mV gives an inward (negative) transient (min %.0f pA)" % early.min(),
          early.min() < -500, "min %g" % early.min())
    check("voltage clamp: the capacitive transient at the step's start is positive (outward)", x[i0:i0 + 5].max() > 100,
          str(x[i0:i0 + 5]))
    check("voltage clamp: the holding current at -70 mV is small", abs(x[1500]) < 500, str(x[1500]))
    with h5py.File(h5_of(d), "r") as f:
        check("voltage clamp: input in pA", text(f["trial_0000/ai/P1"].attrs["unit"]) == "pA")
        check("voltage clamp: the command is mV", text(f["trial_0000/ao/P1"].attrs["unit"]) == "mV")


PAIR_STIM = """sg 2 stimulus
rate 20kHz
channel P1 unit=pA {
    50ms   dc(0)
    @pulse
    2ms    dc(1500)
    148ms  dc(0)
}
"""


def test_pair(tmp):
    d = newdir(tmp, "pair")
    cells = ("[cell P1]\nmodel = hh_soma\n[cell P2]\nmodel = hh_soma\n[synapse P1_P2]\npre = P1\npost = P2\n"
             "type = exp2\ntau1 = 0.5ms\ntau2 = 3ms\nweight = 0.01\ndelay = 2ms\n")
    ini = write_ini(d, cells=cells, p2=True)
    stim = write(os.path.join(d, "pair.sg"), PAIR_STIM)
    r = lcg(["-q", "-c", ini, stim], d)
    check("two cells and a synapse run", r.returncode == 0, r.stderr)
    p1, p2 = trace(h5_of(d), 0, "P1"), trace(h5_of(d), 0, "P2")
    spike = next((i for i, v in enumerate(p1) if v > 0), None)
    check("P1 spiked", spike is not None, "max %g" % p1.max())
    if spike is not None:
        base = p2[:900].mean()
        check("P2 is quiet before the spike (nothing but its own rest)", abs(p2[:spike].max() - p2[:spike].min()) < 0.1,
              "%g" % (p2[:spike].max() - p2[:spike].min()))
        onset = next((i for i in range(spike, len(p2)) if p2[i] - base > 0.05), None)
        check("P2 answers with a depolarisation (peak %.2f mV)" % (p2.max() - base), p2.max() - base > 0.2,
              "peak %g" % (p2.max() - base))
        if onset is not None:
            lag_ms = (onset - spike) / 20.0
            check("the answer comes after the synaptic delay of 2 ms (lag %.2f ms)" % lag_ms, 2.0 <= lag_ms <= 5.0, str(lag_ms))
        else:
            check("P2 shows an onset", False)
    # P2 is not driven: an idle output has no dataset, and P2 is recorded
    with h5py.File(h5_of(d), "r") as f:
        check("P2 is recorded and has no command dataset (it is idle)", "P2" in f["trial_0000/ai"] and "P2" not in f["trial_0000/ao"])
    # no synapse: P2 stays flat
    d2 = newdir(tmp, "nosyn")
    ini2 = write_ini(d2, cells="[cell P1]\nmodel = hh_soma\n[cell P2]\nmodel = hh_soma\n", p2=True)
    r = lcg(["-q", "-c", ini2, write(os.path.join(d2, "pair.sg"), PAIR_STIM)], d2)
    q2 = trace(h5_of(d2), 0, "P2")
    check("without the synapse P2 does not move", r.returncode == 0 and q2.max() - q2.min() < 0.1, "%g" % (q2.max() - q2.min()))


PROTO = """sg 2 protocol
repeat 2
period %s

stimulus {
    channel P1 unit=pA {
        20ms dc(0)
        3ms  dc(1500)
        7ms  dc(0)
    }
}
"""


def test_state_between_trials(tmp):
    first = {}
    for label, period in (("short", "40ms"), ("long", "5s")):
        d = newdir(tmp, "state_" + label)
        ini = write_ini(d)
        r = lcg(["-q", "-c", ini, write(os.path.join(d, "p.sg"), PROTO % period)], d)
        check("a protocol with period %s runs" % period, r.returncode == 0, r.stderr)
        first[label] = (trace(h5_of(d), 0, "P1"), trace(h5_of(d), 1, "P1"))
    t0, t1 = first["short"]
    check("trial 1 of the short protocol starts where the cell was left (the cell keeps its state)",
          abs(t1[0] - t0[0]) > 0.05 or abs(t1[:200] - t0[:200]).max() > 0.05, "%g %g" % (t0[0], t1[0]))
    l0, l1 = first["long"]
    check("after a long pause the cell is back at rest: trial 2 repeats trial 1", abs(l1 - l0).max() < 0.1,
          "%g" % abs(l1 - l0).max())
    check("the first trial does not depend on the pause that follows it", abs(t0 - l0).max() < 1e-9)


def test_models(tmp):
    d = newdir(tmp, "models")
    work = newdir(tmp, "models_cwd")
    # the package: configuration and model files in one folder, run from another
    pkg = newdir(tmp, "package")
    for f in ("pyramidal.py", "simple_cell.hoc", "tiny.swc", "cell_tuning.py", "start_depolarised.py"):
        shutil.copy(os.path.join(MODELS, f), pkg)
    shutil.copytree(os.path.join(MODELS, "mod"), os.path.join(pkg, "mod"))
    cells = """[cell P1]
model = pyramidal.py
apical_length = 200
electrode = apical(0.5)

[cell P2]
model = simple_cell.hoc
template = SimpleCell
electrode = dend[1](0.5)

[cell P3]
morphology = tiny.swc
biophysics = cell_tuning.py
init = start_depolarised.py

[cell P4]
model = ball_and_stick
mechanisms = mod
electrode = dend(1)
"""
    ini = write_ini(pkg, cells=cells, p2=True)
    s = open(ini).read()
    for k, line in ((3, "ao2"), (4, "ao3")):
        s += "[out P%d]\nline = %s\ngain_cc = 400\nunit_cc = pA\n[in P%d]\nline = ai%d\ngain_cc = 100\nunit_cc = mV\n" % (k, line, k, k)
    write(ini, s)
    r = lcg(["-n", "-c", ini, EX("step.sg")], work)
    check("-n builds all four kinds of model (python, hoc, swc+biophysics+init, mod) from another folder",
          r.returncode == 0 and "cell P1" in r.stdout and "cell P4" in r.stdout and "4 cell(s)" in r.stdout, r.stdout + r.stderr)
    check("-n shows the electrodes and where each cell rests", "electrode apical(0.5)" in r.stdout
          and "electrode dend[1](0.5)" in r.stdout and "rests at" in r.stdout, r.stdout)
    check("-n played nothing and wrote nothing", not glob.glob(os.path.join(work, "*.h5")) and not glob.glob(os.path.join(work, ".2*")))
    # run it: P3 starts at -70 mV because of its init file
    stim = write(os.path.join(work, "four.sg"), STEP)
    r = lcg(["-q", "-c", ini, stim], work)
    check("a run with four external models works", r.returncode == 0, r.stderr)
    if r.returncode == 0:
        p3 = trace(h5_of(work), 0, "P3")
        p1 = trace(h5_of(work), 0, "P1")
        check("the init file took effect: P3 starts well below the -65 mV rest, as its init file says (%.1f) and drifts to rest" % p3[0], -70.5 < p3[0] < -67 and p3[8000] > p3[0] + 0.5,
              "%g %g" % (p3[0], p3[8000]))
        check("the python model fired on 200 pA at the apical dendrite? (current clamp at the dendrite works)", p1.max() > p1[0], "")
        fol = folder_of(work)
        files = sorted(os.listdir(os.path.join(fol, "files")))
        check("the model files were copied into the run folder", all(any(n.endswith(x) for n in files) for x in
              ("pyramidal.py", "simple_cell.hoc", "tiny.swc", "cell_tuning.py", "start_depolarised.py", "slowleak.mod")), str(files))
        sums = open(os.path.join(fol, "SHA256SUMS")).read()
        check("network.json and neuron.log are in SHA256SUMS", "network.json" in sums and "neuron.log" in sums, sums)
        net = json.load(open(os.path.join(fol, "network.json")))
        check("network.json describes the cells", [c["name"] for c in net["cells"]] == ["P1", "P2", "P3", "P4"] and net["neuron"],
              str(net)[:300])
        v = go(["shasum", "-a", "256", "-c", os.path.relpath(os.path.join(fol, "SHA256SUMS"), work)], work)
        check("shasum -c verifies the whole folder, the adapter's log included", v.returncode == 0, v.stdout + v.stderr)


def test_mistakes(tmp):
    d = newdir(tmp, "bad")
    stim = write(os.path.join(d, "step.sg"), STEP)

    def bad(label, ini_text, expect, args=("-n",), exit_code=1):
        path = write(os.path.join(d, "bad.ini"), ini_text)
        r = lcg(list(args) + ["-c", path, stim], d)
        out = r.stdout + r.stderr
        check("%s -> %s" % (label, expect), r.returncode == exit_code and expect in out, "exit %d\n%s" % (r.returncode, out))

    good = open(write_ini(d)).read()
    bad("a pipette without a cell", good.replace("[cell P1]\nmodel = hh_soma\n", "[cell Q9]\nmodel = hh_soma\n"),
        "[out P1] has no [cell P1]")
    bad("a synapse between cells that do not exist", good + "[synapse A_B]\npre = P1\npost = Z\n", "'Z' is not a [cell]")
    bad("a synapse with an unknown key", good + "[cell P2]\nmodel = hh_soma\n[synapse A_B]\npre = P1\npost = P2\nspeed = 1\n",
        "unknown key (allowed:")
    bad("an unknown model parameter", good + "gnabr = 0.2\n", "'gnabr' is not a parameter of the model hh_soma")
    bad("a model file that is not there", good.replace("model = hh_soma", "model = nothere.py"), "does not exist")
    write(os.path.join(d, "x.txt"), "not a model")
    bad("a model of an unknown kind", good.replace("model = hh_soma", "model = x.txt"), "is not hh_soma, ball_and_stick")
    bad("an electrode on a section the cell does not have", good + "electrode = dendrite(0.5)\n", "has no section 'dendrite'")
    bad("an electrode position outside 0 to 1", good + "electrode = soma(1.5)\n", "not between 0 and 1")
    bad("sim_python that does not exist", good.replace("sim_python = " + PY, "sim_python = /no/such/python"), "cannot run /no/such/python")
    # a Python without NEURON: its own message names sim_python
    nonrn = "/usr/bin/python3"
    if os.path.exists(nonrn) and not has(nonrn, ["neuron"]):
        bad("a sim_python without NEURON", good.replace("sim_python = " + PY, "sim_python = " + nonrn), "cannot import NEURON")
    bad("sim_dt that is not a duration", good.replace("[rig]\n", "[rig]\nsim_dt = fast\n"), "expected a duration")
    d2 = newdir(tmp, "bad2")
    r = lcg(["-n", "-c", write(os.path.join(d2, "y.ini"), good.replace("device = neuron", "device = sim")), stim], d2)
    check("sim_python is refused on device = sim", r.returncode == 1 and "only used with device = neuron" in r.stderr, r.stderr)


def children_of(pid):
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="], stdout=subprocess.PIPE, universal_newlines=True).stdout
    return [int(a) for a, b in (l.split() for l in out.splitlines() if l.strip()) if int(b) == pid]


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


PROTO_LONG = """sg 2 protocol
repeat 6
period 2s

stimulus {
    channel P1 unit=pA { 1s dc(100) }
}
"""


def start_long(tmp, name):
    d = newdir(tmp, name)
    ini = write_ini(d)
    proc = subprocess.Popen([LCG2, "-R", "-c", ini, write(os.path.join(d, "long.sg"), PROTO_LONG)], cwd=d,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    deadline = time.time() + 40
    kids = []
    while time.time() < deadline and not kids:
        kids = children_of(proc.pid)
        time.sleep(0.2)
    return d, proc, kids


def test_kill_adapter(tmp):
    d, proc, kids = start_long(tmp, "kill")
    check("the adapter is a child of lcg2", len(kids) == 1, str(kids))
    time.sleep(4.5)                                   # trial 0 and 1 are done (real time, period 2 s)
    for k in kids:
        os.kill(k, signal.SIGKILL)
    try:
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        check("a killed adapter does not hang lcg2", False)
    check("a killed adapter ends the run with status 2 and says so (not a crash of lcg2)",
          proc.returncode == 2 and "NEURON adapter" in err and "stopped" in err and "Traceback" not in err,
          "exit %s\n%s" % (proc.returncode, err))
    h5 = h5_of(d)
    ok = False
    try:
        with h5py.File(h5, "r") as f:
            done = [k for k in f if k.startswith("trial_") and not f[k].attrs["incomplete"]]
            ok = len(done) >= 1 and len(f[done[0] + "/ai/P1"][:]) == 20000
    except Exception as e:
        print("      could not read:", e)
    check("the trials completed before the kill are readable", ok)
    check("the log says the outputs are at rest / the run failed", "FAILED" in open(os.path.join(folder_of(d), "log.txt")).read())


def test_ctrl_c(tmp):
    d, proc, kids = start_long(tmp, "sigint")
    time.sleep(2.5)
    proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        check("Ctrl-C does not hang", False)
    check("Ctrl-C ends the run with status 2: aborted, outputs at rest", proc.returncode == 2 and "aborted" in err,
          "exit %s\n%s" % (proc.returncode, err))
    time.sleep(0.5)
    check("no adapter is left behind after Ctrl-C", all(not alive(k) for k in kids), str(kids))
    with h5py.File(h5_of(d), "r") as f:
        trials = sorted(k for k in f if k.startswith("trial_"))
        flags = [int(f[k].attrs["incomplete"]) for k in trials]
    check("the interrupted trial is kept and marked incomplete", flags and flags[-1] == 1 and flags[:-1] == [0] * (len(flags) - 1),
          str(flags))


def test_slow_check_tolerated(tmp):
    d = newdir(tmp, "adapter_check")
    r = go([PY, os.path.join(ROOT, "neuron", "lcg2_neuron.py"), "--check", os.path.join(MODELS, "t1.ini")], d)
    check("lcg2_neuron.py --check builds a configuration on its own", r.returncode == 0 and "4 cell(s)" in r.stdout, r.stdout + r.stderr)
    r = go([PY, os.path.join(ROOT, "neuron", "lcg2_neuron.py"), "--check", write(os.path.join(d, "e.ini"), "[cell A]\nmodel = nope.py\n")], d)
    check("...and explains a mistake without a traceback", r.returncode == 1 and "does not exist" in r.stderr and "Traceback" not in r.stderr, r.stderr)


KIT = os.path.join(ROOT, "examples", "student_kit")


def test_kit(tmp):
    """The student kit works as the README says, from another folder, for every profile."""
    d = newdir(tmp, "kit")
    # the kit names the Python of the development Mac: use the one running this test
    for f in os.listdir(KIT):
        if f.endswith(".ini"):
            s = fix_py(open(os.path.join(KIT, f)).read())
            write(os.path.join(d, f), s)
        elif not os.path.isdir(os.path.join(KIT, f)):
            shutil.copy(os.path.join(KIT, f), d)
    work = newdir(tmp, "kit_work")
    ch = os.path.join(d, "characterise.py")
    r = lcg(["-q", "-c", os.path.join(d, "rig.ini"), os.path.join(d, "current_steps.sg")], work)
    check("the kit: current steps run", r.returncode == 0, r.stderr)
    o = go([PY, ch, "fi", h5_of(work)], work).stdout
    check("the kit: f-I analysis finds the resting potential and an input resistance",
          "resting potential: -65.0 mV" in o and "input resistance" in o and "amp=300pA" in o, o)
    work = newdir(tmp, "kit_vc")
    r = lcg(["-q", "-m", "vc", "-c", os.path.join(d, "rig.ini"), os.path.join(d, "vc_steps.sg")], work)
    o = go([PY, ch, "iv", h5_of(work)], work).stdout
    inward = [float(l.split()[1]) for l in o.splitlines() if l.startswith("vm=")]
    check("the kit: voltage clamp gives a large inward current and no clipping", r.returncode == 0 and min(inward) < -5000
          and "beyond the range" not in open(os.path.join(folder_of(work), "log.txt")).read(), o + r.stderr)
    work = newdir(tmp, "kit_pair")
    r = lcg(["-q", "-c", os.path.join(d, "rig_pair.ini"), os.path.join(d, "pair_pulse.sg")], work)
    o = go([PY, ch, "psp", h5_of(work)], work).stdout
    check("the kit: the second cell answers the first", r.returncode == 0 and "P2 rises" in o and "P1 fires" in o, o + r.stderr)
    for extra in (["-P", "ballstick"], ["-P", "pyramidal"], ["-P", "hoc"]):
        work = newdir(tmp, "kit_" + extra[1])
        r = lcg(["-q"] + extra + ["-c", os.path.join(d, "rig.ini"), os.path.join(d, "current_steps.sg")], work)
        check("the kit: profile %s runs" % extra[1], r.returncode == 0, r.stderr)
    work = newdir(tmp, "kit_morph")
    r = lcg(["-q", "-c", os.path.join(d, "rig_morphology.ini"), os.path.join(d, "current_steps.sg")], work)
    check("the kit: the morphology rig runs", r.returncode == 0, r.stderr)
    if r.returncode == 0:
        with h5py.File(h5_of(work), "r") as f:
            v = f["trial_0008/ai/P1"][:]
        check("the morphology cell has HH channels in its soma: it fires on 300 pA", (v > 0).any() and v[:2000].max() < -60,
              "max %g" % v.max())


def test_hoc_hooks(tmp):
    d = newdir(tmp, "hoc_hooks")
    write(os.path.join(d, "bio.hoc"), "proc biophysics() { forsec $o1 { insert pas  g_pas = 0.0002  e_pas = -60 } }\n")
    write(os.path.join(d, "ini.hoc"), "proc initialize() { forsec $o1 { v = -50 } }\n")
    ini = write(os.path.join(d, "c.ini"), "[cell A]\nmorphology = %s\nbiophysics = bio.hoc\ninit = ini.hoc\n" % os.path.join(MODELS, "tiny.swc"))
    r = go([PY, os.path.join(ROOT, "neuron", "lcg2_neuron.py"), "--check", ini], d)
    check("hoc biophysics and init procedures run (the cell starts at -50 mV, as the init proc says)",
          r.returncode == 0 and "rests at -50.0 mV" in r.stdout, r.stdout + r.stderr)


def test_tutorials(tmp):
    """The numbers the tutorials (docs/16) quote: they must stay true."""
    d = newdir(tmp, "tut")
    for f in os.listdir(KIT):
        p = os.path.join(KIT, f)
        if f.endswith(".ini"):
            write(os.path.join(d, f), fix_py(open(p).read()))
        elif os.path.isdir(p):
            shutil.copytree(p, os.path.join(d, f))
        else:
            shutil.copy(p, d)
    ch = os.path.join(d, "characterise.py")
    runs = [0]

    def measure(rig, stim, what, *extra):
        runs[0] += 1
        w = newdir(tmp, "tut_run_%d" % runs[0])
        r = lcg(["-q"] + list(extra) + ["-c", os.path.join(d, rig), os.path.join(d, stim)], w)
        if r.returncode != 0:
            return "RUN FAILED: " + r.stderr
        return go([PY, ch, what, h5_of(w)], w).stdout

    def num(out, key):
        import re
        m = re.search(key + r"[^0-9-]*(-?[0-9.]+)", out)
        return float(m.group(1)) if m else float("nan")

    o = measure("rig.ini", "small_step.sg", "rin")
    check("tutorial A: input resistance 76.7 MOhm, time constant under 1 ms", abs(num(o, "input resistance") - 76.7) < 0.2
          and num(o, "time constant") < 1.0, o)
    o = measure("rig.ini", "small_step.sg", "rin", "-P", "big")
    check("tutorial A: the big cell has an input resistance of 17.5 MOhm", abs(num(o, "input resistance") - 17.5) < 0.2, o)
    o = measure("rig.ini", "current_steps_fine.sg", "fi")
    check("tutorial B: repetitive firing starts at 80 pA, a single spike at 50 pA", "first step with a spike: 50 pA" in o
          and "repetitive firing (5 spikes or more): 80 pA" in o, o)
    o = measure("rig.ini", "small_step.sg", "rin", "-P", "ballstick_dend")
    check("tutorial D: 304 MOhm at the tip of the dendrite", abs(num(o, "input resistance") - 303.8) < 1.0, o)
    o = measure("rig_pair.ini", "pair_pulse.sg", "psp")
    check("tutorial E: a weak synapse gives a 2.2 mV PSP", abs(num(o, "P2 rises") - 2.19) < 0.05, o)
    o = measure("rig_pair.ini", "pair_pulse.sg", "psp", "-P", "strong")
    check("tutorial E: a strong synapse makes P2 fire (a rise of 100 mV or more)", num(o, "P2 rises") > 100, o)
    o1 = measure("rig_custom.ini", "small_step.sg", "rin", "-P", "short")
    o2 = measure("rig_custom.ini", "small_step.sg", "rin", "-P", "long")
    check("tutorial F: a longer dendrite lowers the input resistance (75.6 against 65.7 MOhm)",
          abs(num(o1, "input resistance") - 75.6) < 0.3 and abs(num(o2, "input resistance") - 65.7) < 0.3, o1 + o2)
    o = measure("rig_morphology.ini", "small_step.sg", "rin")
    check("tutorial G: the morphology cell has HH in its soma (about 70 MOhm and a time constant of a millisecond, not a passive 400 MOhm)",
          50 < num(o, "input resistance") < 100 and num(o, "time constant") < 3, o)
    o = measure("rig_ih.ini", "sag_step.sg", "sag")
    o2 = measure("rig_ih.ini", "sag_step.sg", "sag", "-P", "noih")
    check("tutorial H: Ih makes a sag (ratio 2.1) and without it there is none (1.00); the .mod file compiled",
          1.9 < num(o, "sag ratio") < 2.3 and abs(num(o2, "sag ratio") - 1.0) < 0.01, o + o2)
    try:
        import matplotlib   # noqa: F401
        w = newdir(tmp, "tut_plot")
        r = lcg(["-q", "-c", os.path.join(d, "rig.ini"), os.path.join(d, "small_step.sg")], w)
        p = go([PY, os.path.join(d, "plot_run.py"), h5_of(w), "0", os.path.join(w, "t.svg")], w)
        check("plot_run.py draws a trial to a file", p.returncode == 0 and os.path.getsize(os.path.join(w, "t.svg")) > 1000, p.stderr)
    except ImportError:
        print("      (plot_run.py not tested: no matplotlib)")


def test_electrodes(tmp):
    """The pipette and the amplifier: resistance, capacitance, bridge balance, neutralisation, compensation, noise."""
    d = newdir(tmp, "electrodes")
    for f in os.listdir(KIT):
        pth = os.path.join(KIT, f)
        if f.endswith(".ini"):
            write(os.path.join(d, f), fix_py(open(pth).read()))
        elif os.path.isdir(pth):
            shutil.copytree(pth, os.path.join(d, f))
        elif not f.endswith(".pyc"):
            shutil.copy(pth, d)
    runs = [0]

    def trace_of(rig, stim, *extra, trial=0):
        runs[0] += 1
        w = newdir(tmp, "el_run_%d" % runs[0])
        r = lcg(["-q"] + list(extra) + ["-c", os.path.join(d, rig), os.path.join(d, stim)], w)
        if r.returncode != 0:
            print("      run failed:", r.stderr)
            return None
        return trace(h5_of(w), trial, "P1")

    ideal = trace_of("rig_electrode.ini", "small_step.sg", "-P", "ideal")
    raw = trace_of("rig_electrode.ini", "small_step.sg", "-P", "raw")
    bridge = trace_of("rig_electrode.ini", "small_step.sg", "-P", "bridge")
    bal = trace_of("rig_electrode.ini", "small_step.sg", "-P", "balanced")
    if ideal is None or raw is None or bridge is None or bal is None:
        check("the electrode profiles run", False)
        return
    a = 7000                                                    # 350 ms: the step (from 100 ms to 400 ms) has settled
    check("cc: the electrode adds its voltage drop: -20 pA through 50 MOhm is exactly -1.000 mV (%.4f)" % (raw[a] - ideal[a]),
          abs((raw[a] - ideal[a]) + 1.0) < 0.005, raw[a] - ideal[a])
    check("cc: bridge balance removes it at steady state (%.4f mV left)" % (bridge[a] - ideal[a]), abs(bridge[a] - ideal[a]) < 0.005)
    onset = range(2000, 2040)                                   # the first 2 ms of the step
    e_bridge = max(abs(bridge[i] - ideal[i]) for i in onset)
    e_bal = max(abs(bal[i] - ideal[i]) for i in onset)
    check("cc: neutralising the capacitance too makes the transient after the step smaller (%.2f against %.2f mV)" % (e_bal, e_bridge),
          e_bal < 0.5 * e_bridge, (e_bal, e_bridge))
    check("cc: the electrode capacitance slows the raw recording (it is not the ideal one in the first millisecond)",
          max(abs(raw[i] - ideal[i]) for i in onset) > 0.3)
    # the cell sees the injected current: it is the same cell, so the settled potential of the cell is unchanged
    check("cc: the cell itself is unchanged: bridge-balanced equals ideal at rest too", abs(bridge[1000] - ideal[1000]) < 0.005)
    # an ideal electrode written out in the file equals no electrode keys at all (the fast path)
    plain = trace_of("rig.ini", "small_step.sg")
    check("cc: electrode keys at zero reproduce the plain cell exactly", max(abs(p - q) for p, q in zip(plain, ideal)) < 1e-9)
    n1 = trace_of("rig_electrode.ini", "small_step.sg", "-P", "noisy")
    n2 = trace_of("rig_electrode.ini", "small_step.sg", "-P", "noisy")
    import statistics
    sd = statistics.pstdev([n1[i] - raw[i] for i in range(1500)])
    check("noise: 0.3 mV rms is recorded as %.3f mV rms" % sd, 0.25 < sd < 0.35, sd)
    check("noise: the same run gives the same noise (reproducible)", n1 is not None and max(abs(p - q) for p, q in zip(n1, n2)) < 1e-12)
    # voltage clamp
    ivc = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "ideal", trial=3)
    rvc = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_raw", trial=3)
    cvc = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_comp", trial=3)
    if ivc is None or rvc is None or cvc is None:
        check("the voltage-clamp electrode profiles run", False)
        return
    end = 1190                                                  # end of the 0 mV step (50 + 30 ms = 80 ms = sample 1600; 1190 is 59.5 ms)
    peak = lambda x: min(x[1005:1200])                          # noqa: E731
    check("vc: a 10 MOhm series resistance cuts the sodium current the clamp can hold (%.0f against %.0f pA)" % (peak(rvc), peak(ivc)),
          peak(rvc) > 0.5 * peak(ivc), (peak(rvc), peak(ivc)))
    check("vc: rs_compensation 70 percent restores much of it (%.0f pA)" % peak(cvc), abs(peak(cvc)) > 1.5 * abs(peak(rvc)),
          (peak(rvc), peak(cvc)))
    cpc = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_cp", trial=3)
    check("vc: the pipette transient is Cp*dV/T = 5 pF * 70 mV / 50 us = 7000 pA in the first sample of the step, and "
          "cp_compensation removes exactly that (%.3f pA)" % (rvc[1000] - cpc[1000]), abs(rvc[1000] - cpc[1000] - 7000) < 1e-6,
          (rvc[1000], cpc[1000]))

    # the seal, the liquid junction potential, the amplifier's bandwidth
    seal = trace_of("rig_electrode.ini", "small_step.sg", "-P", "seal1g")
    junction = trace_of("rig_electrode.ini", "small_step.sg", "-P", "junction")
    bw = trace_of("rig_electrode.ini", "small_step.sg", "-P", "bandwidth")
    check("seal: a 1 GOhm seal leaks to the bath (0 mV) and depolarises the cell (%.2f mV at rest)" % (seal[1000] - ideal[1000]),
          2.0 < seal[1000] - ideal[1000] < 5.0, seal[1000] - ideal[1000])
    check("junction: the recording reads exactly 10 mV more positive than the cell, all the time",
          max(abs((junction[i] - ideal[i]) - 10.0) for i in range(len(ideal))) < 1e-9)
    check("bandwidth: a 2 kHz filter leaves the steady state alone and rounds the onset",
          abs(bw[a] - ideal[a]) < 0.005 and max(abs(bw[i] - ideal[i]) for i in range(2000, 2040)) > 0.1)
    w1 = newdir(tmp, "el_ljp_a")
    w2 = newdir(tmp, "el_ljp_b")
    stim = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=mV { 50ms dc(%d) }\n"
    ra = lcg(["-q", "-m", "vc", "-P", "junction", "-c", os.path.join(d, "rig_electrode.ini"), "-e", stim % -60], w1)
    rb = lcg(["-q", "-m", "vc", "-P", "ideal", "-c", os.path.join(d, "rig_electrode.ini"), "-e", stim % -70], w2)
    ia, ib = trace(h5_of(w1), 0, "P1"), trace(h5_of(w2), 0, "P1")
    check("junction: in voltage clamp a command of -60 mV with a 10 mV junction potential holds the cell where -70 mV does without",
          ra.returncode == 0 and rb.returncode == 0 and max(abs(x - y) for x, y in zip(ia, ib)) < 1e-6, ra.stderr + rb.stderr)
    vraw = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_raw")
    vseal = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_seal")
    vbw = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_bandwidth")
    check("seal: in voltage clamp a 500 MOhm seal adds about -70 mV / 500 MOhm = -140 pA to the holding current (%.0f pA)" % (vseal[500] - vraw[500]),
          abs((vseal[500] - vraw[500]) + 140) < 30, vseal[500] - vraw[500])
    vraw3 = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_raw", trial=3)
    vbw3 = trace_of("rig_electrode.ini", "vc_steps.sg", "-m", "vc", "-P", "vc_bandwidth", trial=3)
    q_raw, q_bw = sum(vraw3[1000:2550]), sum(vbw3[1000:2550])            # the charge from the step to rest again (pA * samples)
    pk_raw, pk_bw = max(vraw3[1000:1040]), max(vbw3[1000:1040])
    check("bandwidth: in voltage clamp the 2 kHz filter spreads the capacitive spike (peak %.0f against %.0f pA) but keeps its "
          "charge (%.2f%% apart): a filter delays and smooths, it removes nothing" % (pk_bw, pk_raw, 100 * abs(q_bw / q_raw - 1)),
          pk_bw < 0.5 * pk_raw and abs(q_bw / q_raw - 1) < 0.01, (pk_bw, pk_raw, q_bw, q_raw))
    import math as _m
    sys.path.insert(0, os.path.join(ROOT, "neuron"))
    import lcg2_neuron as _L

    def gain(fc, mult):
        dt = min(0.025, _L.Bessel4.MAX_FC_DT / fc)                  # the step the adapter uses for this corner
        f = _L.Bessel4(fc)
        f.set_dt(dt)
        f.reset(0.0)
        n, pk = int(60 / dt), 0.0
        for i in range(n):
            y = f.step(_m.sin(2 * _m.pi * fc * mult * i * dt))
            if i > n // 2:
                pk = max(pk, abs(y))
        return pk
    g1, g2, g3 = gain(2.0, 0.1), gain(2.0, 1.0), gain(2.0, 2.0)
    check("bandwidth: the filter is a 4-pole Bessel: gain %.4f at 0.1 fc, %.4f at fc (-3 dB is 0.7071), %.4f at 2 fc (analogue 0.2137)"
          % (g1, g2, g3), g1 > 0.99 and abs(g2 - 0.7071) < 0.002 and abs(g3 - 0.2137) < 0.002, (g1, g2, g3))

    # active electrode compensation: noise injected through a bad electrode, the electrode learned and subtracted
    w = newdir(tmp, "el_aec")
    r = lcg(["-q", "-s", "5", "-c", os.path.join(d, "rig_aec.ini"), os.path.join(d, "aec_noise.sg")], w)
    out = go([PY, os.path.join(d, "aec.py"), h5_of(w)], w).stdout
    import re as _re
    m = _re.search(r"([0-9.]+) mV rms before, ([0-9.]+) mV rms after", out)
    check("aec: the compensated recording is at least 3 times closer to the true membrane potential (a second, ideal electrode "
          "on the same cell) than the raw one", r.returncode == 0 and m is not None and float(m.group(1)) > 3 * float(m.group(2)), out)
    with h5py.File(h5_of(w), "r") as f:
        both = f["trial_0000/ai/P1"][:], f["trial_0000/ai/V0"][:]
    check("same_as: the second electrode records the same cell (it differs from the pipette P1 by its electrode only: "
          "an ideal electrode and the electrode-free run agree, P1 does not)", abs(both[0] - both[1]).max() > 2.0
          and abs(both[1]).max() < 200)

    def bad(label, extra, expect):
        w = newdir(tmp, "el_bad_" + re.sub(r"\W", "_", label))
        ini = open(os.path.join(d, "rig_electrode.ini")).read()
        if extra.startswith("electrode_resistance"):
            ini = ini.replace("electrode_resistance  = 50", extra, 1)
        else:
            ini = ini.replace("recording_noise       = 0       ; mV (cc) or pA (vc), rms", "recording_noise = 0\n" + extra)
        write(os.path.join(d, "bad_el.ini"), ini)
        r = lcg(["-n", "-c", os.path.join(d, "bad_el.ini"), os.path.join(d, "small_step.sg")], w)
        check("%s -> %s" % (label, expect), r.returncode == 1 and expect in r.stderr, "exit %d\n%s" % (r.returncode, r.stderr))
    bad("neutralisation beyond the electrode capacitance", "cap_neutralization = 3.5", "must stay below electrode_capacitance")
    bad("full series-resistance compensation", "rs_compensation = 100", "must be below 100 percent")
    bad("a negative resistance", "bridge_balance = -5", "must not be negative")
    bad("an electrode too small for the time step", "electrode_resistance = 0.5", "too small for sim_dt")
    bad("an unknown unit", "bridge_balance = 50 ohms", "unknown unit")
    bad("a seal too leaky for the time step", "seal_resistance = 0.0001", "seal_resistance")
    bad("a bandwidth wider than any amplifier's", "amplifier_bandwidth = 200", "at most 100 kHz")
    bad("a compensation lag below a microsecond", "rs_lag = 0.5us", "at least 1 us")
    bad("same_as naming a cell that is not there", "[cell W]\nsame_as = Nope", "must name a [cell] written EARLIER")


def test_physics(tmp):
    """The electrode and amplifier model against references derived independently of the adapter: exact solutions
    of the linear circuits, the analogue Bessel filter, the d_lambda rule (the audit of 2026-11-18)."""
    import cmath
    import math
    import numpy as np
    runs = [0]

    def run(cells, stim, rig="", args=(), trial=0):
        runs[0] += 1
        w = newdir(tmp, "phys_%d" % runs[0])
        write_ini(w, cells="[cell P1]\nmodel = hh_soma\n" + cells, rig=rig)
        write(os.path.join(w, "s.sg"), stim)
        r = lcg(["-q", "-s", "3"] + list(args) + ["-c", "rig.ini", "s.sg"], w)
        if r.returncode != 0:
            print("      run failed:", r.stderr)
            return None
        with h5py.File(h5_of(w), "r") as f:
            g = f["trial_%04d/ai" % trial]
            return {k: g[k][:] for k in g}

    step = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA {\n 20ms dc(0)\n 30ms dc(300)\n 20ms dc(0)\n}\n"
    passive = "gnabar = 0\ngkbar = 0\nel = -80\n"
    # a resistive electrode, balanced: the recording is the ideal one, sample for sample (no lag of one step)
    for dt in ("25us", "50us"):
        ideal = run("", step, "sim_dt = %s" % dt)
        br = run("electrode_resistance = 50\nbridge_balance = 50\n", step, "sim_dt = %s" % dt)
        e = abs(br["P1"] - ideal["P1"]).max() if ideal and br else 1
        check("cc: Re = Rb = 50 MOhm, no capacitance, equals the ideal recording at sim_dt %s (%.1e mV)" % (dt, e), e < 1e-9, e)
    # the pipette starts at rest on the cell as initialised (v_init), not at -65 mV
    t = run(passive + "electrode_resistance = 50\nelectrode_capacitance = 3\nbridge_balance = 50\n", step, "sim_v_init = -80")
    check("cc: with Re and Ce the first samples are the cell at v_init = -80 mV (%s)" % (t and list(np.round(t["P1"][:3], 6))),
          t is not None and abs(t["P1"][:3] + 80).max() < 1e-6)
    # neutralisation: Re*(Ce - Cn) = 25 us, the node solved exactly: the result does not depend on the step
    small = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA {\n 20ms dc(0)\n 20ms dc(-20)\n}\n"
    errs = []
    for dt in ("25us", "1us"):
        a = run("", small, "sim_dt = %s" % dt)
        b = run("electrode_resistance = 50\nelectrode_capacitance = 3\nbridge_balance = 50\ncap_neutralization = 2.5\n",
                small, "sim_dt = %s" % dt)
        errs.append(abs(b["P1"][400:440] - a["P1"][400:440]).max() if a and b else 99)
    check("cc: the neutralised transient is the same at sim_dt 25 and 1 us (%.4f and %.4f mV)" % tuple(errs),
          abs(errs[0] - errs[1]) < 0.005, errs)
    # a pipette that only records (same_as) applies its junction potential on any path
    v0 = "[cell V0]\nsame_as = P1\nliquid_junction_potential = 10\n[in V0]\nline = ai1\ngain_cc = 100\nunit_cc = mV\n"
    r = run(v0, step)
    e = abs(r["V0"] - r["P1"] - 10).max() if r else 1
    check("a recording-only pipette with a 10 mV junction potential reads exactly 10 mV more (%.1e)" % e, e < 1e-9)

    # the filter at the step it is given against the analogue 4-pole Bessel, 105/(s^4 + 10 s^3 + 45 s^2 + 105 s + 105)
    sys.path.insert(0, os.path.join(ROOT, "neuron"))
    import lcg2_neuron as L
    lo, hi = 1.0, 3.0
    for _ in range(80):
        m = (lo + hi) / 2
        lo, hi = (m, hi) if abs(105 / ((1j * m) ** 4 + 10 * (1j * m) ** 3 + 45 * (1j * m) ** 2 + 105j * m + 105)) > 2 ** -0.5 else (lo, m)

    def analog(f, fc):
        s = 1j * lo * f / fc
        return 105 / (s ** 4 + 10 * s ** 3 + 45 * s ** 2 + 105 * s + 105)
    for fc in (2.0, 10.0):
        dt = min(0.025, L.Bessel4.MAX_FC_DT / fc)
        fl = L.Bessel4(fc)
        fl.set_dt(dt)

        def digital(f):
            z, H = cmath.exp(2j * math.pi * f * dt), 1
            for b0, b1, a1, a2 in [x[:4] for x in fl.sec]:
                H *= (b0 + b1 / z + b0 / z ** 2) / (1 + a1 / z + a2 / z ** 2)
            return H
        worst = max(abs(abs(digital(k * fc / 50)) / abs(analog(k * fc / 50, fc)) - 1) for k in range(1, 101))
        check("bandwidth: %g kHz at the step used (%g us) is within 0.3%% of the analogue Bessel up to twice the corner "
              "(%.3f%%; gain %.4f at 2 fc, analogue %.4f)" % (fc, dt * 1000, 100 * worst, abs(digital(2 * fc)), abs(analog(2 * fc, fc))),
              worst < 0.003)
    # the noise passes the filter: rms = sd * sqrt(2 * ENBW * T), whatever sim_dt
    fs = np.linspace(0, 400.0, 400001)
    enbw = float(np.sum(abs(analog(fs, 2.0)) ** 2) * (fs[1] - fs[0]))
    want = 0.3 * math.sqrt(2 * enbw * 0.05)
    quiet = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA {\n 2000ms dc(0)\n}\n"
    got = []
    for dt in ("25us", "10us"):
        a = run(passive + "recording_noise = 0.3\namplifier_bandwidth = 2\n", quiet, "sim_dt = %s\nsim_v_init = -80" % dt)
        got.append(float((a["P1"][200:] + 80).std()) if a else 0)
    check("noise: 0.3 mV rms through a 2 kHz filter is %.4f mV rms, as sd*sqrt(2*ENBW*T) = %.4f (sim_dt 25 and 10 us)"
          % (got[0], want), all(abs(g - want) < 0.05 * want for g in got), got)
    a = run(passive + "recording_noise = 0.3\n", quiet, "sim_v_init = -80")
    check("noise: without the filter it is 0.3 mV rms per sample", a is not None and abs((a["P1"] + 80).std() - 0.3) < 0.01)

    # Rs compensation with a lag, on a passive cell, against the exact solution of the linear circuit
    vstep = "sg 2 stimulus\nrate 100kHz\nchannel P1 unit=mV {\n 5ms dc(-80)\n 5ms dc(-70)\n}\n"
    t = run(passive + "electrode_resistance = 10\nrs_compensation = 90\nrs_lag = 20us\n", vstep, "sim_v_init = -80", ["-m", "vc"])
    cm = math.pi * 20 * 20 * 0.01                                  # pF: the 20 um hh_soma, 1 uF/cm2
    rm = 1.0 / (0.0003 * math.pi * 20 * 20 * 1e-8) * 1e-6          # MOhm: gl = 0.0003 S/cm2
    rs, al, lag, el, vc = 10.0, 0.9, 0.020, -80.0, -70.0
    A = np.array([[(-1 / rs - 1 / rm) / cm * 1000, 1 / rs / cm * 1000], [-al / lag, (al - 1) / lag]])
    bv = np.array([(vc / rs + el / rm) / cm * 1000, al * vc / lag])
    xs = np.linalg.solve(A, -bv)
    lam, V = np.linalg.eig(A)
    c = np.linalg.solve(V, np.array([-80.0, 0.0]) - xs)
    X = np.real(np.array([xs + V @ (c * np.exp(lam * k * 0.01)) for k in range(1, 501)]))
    exact = (vc + X[:, 1] - X[:, 0]) / rs * 1000
    e = abs(t["P1"][500:1000] - exact).max() if t else 1e9
    check("vc: 90%% compensation with a 20 us lag rings as the exact solution of the circuit does (within %.1f pA of a "
          "%.0f pA peak; the rest is NEURON's own first-order step)" % (e, abs(exact).max()), e < 0.015 * abs(exact).max())
    # with no lag the compensation is ideal: the clamp is (1 - alpha) * Rs, here 1 MOhm, no ringing
    t0 = run(passive + "electrode_resistance = 10\nrs_compensation = 90\nrs_lag = 0\n", vstep, "sim_v_init = -80", ["-m", "vc"])
    t1 = run(passive + "electrode_resistance = 1\n", vstep, "sim_v_init = -80", ["-m", "vc"])
    e = abs(t0["P1"] - t1["P1"]).max() if t0 and t1 else 1
    check("vc: rs_lag = 0 is ideal compensation: 90%% of 10 MOhm is a 1 MOhm clamp exactly (%.1e pA)" % e, e < 1e-6)

    # d_lambda counts with the Ra and cm the biophysics file sets
    w = newdir(tmp, "phys_dlambda")
    shutil.copy(EX("student_kit/tiny.swc"), w)
    write(os.path.join(w, "bio.py"), "def biophysics(cell):\n    for sec in cell.all:\n        sec.Ra, sec.cm = 300.0, 2.0\n"
          "        sec.insert('pas')\n")
    write(os.path.join(w, "c.ini"), "[cell P1]\nmorphology = tiny.swc\nbiophysics = bio.py\n")
    out = go([PY, "-I", os.path.join(ROOT, "neuron", "lcg2_neuron.py"), "--check", "c.ini"], w).stdout
    m = re.search(r"(\d+) section\(s\),\s+(\d+) segment\(s\)", out)
    check("d_lambda counts segments with the final Ra = 300 and cm = 2 (13 segments, not 7)", m is not None and m.group(2) == "13", out)


PATCH_RIG = """[rig]
device = neuron
sim_python = %(py)s
mode = vc
sim_dt = 5us
[out P1]
line = ao0
gain_vc = 20
unit_vc = mV
gain_cc = 400
unit_cc = pA
[in P1]
line = ai0
gain_vc = 1000
unit_vc = pA
gain_cc = 100
unit_cc = mV
membrane_test = yes
[cell P1]
model = hh_soma
gnabar = 0
gkbar = 0
pipette_resistance = 5
pipette_capacitance = 4
cp_compensation = 4
access_resistance = 12
seal_target = 2
seal_failure = 0
break_in_success = 1
liquid_junction_potential = 12
patch_seed = 51
%(cell)s
"""

PULSES = """sg 2 protocol
sweep hold = [%s]
repeat %d
order grouped
period 100ms
stimulus {
    rate 100kHz
    channel P1 unit=mV rest=$hold {
        5ms  dc($hold)
        10ms dc($(hold + 5))
        5ms  dc($hold)
    }
}
"""


def test_patch(tmp):
    """Patching (tutorial L): the bath, the seal, cell-attached and whole-cell against the circuit's own values, the
    membrane test, the junction potential and the pipette offset, the action currents of a cell-attached patch."""
    import math
    import numpy as np
    runs = [0]

    def run(cell, stim, args=(), rig=PATCH_RIG):
        runs[0] += 1
        w = newdir(tmp, "patch_%d" % runs[0])
        write(os.path.join(w, "rig.ini"), rig % dict(py=PY, cell=cell))
        write(os.path.join(w, "s.sg"), stim)
        r = lcg(["-s", "1"] + list(args) + ["-c", "rig.ini", "s.sg"], w)
        if r.returncode != 0:
            print("      run failed:", r.stderr[-400:])
            return None, None, r
        net = json.load(open(glob.glob(os.path.join(folder_of(w), "network.json"))[0]))
        return h5py.File(h5_of(w), "r"), net["cells"][0].get("patch_truth"), r

    def mt(f, k, key):
        return float(f["trial_%04d/ai/P1" % k].attrs["membrane_test_" + key])

    rmem, el, cm = 1.0 / (0.0003 * math.pi * 400 * 1e-8) * 1e-6, -54.3, math.pi * 400 * 0.01   # the passive soma
    rp = 5.0
    par = lambda a, b: a * b / (a + b)                       # noqa: E731
    # 1. the bath, with the offset nulled: R is the pipette, and no current flows at 0 mV
    f, tr, r = run("", PULSES % ("0", 3))
    check("patch, bath: the seal test reads the pipette, R = %.6f MOhm (5)" % mt(f, 0, "R_MOhm"), abs(mt(f, 0, "R_MOhm") - rp) < 1e-6)
    check("patch, bath: with the offset nulled no current flows at 0 mV (%.2e pA), although LJP is 12 mV"
          % mt(f, 0, "Ihold_pA"), abs(mt(f, 0, "Ihold_pA")) < 1e-6)
    check("patch, bath: the seal test prints the resistance after each trial", "R 5.00 MOhm, I hold 0.0 pA" in r.stderr, r.stderr[-300:])
    f, tr, r = run("pipette_offset = none\n", PULSES % ("0", 1))
    want = 12.0 / (rp + 1e-6) * 1000.0                         # the open tip is 1 Ohm (OPEN_TIP)
    check("patch, bath: without nulling, the junction potential drives LJP / Rp = %.4f pA at 0 mV (%.4f)" % (want, mt(f, 0, "Ihold_pA")),
          abs(mt(f, 0, "Ihold_pA") / want - 1) < 1e-9)
    # 2. cell-attached: the seal (drawn from the seed) in parallel with the patch in series with the cell.  The patch charges
    # the cell (Cm with Rpatch || Rm, about 3 ms): the conductance of patch + cell after a step is
    # (1 + (Rm/Rpatch) exp(-t/tau)) / (Rpatch + Rm); the membrane test reads its mean over 7.5 to 10 ms of the step.
    f, tr, r = run("patch_start = cell_attached\n", PULSES % ("-70", 2))
    taup = cm * 1e-3 * par(10000.0, rmem)
    gpatch = (1 + rmem / 10000.0 * taup / 2.5 * (math.exp(-7.5 / taup) - math.exp(-10.0 / taup))) / (10000.0 + rmem)
    want = rp + 1.0 / (1.0 / tr["seal_MOhm"] + gpatch)
    check("patch, cell-attached: R = Rp + seal || (patch + cell) = %.3f MOhm (%.3f)" % (want, mt(f, 1, "R_MOhm")),
          abs(mt(f, 1, "R_MOhm") / want - 1) < 2e-3)
    check("patch, cell-attached: no cell is fitted (the patch is in series)", "membrane_test_Cm_pF" not in f["trial_0001/ai/P1"].attrs)
    # 3. whole-cell: R exact; Ra = Rp + seal || Rx at the step; tau = Cm (Rm || (Rx + Rp || seal)) after NEURON's step
    f, tr, r = run("patch_start = whole_cell\n", PULSES % ("-70", 2))
    rx, sl = tr["access_MOhm"] - rp, tr["seal_MOhm"]
    want_r, want_ra = rp + par(sl, rx + rmem), rp + par(sl, rx)
    tau = cm * 1e-3 * par(rmem, rx + par(rp, sl))              # ms
    tau_be = 0.005 / math.log(1 + 0.005 / tau)                 # what backward Euler at 5 us makes of it
    check("patch, whole-cell: R = Rp + seal || (Rx + Rm) = %.3f MOhm (%.3f)" % (want_r, mt(f, 1, "R_MOhm")), abs(mt(f, 1, "R_MOhm") / want_r - 1) < 1e-3)
    check("patch, whole-cell: Ra = %.3f MOhm (%.3f), within 1 %%" % (want_ra, mt(f, 1, "Ra_MOhm")), abs(mt(f, 1, "Ra_MOhm") / want_ra - 1) < 0.01)
    check("patch, whole-cell: tau = %.1f us, as the circuit at NEURON's step (%.1f)" % (tau_be * 1000, mt(f, 1, "tau_ms") * 1000),
          abs(mt(f, 1, "tau_ms") / tau_be - 1) < 0.01)
    check("patch, whole-cell: Cm %.2f pF of a %.2f pF soma (within 3 %%: the step and the seal's place)" % (mt(f, 1, "Cm_pF"), cm),
          abs(mt(f, 1, "Cm_pF") / cm - 1) < 0.03)
    check("patch, whole-cell: the readout prints Ra, Rm and Cm", "Ra 1" in r.stderr and "Cm 1" in r.stderr, r.stderr[-300:])
    # the junction-potential error after nulling: in current clamp the reading is the tip plus LJP
    f2, tr2, r2 = run("patch_start = whole_cell\n", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 300ms dc(0) }\n", ["-m", "cc"])
    v = f2["trial_0000/ai/P1"][:]
    want_v = el * sl / (sl + rx + rmem) + 12.0
    check("patch, whole-cell, current clamp: the cell rests at %.4f mV through the seal and reads LJP more (%.4f)" % (want_v, v[-1]),
          abs(v[-1] - want_v) < 1e-3)
    check("patch: the seed is a cell: the same draw in current and voltage clamp", tr2 == tr)
    check("patch: the membrane test needs voltage clamp and says so", "no membrane test: it needs voltage clamp" in r2.stderr, r2.stderr[-300:])
    f3, tr3, r3 = run("patch_start = whole_cell\n", PULSES % ("-70", 1), rig=PATCH_RIG.replace("patch_seed = 51", "patch_seed = 52"))
    check("patch: another seed, another cell", tr3 is not None and tr3 != tr)
    # 4. the timeline in one run: the bath (0 mV), the contact, the seal growing, -70 mV from 3.5 s, the break-in at 6 s
    f, tr, r = run("patch_contact = 0.3s\npatch_suction = 0.5s\npatch_break_in = 6s\n", PULSES % ("0, -70", 35))
    rs = [mt(f, k, "R_MOhm") for k in range(70)]
    check("patch, timeline: the bath, then the contact raises R a little (%.2f -> %.2f MOhm)" % (rs[1], rs[4]),
          abs(rs[1] - rp) < 1e-6 and rp < rs[4] < 1.5 * rp)
    check("patch, timeline: the seal grows (%.0f MOhm at 3.4 s, never falling while it forms) to gigaohms (%.0f MOhm at 5.9 s)"
          % (rs[34], rs[59]), rs[34] > 500 and rs[59] > 1500 and all(b >= a - 1e-6 for a, b in zip(rs[5:34], rs[6:35])))
    check("patch, timeline: after the break-in the cell is there (R %.1f MOhm, Cm %.2f pF)" % (rs[65], mt(f, 65, "Cm_pF")),
          abs(rs[65] / want_r - 1) < 0.01 and abs(mt(f, 65, "Cm_pF") / cm - 1) < 0.03)
    # an event inside a pause between trials happens there: the trial of 6.0 s (to 6.02 s) is cell-attached, the next one whole-cell
    f, tr, r = run("patch_contact = 0.3s\npatch_suction = 0.5s\npatch_break_in = 6.05s\n", PULSES % ("0, -70", 35))
    a60, a61 = f["trial_0060/ai/P1"].attrs, f["trial_0061/ai/P1"].attrs
    check("patch, timeline: a break-in at 6.05 s, in the pause, shows in the next trial (R %.0f then %.1f MOhm)"
          % (a60["membrane_test_R_MOhm"], a61["membrane_test_R_MOhm"]),
          "membrane_test_Cm_pF" not in a60 and abs(a61["membrane_test_R_MOhm"] / want_r - 1) < 0.01)
    # what was drawn is never shown: not by -n, not by the run (only network.json has it)
    w = newdir(tmp, "patch_secret")
    write(os.path.join(w, "rig.ini"), PATCH_RIG % dict(py=PY, cell="patch_contact = 0.3s\npatch_suction = 0.5s\npatch_break_in = 6s\n"))
    write(os.path.join(w, "s.sg"), PULSES % ("0", 2))
    rn = lcg(["-n", "-c", "rig.ini", "s.sg"], w)
    shown = rn.stdout + rn.stderr + r.stdout + r.stderr
    secrets = ["%.0f" % tr["seal_MOhm"], "%.2f GOhm" % (tr["seal_MOhm"] / 1000), "%.4f" % tr["access_MOhm"],
               "%.3f" % tr["access_MOhm"], "%.0f" % tr["seal_tau_ms"], "%.3f" % tr["contact_increase"]]
    check("patch: -n shows what was set (pipette 5 MOhm, the timeline) and neither it nor the run shows what was drawn (%s)"
          % ", ".join(secrets), "patch: pipette 5 MOhm" in rn.stdout and not any(x in shown for x in secrets), rn.stdout[-400:])

    # 5. action currents through a cell-attached patch, in-process (a voltage-clamp run has no current-clamp electrode to make
    # the cell fire): the current the amplifier records is minus the patch current, Cpatch dVm/dt + (Vm - T)/Rpatch, times
    # the share that goes to the amplifier rather than through the seal
    sys.path.insert(0, os.path.join(ROOT, "neuron"))
    import lcg2_neuron as L
    H = L.load_neuron()
    prep = ("[cell P9]\nmodel = hh_soma\npipette_resistance = 5\npatch_seed = 51\nseal_failure = 0\npatch_start = cell_attached\n"
            "patch_capacitance = 0.05\npatch_resistance = 10\n")
    net = L.Network(H, prep, ".", [dict(name="P9", dim="voltage")], [dict(name="P9", dim="current")],
                    dict(dt_ms=0.005, celsius=6.3, v_init=-65.0, settle_ms=0.0))
    ic = H.IClamp(net.cells["P9"]["seg"])
    ic.delay, ic.dur, ic.amp = 10.0, 40.0, 0.2
    net.start_trial(100000, 1)
    el9 = net.out_el[0]
    ip, vm, tt = [], [], []
    for k in range(6000):
        i, = net.block([array.array("d", [0.0])], 1)
        ip.append(i[0])
        vm.append(net.cells["P9"]["seg"].v)
        tt.append(el9.y.x[1])
    ip, vm, tt = np.array(ip), np.array(vm), np.array(tt)
    seal = el9.ps["truth"]["seal_MOhm"]
    pred = -(0.05e-3 * np.gradient(vm, 0.01) + (vm - tt) / 10000.0) * 1e3 * seal / (seal + 5.0)
    nsp = int(sum(1 for k in range(1, len(vm)) if vm[k - 1] < 0 <= vm[k]))
    e = np.abs(ip[1000:5000] - pred[1000:5000]).max() / np.abs(pred[1000:5000]).max()
    check("patch, cell-attached: the cell fires (%d spikes) and each spike is a biphasic current of a few pA through the patch "
          "(%.1f to %.1f pA), Cpatch dV/dt + (V - T)/Rpatch to %.1f %%" % (nsp, ip[1000:5000].min(), ip[1000:5000].max(), 100 * e),
          nsp >= 3 and 2 < -ip[1000:5000].min() < 50 and e < 0.05, (nsp, e))

    # mistakes
    def bad(label, cell, expect, rig=PATCH_RIG):
        runs[0] += 1
        w = newdir(tmp, "patch_bad_%d" % runs[0])
        write(os.path.join(w, "rig.ini"), rig % dict(py=PY, cell=cell))
        write(os.path.join(w, "s.sg"), PULSES % ("0", 1))
        r = lcg(["-n", "-c", "rig.ini", "s.sg"], w)
        check("patch, refused: %s" % label, r.returncode == 1 and expect in r.stderr, r.stderr[-300:])
    bad("a suction before any contact", "patch_suction = 2s\n", "patch_suction needs a patch_contact before it")
    bad("a break-in without a seal", "patch_contact = 1s\npatch_break_in = 3s\n", "patch_break_in needs a patch_suction before it")
    bad("an access smaller than the pipette", "", "must be larger than pipette_resistance",
        PATCH_RIG.replace("access_resistance = 12", "access_resistance = 4"))
    bad("the plain electrode's keys", "electrode_resistance = 10\n", "a patching pipette (pipette_resistance) has its own keys")
    bad("a break-in when already in whole-cell", "patch_start = whole_cell\npatch_break_in = 2s\n", "there is no patch to break")
    bad("times that go back", "patch_contact = 1s\npatch_suction = 2s\npatch_break_in = 5s, 4s\n", "the times must increase")
    bad("a patch key without pipette_resistance", "", "give pipette_resistance too", PATCH_RIG.replace("pipette_resistance = 5\n", ""))


def test_tutorial_l(tmp):
    """The lines and numbers tutorial L (docs/16) quotes, from the kit's own files."""
    import numpy as np
    d = newdir(tmp, "tut_l")
    for f in ("rig_patch.ini", "seal_test.sg", "cell_attached.sg", "small_step.sg"):
        p = os.path.join(KIT, f)
        write(os.path.join(d, f), fix_py(open(p).read()) if f.endswith(".ini") else open(p).read())
    runs = [0]

    def run(*args):
        runs[0] += 1
        w = newdir(tmp, "tut_l_%d" % runs[0])
        r = lcg(list(args[:-1]) + ["-c", os.path.join(d, "rig_patch.ini"), os.path.join(d, args[-1])], w)
        return r, (h5_of(w) if r.returncode == 0 else None)

    r, h5 = run("-m", "vc", "seal_test.sg")
    for line in ("trial 0000  P1  R 5.00 MOhm, I hold 0.0 pA", "trial 0024  P1  R 6.13 MOhm, I hold 1.2 pA",
                 "trial 0040  P1  R 65.09 MOhm, I hold 5.9 pA", "trial 0060  P1  R 886.72 MOhm, I hold 6.4 pA",
                 "trial 0099  P1  R 1.75 GOhm, I hold 6.4 pA", "trial 0100  P1  R 1.78 GOhm, I hold -33.4 pA",
                 "trial 0159  P1  R 1.82 GOhm, I hold -32.1 pA",
                 "trial 0161  P1  R 242.84 MOhm: Ra 10.98 MOhm, Rm 231.9 MOhm, Cm 12.87 pF (tau 135 us), I hold -136.8 pA"):
        check("tutorial L prints: %s" % line, line in r.stderr, r.stderr[-500:])
    r, h5 = run("-m", "vc", "-P", "other_cell", "seal_test.sg")
    check("tutorial L: a bad seal never passes 108 MOhm; in the cell -632 pA at -70 mV and Cm 14.1 pF",
          "trial 0099  P1  R 107.92 MOhm" in r.stderr and "I hold -631.8 pA" in r.stderr and "Cm 14.1" in r.stderr, r.stderr[-500:])
    r, h5 = run("-m", "vc", "-P", "no_offset", "seal_test.sg")
    check("tutorial L: without the offset nulled the bath shows 2400 pA at 0 mV", "trial 0000  P1  R 5.00 MOhm, I hold 2400.0 pA" in r.stderr,
          r.stderr[-300:])
    r, h5 = run("-m", "cc", "-P", "whole_cell", "small_step.sg")
    v = trace(h5, 0, "P1") if h5 else np.zeros(8000)
    check("tutorial L: in current clamp the same cell reads -51.1 mV (really -63.1 mV)", abs(v[1000:2000].mean() + 51.1) < 0.05, v[1000:2000].mean())
    r, h5 = run("-m", "vc", "-P", "firing", "cell_attached.sg")
    i = trace(h5, 0, "P1") if h5 else np.zeros(10)
    b = np.median(i)
    n = int(np.sum((i[1:] < b - 5) & (i[:-1] >= b - 5)))
    late = i[len(i) // 2:].min()
    check("tutorial L: cell-attached, 31 action currents in 0.5 s, down to -11.5 pA (the first, from rest, -16 pA) from a baseline "
          "of about 6 pA (%d, %.1f, %.1f, %.1f)" % (n, late, i.min(), b),
          n == 31 and abs(late + 11.5) < 0.2 and abs(i.min() + 16.1) < 0.2 and abs(b - 6.3) < 0.3)


def test_live(tmp):
    """Live patching (-L): keys in a terminal act on the pipette in real time; q ends the run normally; the session can
    be replayed as a scripted timeline and gives the same resistance at the same simulated times."""
    import pty
    import select
    import numpy as np
    d = newdir(tmp, "live")
    for f in ("rig_patch.ini", "seal_test_live.sg"):
        p = os.path.join(KIT, f)
        write(os.path.join(d, f), fix_py(open(p).read()) if f.endswith(".ini") else open(p).read())
    w = newdir(tmp, "live_run")
    # keys at times after the run has started (the first trial's line): a suction before touching does nothing
    keys = [(0.3, b"s"), (0.5, b"c"), (1.0, b"s"), (4.5, b"h"), (5.5, b"b"), (6.1, b"b"), (6.7, b"b"), (8.0, b"q")]
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(w)
        os.execv(LCG2, [LCG2, "-L", "-s", "1", "-m", "vc", "-P", "live", "-c", os.path.join(d, "rig_patch.ini"),
                        os.path.join(d, "seal_test_live.sg")])
    out, t0, started = b"", time.time(), None
    while time.time() - t0 < 120:
        if started is None and b"live: c touch" in out:
            started = time.time()
        while started is not None and keys and time.time() - started >= keys[0][0]:
            os.write(fd, keys.pop(0)[1])
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                b = os.read(fd, 4096)
            except OSError:
                break
            if not b:
                break
            out += b
    else:
        os.kill(pid, signal.SIGKILL)
    _, st = os.waitpid(pid, 0)
    text = out.decode(errors="replace").replace("\x1b[K", "")
    check("live: q ends the run normally (exit status 0) and the data are written",
          os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0 and "lcg2: done:" in text, text[-500:])
    for said in ("nothing: suction in the bath does nothing: touch the cell first (c)", "touch", "suction", "holding -70 mV",
                 "suction pulse", "nothing: already in whole-cell", "q: stopping"):
        check("live: the key is answered: %s" % said, said in text, text[-800:])
    readouts = [l for l in re.split(r"[\r\n]+", text) if " R " in l]
    check("live: the readout line ends in whole-cell at -70 mV (%s)" % (readouts[-1] if readouts else ""),
          bool(readouts) and "Cm 12." in readouts[-1] and "[holding -70 mV]" in readouts[-1])
    rep = open(glob.glob(os.path.join(folder_of(w), "live_replay.ini"))[0]).read()
    log = open(glob.glob(os.path.join(folder_of(w), "log.txt"))[0]).read()
    check("live: the events are logged, with the time they took effect", log.count("event  ") == 7 and "stopped with q" in log, log[-600:])
    check("live: live_replay.ini holds the session as a scripted timeline",
          all(("cell.P1.patch_%s" % k) in rep for k in ("contact", "suction", "break_in", "zap", "holding")) and "-70" in rep, rep)
    # the replay: the same events at the same simulated times, in virtual time
    f = h5py.File(h5_of(w), "r")
    n = len([k for k in f if k.startswith("trial_")])
    done = sum(1 for k in range(n) if not f["trial_%04d" % k].attrs["incomplete"])
    tl = np.array([f["trial_%04d" % k].attrs["start_ns"] for k in range(done)]) / 1e6
    tl -= tl[0]                                   # the cells' time of each pulse of the session (its trials start late)
    nrep = int(tl[-1] / 100.0) + 1                # the replay's pulses start on time: as many as cover the same span
    r2 = newdir(tmp, "live_replay")
    write(os.path.join(r2, "rig.ini"), open(os.path.join(d, "rig_patch.ini")).read() + "\n" + rep)
    write(os.path.join(r2, "s.sg"), open(os.path.join(d, "seal_test_live.sg")).read().replace("repeat   6000", "repeat   %d" % nrep))
    rr = lcg(["-q", "-s", "1", "-m", "vc", "-P", "live_replay", "-c", "rig.ini", "s.sg"], r2)
    g = h5py.File(h5_of(r2), "r")
    tr_ = np.array([g["trial_%04d" % k].attrs["start_ns"] for k in range(nrep)]) / 1e6
    rl = np.array([f["trial_%04d/ai/P1" % k].attrs["membrane_test_R_MOhm"] for k in range(done)])
    rp = np.array([g["trial_%04d/ai/P1" % k].attrs["membrane_test_R_MOhm"] for k in range(nrep)])
    ev = [float(x) for x in re.findall(r"([0-9.]+)ms", rep)]
    # the session's pulses are where they are (late, by a few ms each): compare the replay with the session where the session's
    # resistance changes by less than 2 % from one pulse to the next, so that interpolating between its pulses is accurate
    def smooth(t):
        i = int(np.searchsorted(tl, t)) - 1
        return 0 <= i < len(tl) - 1 and 0.98 < rl[i + 1] / rl[i] < 1.02
    ok = [k for k in range(nrep) if all(abs(tr_[k] - e) > 200 and abs(tr_[k] + 20 - e) > 200 for e in ev) and tr_[k] < tl[-1]
          and smooth(tr_[k])]
    rel = np.abs(rp[ok] / np.exp(np.interp(tr_[ok], tl, np.log(rl))) - 1)
    check("live: the replay gives the same resistance at the same simulated time (%d of %d pulses: worst %.2f %%, median %.3f %%; "
          "in the cell %.3f and %.3f MOhm)" % (len(ok), done, 100 * rel.max(), 100 * np.median(rel), rl[-1], rp[-1]),
          rr.returncode == 0 and len(ok) > 10 and rel.max() < 0.005 and abs(rp[-1] / rl[-1] - 1) < 1e-4, rr.stderr[-300:])
    check("live: live_replay.ini also gives the session's seed and start (the same cell when replayed)",
          "cell.P1.patch_seed     = 51" in rep and "cell.P1.patch_start    = bath" in rep, rep)

    # refusals, in a terminal; and the terminal is given back as it was (single keys off, echo on)
    import termios

    def in_terminal(args, cwd):
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(cwd)
            os.execv(LCG2, [LCG2] + args)
        out, t0 = b"", time.time()
        while time.time() - t0 < 60:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    b = os.read(fd, 4096)
                except OSError:
                    break
                if not b:
                    break
                out += b
        _, st = os.waitpid(pid, 0)
        try:
            lflag = termios.tcgetattr(fd)[3]
            restored = bool(lflag & termios.ICANON) and bool(lflag & termios.ECHO)
        except termios.error:
            restored = None
        os.close(fd)
        return (os.WEXITSTATUS(st) if os.WIFEXITED(st) else -1), out.decode(errors="replace"), restored

    w = newdir(tmp, "live_cc")
    write(os.path.join(d, "pa.sg"), "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 100ms dc(0) }\n")    # a current: cc
    rc, text, _ = in_terminal(["-L", "-m", "cc", "-P", "live", "-c", os.path.join(d, "rig_patch.ini"), os.path.join(d, "pa.sg")], w)
    check("live: -L in current clamp is refused (the live readout is the voltage-clamp seal test)",
          rc == 1 and "give -m vc" in text and not os.listdir(w), text[-300:])
    w = newdir(tmp, "live_sim")
    write(os.path.join(w, "sim.ini"), "[rig]\ndevice = sim\nmode = vc\n[out P1]\nline = ao0\ngain_vc = 20\nunit_vc = mV\n"
          "[in P1]\nline = ai0\ngain_vc = 1000\nunit_vc = pA\n")
    rc, text, restored = in_terminal(["-L", "-c", "sim.ini", os.path.join(d, "seal_test_live.sg")], w)
    check("live: -L on the loopback simulator is refused, and the terminal is given back as it was (%s)" % restored,
          rc != 0 and "needs device = neuron and a patching pipette" in text and restored is not False, text[-300:])
    w = newdir(tmp, "live_nopatch")
    write(os.path.join(w, "rig.ini"), fix_py(open(os.path.join(KIT, "rig_electrode.ini")).read()))
    rc, text, restored = in_terminal(["-L", "-m", "vc", "-c", "rig.ini", os.path.join(d, "seal_test_live.sg")], w)
    check("live: a NEURON rig without a patching pipette is refused, and the terminal is given back (%s)" % restored,
          rc != 0 and "live mode needs a patching pipette" in text and restored is not False, text[-300:])

    # without a terminal, -L is refused before anything is written
    w = newdir(tmp, "live_noterm")
    r = lcg(["-L", "-m", "vc", "-P", "live", "-c", os.path.join(d, "rig_patch.ini"), os.path.join(d, "seal_test_live.sg")], w)
    check("live: without a terminal -L is refused, and nothing is written",
          r.returncode == 1 and "needs a terminal" in r.stderr and not os.listdir(w), r.stderr)


def main():
    tmp = tempfile.mkdtemp(prefix="lcg2_neuron_test_")
    try:
        for t in (test_reference, test_overrange, test_vc, test_pair, test_state_between_trials, test_models,
                  test_mistakes, test_slow_check_tolerated, test_hoc_hooks, test_kit, test_tutorials, test_electrodes, test_physics, test_patch, test_tutorial_l, test_live, test_kill_adapter, test_ctrl_c):
            t(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("NEURON backend tests: %d checks, %d failed (NEURON via %s)" % (count, len(failures), PY))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
