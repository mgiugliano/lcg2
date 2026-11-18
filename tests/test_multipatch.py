#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M5 acceptance test: a multi-patch rig on the loopback simulator, end to end, with no hardware.
#
#     python3 tests/test_record.py        (after `make`)
#
# It reads the data files back with h5py (found as in test_record.py; the test FAILS if there is none).
#
# What it proves, on a rig of three pipettes and a digital line:
#   * every pipette is recorded as what it was commanded, through its own gain and calibration, with no
#     crosstalk between channels (an idle pipette records its rest value);
#   * voltage clamp (-m vc) and the profiles (skip, re-addressing) behave as documented;
#   * a protocol's trials are recorded one by one, each with its own deterministic noise; the same command
#     gives the same recording;
#   * a digital line is recorded as 0/1 with the right number of high samples;
#   * nothing in the program needs hardware: no NI header or library is referenced, a board name is refused
#     before anything is touched, and the NEURON tests skip (loudly, exit 0) when told there is no NEURON.
# Every real run happens in a scratch folder, never in the repository.
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

# ---- find an interpreter with h5py, and re-run with it --------------------------------
try:
    import h5py
except ImportError:
    h5py = None
if h5py is None:
    candidates = []
    if os.environ.get("LCG2_H5PY_PYTHON"):
        candidates.append(os.environ["LCG2_H5PY_PYTHON"])
    candidates += ["/opt/homebrew/opt/python@3.11/bin/python3.11", "/opt/homebrew/opt/python@3.12/bin/python3.12",
                   "/opt/homebrew/opt/python@3.13/bin/python3.13", "/usr/bin/python3"]
    for name in ("python3.11", "python3.12", "python3.13", "python3.10"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    for cand in candidates:
        if os.path.exists(cand) and subprocess.run([cand, "-c", "import h5py"], stdout=subprocess.DEVNULL,
                                                   stderr=subprocess.DEVNULL).returncode == 0:
            os.execv(cand, [cand] + sys.argv)
    print("FAIL  no Python with h5py was found, so the data files cannot be read back.\n"
          "      Install it (pip install h5py) in a Python of yours, or set LCG2_H5PY_PYTHON to one.\n"
          "      This test does not skip silently: a recorder that cannot be read back is untested.")
    sys.exit(1)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("LCG2_BUILD", os.path.join(ROOT, "build"))
LCG2 = os.environ.get("LCG2_BIN", os.path.join(ROOT, "lcg2"))
RUNTEST = os.path.join(BUILD, "runtest")

failures = []
count = 0


def check(name, ok, detail=""):
    global count
    count += 1
    if not ok:
        failures.append(name)
        print("FAIL  %s" % name)
        for line in str(detail).splitlines()[:10]:
            print("      " + line)


def go(cmd, cwd, **kw):
    kw.setdefault("timeout", 120)
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, errors="replace", **kw)


def newdir(tmp, name):
    d = os.path.join(tmp, name)
    os.makedirs(d)
    return d


def h5s(cwd):
    return sorted(glob.glob(os.path.join(cwd, "*.h5")))


def exact(args, cwd):
    """The real run_execute() with the loopback noise switched off (the driver of the tests)."""
    return go([RUNTEST, "--noise", "0", "--"] + args, cwd)


RIG = """[rig]
device = sim
mode = cc
rate = 20kHz

[out P1]
line = ao0
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV
[out P2]
line = ao1
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV
[out P3]
line = ao2
gain_cc = 200
unit_cc = pA
gain_vc = 50
unit_vc = mV
rest = -30
[out TTL]
line = port0/line0
digital = yes

[in P1]
line = ai0
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA
[in P2]
line = ai1
gain_cc = 50
unit_cc = mV
gain_vc = 500
unit_vc = pA
cal_scale = 1.1
cal_offset = 0.01
[in P3]
line = ai2
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA

[profile single]
out.P2.skip = yes
in.P2.skip = yes
out.P3.skip = yes
in.P3.skip = yes

[profile moved]
out.P2.line = ao3
in.P2.line = ai3
"""

STIM = """sg 2 stimulus
rate 20kHz
channel P1 unit=pA { 1s dc(100) }
channel P2 unit=pA { 1s dc(-200) }
digital TTL { 1s pulses(amp=1, rate=100Hz, width=1ms) }
"""

STIM_VC = """sg 2 stimulus
rate 20kHz
channel P1 unit=mV { 1s dc(-40) }
channel P2 unit=mV { 1s dc(10) }
"""

PROTO = """sg 2 protocol
repeat 3
period 2s

stimulus {
    channel P1 unit=pA { 500ms dc(100) }
}
"""


def write(path, s):
    with open(path, "w") as f:
        f.write(s)
    return path


def means(h5, trial, names=("P1", "P2", "P3")):
    with h5py.File(h5, "r") as f:
        g = f["trial_%04d" % trial]
        return {n: float(g["ai/" + n][:].mean()) for n in names if n in g["ai"]}, \
               {n: float(g["ai/" + n][:].std()) for n in names if n in g["ai"]}


def close(a, b, tol=1e-6):
    return abs(a - b) <= tol


def test_cc(tmp):
    d = newdir(tmp, "cc")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    r = exact(["-c", ini, write(os.path.join(d, "s.sg"), STIM)], d)
    check("three pipettes and a digital line run", r.returncode == 0, r.stderr)
    m, sd = means(h5s(d)[0], 0)
    # P1: 100 pA / 400 pA/V = 0.25 V; read at 100 mV/V -> 25 mV
    check("P1 records 25 mV (100 pA through its own gains)", close(m["P1"], 25.0), m)
    # P2: -200 pA / 400 = -0.5 V; read with cal 1.1*v + 0.01 at 50 mV/V -> 50 * (1.1*-0.5 + 0.01) = -27.0
    check("P2 records -27 mV (its own gain and calibration)", close(m["P2"], -27.0), m)
    # P3 is idle: rest -30 pA / 200 pA/V = -0.15 V; read at 100 mV/V -> -15 mV
    check("P3 is idle and records its rest value, -15 mV (no crosstalk)", close(m["P3"], -15.0), m)
    check("noise is off in the exact driver", sd["P1"] < 1e-9 and sd["P2"] < 1e-9, sd)
    with h5py.File(h5s(d)[0], "r") as f:
        g = f["trial_0000"]
        ttl = g["ao/TTL"][:]
        check("the digital line is recorded as 0/1 with 100 pulses of 20 samples (%d high)" % ttl.sum(),
              ttl.dtype.name == "uint8" and set(ttl.tolist()) <= {0, 1} and int(ttl.sum()) == 2000, ttl.dtype)
        check("only the driven outputs have a command dataset", sorted(g["ao"]) == ["P1", "P2", "TTL"], sorted(g["ao"]))
        check("all three inputs are recorded", sorted(g["ai"]) == ["P1", "P2", "P3"], sorted(g["ai"]))
        check("P2's calibration is in the file", abs(g["ai/P2"].attrs["cal_scale"] - 1.1) < 1e-12)


def test_vc(tmp):
    d = newdir(tmp, "vc")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    r = exact(["-m", "vc", "-c", ini, write(os.path.join(d, "s.sg"), STIM_VC)], d)
    check("voltage clamp runs on the same rig", r.returncode == 0, r.stderr)
    m, _ = means(h5s(d)[0], 0)
    # P1: -40 mV / 20 mV/V = -2 V; read 1000 pA/V -> -2000 pA
    check("P1 records -2000 pA", close(m["P1"], -2000.0), m)
    # P2: 10/20 = 0.5 V; 500 * (1.1*0.5 + 0.01) = 280
    check("P2 records 280 pA", close(m["P2"], 280.0), m)
    # P3 idle rest -30 mV / 50 = -0.6 V; read 1000 pA/V -> -600 pA
    check("P3 idles at its rest in voltage clamp (-30 mV), -600 pA", close(m["P3"], -600.0), m)
    with h5py.File(h5s(d)[0], "r") as f:
        check("units follow the mode", f["trial_0000/ai/P1"].attrs["unit"] in ("pA", b"pA")
              and f["trial_0000/ao/P1"].attrs["unit"] in ("mV", b"mV"))


def test_profiles(tmp):
    d = newdir(tmp, "single")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    stim = write(os.path.join(d, "one.sg"), "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(100) }\n")
    r = exact(["-P", "single", "-c", ini, stim], d)
    with h5py.File(h5s(d)[0], "r") as f:
        check("-P single: only P1 is recorded", r.returncode == 0 and sorted(f["trial_0000/ai"]) == ["P1"], sorted(f["trial_0000/ai"]))
    d = newdir(tmp, "moved")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    r = exact(["-P", "moved", "-c", ini, write(os.path.join(d, "s.sg"), STIM)], d)
    m, _ = means(h5s(d)[0], 0)
    check("-P moved: P2 re-addressed to ao3/ai3 still records its command", r.returncode == 0 and close(m["P2"], -27.0), m)
    # a stimulus naming a skipped output is refused before anything is played
    d = newdir(tmp, "skipped")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    r = exact(["-P", "single", "-c", ini, write(os.path.join(d, "s.sg"), STIM)], d)
    check("a stimulus for a skipped pipette is refused, nothing written", r.returncode == 1 and not h5s(d), r.stderr)


def test_protocol_and_noise(tmp):
    d = newdir(tmp, "proto")
    ini = write(os.path.join(d, "rig.ini"), RIG)
    stim = write(os.path.join(d, "p.sg"), PROTO)
    r = go([LCG2, "-q", "-c", ini, stim], d)
    check("a protocol of three trials runs with noise on", r.returncode == 0, r.stderr)
    with h5py.File(h5s(d)[0], "r") as f:
        t = [f["trial_%04d/ai/P1" % k][:] for k in range(3)]
        starts = [int(f["trial_%04d" % k].attrs["start_ns"]) for k in range(3)]
        seeds = [f["trial_%04d" % k].attrs["seed"] for k in range(3)]
    check("trials start every 2 s", starts == [0, 2000000000, 4000000000], starts)
    check("each trial has noise of its own (about 0.2 mV rms) but the same mean",
          not (t[0] == t[1]).all() and 0.1 < t[0].std() < 0.4 and abs(t[0].mean() - t[1].mean()) < 0.05,
          (t[0].std(), t[0].mean(), t[1].mean()))
    check("trials have distinct seeds", len(set(map(str, seeds))) == 3, seeds)
    d2 = newdir(tmp, "proto2")
    ini2 = write(os.path.join(d2, "rig.ini"), RIG)
    go([LCG2, "-q", "-c", ini2, write(os.path.join(d2, "p.sg"), PROTO)], d2)
    with h5py.File(h5s(d2)[0], "r") as f:
        same = all((f["trial_%04d/ai/P1" % k][:] == t[k]).all() for k in range(3))
    check("the same command gives the same recording, noise included", same)


def test_no_hardware(tmp):
    hits = []
    for dp, _dn, fn in os.walk(os.path.join(ROOT, "src")):
        for f in fn:
            if f.endswith((".c", ".h")) and "NIDAQmx" in open(os.path.join(dp, f), errors="replace").read():
                hits.append(f)
    check("no source references NI-DAQmx (there is no hardware backend yet)", not hits, hits)
    d = newdir(tmp, "board")
    ini = write(os.path.join(d, "rig.ini"), RIG.replace("device = sim", "device = Dev1"))
    r = go([LCG2, "-c", ini, write(os.path.join(d, "s.sg"), STIM)], d)
    check("a board name is refused before anything is touched (status 1, nothing written)",
          r.returncode == 1 and "no hardware backend" in r.stderr and not h5s(d) and not glob.glob(os.path.join(d, ".2*")), r.stderr)
    env = dict(os.environ, LCG2_NO_NEURON="1")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "test_neuron.py")], env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True, timeout=60)
    check("without NEURON the NEURON tests skip loudly with status 0, and say nothing was tested",
          r.returncode == 0 and "SKIPPED" in r.stdout and "Nothing of M3b was tested" in r.stdout, r.stdout + r.stderr)


def main():
    for tool in (RUNTEST, LCG2):
        if not os.path.exists(tool):
            print("missing %s: run `make` first" % os.path.relpath(tool, ROOT))
            return 1
    tmp = tempfile.mkdtemp(prefix="lcg2_multipatch_")
    try:
        for fn in (test_cc, test_vc, test_profiles, test_protocol_and_noise, test_no_hardware):
            fn(newdir(tmp, fn.__name__))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("multi-patch tests: %d checks, %d failed" % (count, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
