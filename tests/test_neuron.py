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
    return subprocess.run([py, "-c", "import " + ", ".join(mods)], stdout=subprocess.DEVNULL,
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


PY = None if os.environ.get("LCG2_NO_NEURON") else find_python()
if PY is None:
    print("SKIPPED  device = neuron: no Python with both NEURON and h5py was found.\n"
          "         Install them (pip install neuron h5py) or set LCG2_NEURON_PYTHON.  Nothing of M3b was tested.")
    sys.exit(0)
if os.path.realpath(PY) != os.path.realpath(sys.executable):
    os.execv(PY, [PY] + sys.argv)

import array   # noqa: E402
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
        check("P2 is quiet before the spike (nothing but its own rest)", abs(p2[:spike].max() - p2[:spike].min()) < 0.05,
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
    check("without the synapse P2 does not move", r.returncode == 0 and q2.max() - q2.min() < 0.05, "%g" % (q2.max() - q2.min()))


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
            s = open(os.path.join(KIT, f)).read().replace("/opt/homebrew/opt/python@3.11/bin/python3.11", PY)
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


def test_hoc_hooks(tmp):
    d = newdir(tmp, "hoc_hooks")
    write(os.path.join(d, "bio.hoc"), "proc biophysics() { forsec $o1 { insert pas  g_pas = 0.0002  e_pas = -60 } }\n")
    write(os.path.join(d, "ini.hoc"), "proc initialize() { forsec $o1 { v = -50 } }\n")
    ini = write(os.path.join(d, "c.ini"), "[cell A]\nmorphology = %s\nbiophysics = bio.hoc\ninit = ini.hoc\n" % os.path.join(MODELS, "tiny.swc"))
    r = go([PY, os.path.join(ROOT, "neuron", "lcg2_neuron.py"), "--check", ini], d)
    check("hoc biophysics and init procedures run (the cell starts at -50 mV, as the init proc says)",
          r.returncode == 0 and "rests at -50.0 mV" in r.stdout, r.stdout + r.stderr)


def main():
    tmp = tempfile.mkdtemp(prefix="lcg2_neuron_test_")
    try:
        for t in (test_reference, test_overrange, test_vc, test_pair, test_state_between_trials, test_models,
                  test_mistakes, test_slow_check_tolerated, test_hoc_hooks, test_kit, test_kill_adapter, test_ctrl_c):
            t(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("NEURON backend tests: %d checks, %d failed (NEURON via %s)" % (count, len(failures), PY))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
