#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M3 acceptance test: the trial engine and the loopback simulator.
#
#     python3 tests/test_engine.py        (after `make`)
#
# Two kinds of test, and why:
#   * driver tests: build/enginetest runs the real engine on the VIRTUAL clock, so
#     every time is exact and nothing sleeps. They check the timing rules of a
#     protocol (period, gap, a trial longer than its period, start latency, a
#     trigger), the order of operations (every exit path ends with the outputs at
#     rest), the inputs the loopback produces, and that what was not checked is
#     never played.
#   * process tests: the real `lcg2` program gets real signals. They block on the
#     Enter prompt of `start on trigger` instead of racing a clock, so they do not
#     flake. A real-time run is checked with loose bounds only.
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("LCG2_BUILD", os.path.join(ROOT, "build"))
LCG2 = os.environ.get("LCG2_BIN", os.path.join(ROOT, "lcg2"))
ET = os.path.join(BUILD, "enginetest")
CFG = "examples/lcg2.ini"          # P1 ao0 (limit 5 V), P2 ao1, TTL port0/line0; inputs ai0, ai1

failures = []
count = 0
S = 1_000_000_000                   # one second in ns

# Every real run (one that is not a dry run) writes a data file and a run folder into its
# working folder.  They must never land in the repository, so real runs happen in WORK, a
# scratch folder, and paths that name files of the repository are made absolute.
WORK = tempfile.mkdtemp(prefix="lcg2_engine_work_")


def A(args):
    out = []
    for a in args:
        out.append(os.path.join(ROOT, a) if (a and not a.startswith("-") and
                                              os.path.exists(os.path.join(ROOT, a))) else a)
    return out


def check(name, ok, detail=""):
    global count
    count += 1
    if not ok:
        failures.append(name)
        print("FAIL  %s" % name)
        for line in str(detail).splitlines()[:10]:
            print("      " + line)


def write(tmp, name, text):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write(text)
    return p


def run_et(driver, lcg2args, cfg=CFG, stdin="", timeout=60):
    cmd = [ET] + driver + ["--", "-c", cfg] + lcg2args
    r = subprocess.run(cmd, cwd=ROOT, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True, errors="replace", timeout=timeout)
    return Out(r)


class Out:
    """The lines of the driver's output, parsed."""
    def __init__(self, r):
        self.rc = r.returncode
        self.stderr = r.stderr
        self.raw = r.stdout
        self.trials, self.inputs = [], {}
        self.events = ""
        self.state = {}
        self.status = None
        self.error = ""
        for l in r.stdout.splitlines():
            w = l.split()
            if not w:
                continue
            if w[0] == "T":
                self.trials.append(dict(index=int(w[1]), name=w[2], scheduled=int(w[3]), start=int(w[4]),
                                        end=int(w[5]), delay=int(w[6]), latency=int(w[7]),
                                        recorded=int(w[8]), incomplete=int(w[9]), utc=w[10] if len(w) > 10 else ""))
            elif w[0] == "I":
                self.inputs[(int(w[1]), w[2])] = dict(n=int(w[3]), mn=float(w[4]), mx=float(w[5]),
                                                      mean=float(w[6]), over=int(w[7]), fnv=w[8])
            elif w[0] == "EVENTS":
                self.events = w[1] if len(w) > 1 else ""
            elif w[0] == "STATE":
                self.state = {k: float(v) for k, v in (x.split("=") for x in w[1:])}
            elif w[0] == "STATUS":
                self.status = int(w[1])
                self.kv = {k: int(v) for k, v in (x.split("=") for x in w[2:])}
            elif w[0] == "ERROR":
                self.error = l[6:]

    def starts(self):
        return [t["start"] for t in self.trials]

    def delays(self):
        return [t["delay"] for t in self.trials]


def protocol(tmp, name, timing, repeat=4, dur="2s", amp=100, rest=None, extra=""):
    r = (" rest=%s" % rest) if rest is not None else ""
    return write(tmp, name, "sg 2 protocol\nrepeat %d\n%s\n%s\nstimulus {\n channel P1 unit=pA%s { %s dc(%d) }\n}\n"
                 % (repeat, timing, extra, r, dur, amp))


# --------------------------------------------------------------------------
# 1. The timing rules, exactly
# --------------------------------------------------------------------------
def test_timing(tmp):
    base = ["-s", "1", "-r", "20kHz"]

    p = protocol(tmp, "period.sg", "period 5s")
    o = run_et([], base + [p])
    check("period 5s: trials start 5 s apart", o.starts() == [0, 5 * S, 10 * S, 15 * S], o.raw)
    check("period 5s: no delay", o.delays() == [0] * 4 and o.kv["max_delay"] == 0, o.delays())
    check("period 5s: each trial lasts its 2 s", [t["end"] - t["start"] for t in o.trials] == [2 * S] * 4)
    check("period 5s: the run ends when the last trial does", o.kv["elapsed"] == 17 * S, o.kv)

    p = protocol(tmp, "gap.sg", "gap 2s")
    o = run_et([], base + [p])
    check("gap 2s: each trial starts 2 s after the previous one ended", o.starts() == [0, 4 * S, 8 * S, 12 * S],
            o.starts())

    p = protocol(tmp, "gap0.sg", "gap 0s")
    o = run_et([], base + [p])
    check("gap 0s: back to back", o.starts() == [0, 2 * S, 4 * S, 6 * S], o.starts())

    p = protocol(tmp, "default.sg", "")
    o = run_et([], base + [p])
    check("a protocol with no timing statement is gap 0s", o.starts() == [0, 2 * S, 4 * S, 6 * S], o.starts())

    p = protocol(tmp, "frac.sg", "period 2.5s")
    o = run_et([], base + [p])
    check("period 2.5s", o.starts() == [0, 2500000000, 5 * S, 7500000000], o.starts())

    p = protocol(tmp, "ms.sg", "gap 500ms")
    o = run_et([], base + [p])
    check("gap 500ms", o.starts() == [0, 2500000000, 5 * S, 7500000000], o.starts())

    # a trial longer than its period: the next starts when it ends, the delay is recorded
    p = protocol(tmp, "short.sg", "period 1s")
    o = run_et([], base + [p])
    check("a trial longer than the period: the next starts as soon as it ends",
          o.starts() == [0, 2 * S, 4 * S, 6 * S], o.starts())
    check("...and the delay is recorded (1 s each time, none for the first)",
          o.delays() == [0, S, S, S] and o.kv["max_delay"] == S, o.delays())
    check("...the scheduled time is the previous start plus the period",
          [t["scheduled"] for t in o.trials] == [0, 1 * S, 3 * S, 5 * S], [t["scheduled"] for t in o.trials])

    # start latency of the backend counts in the delay (15 ms)
    p = protocol(tmp, "lat.sg", "period 5s")
    o = run_et(["--latency", "15000000"], base + [p])
    check("start latency is part of the delay", o.delays() == [15000000] * 4 and
          all(t["latency"] == 15000000 for t in o.trials), o.delays())
    check("period counts from the START of the previous trial, latency included",
          o.starts() == [15000000 + i * 5 * S + i * 15000000 for i in range(4)], o.starts())
    p = protocol(tmp, "latgap.sg", "gap 2s")
    o = run_et(["--latency", "15000000"], base + [p])
    ends = [t["end"] for t in o.trials]
    check("gap: every trial starts 2 s (and its own latency) after the previous end",
          all(o.trials[i]["start"] == ends[i - 1] + 2 * S + 15000000 for i in range(1, 4)), o.raw)

    # the wall-clock time of each trial: to the millisecond, from the run's start plus the exact offset
    p = protocol(tmp, "utc.sg", "period 5s", dur="2s")
    o = run_et([], base + [p])
    utc = [t["utc"] for t in o.trials]
    check("each trial has a UTC start time to the millisecond", all(re.match(
        r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$", u) for u in utc), utc)

    def ms(u):
        h, m, sec = u[11:-1].split(":")
        return (int(h) * 3600 + int(m) * 60) * 1000 + round(float(sec) * 1000)
    d = [ms(utc[i + 1]) - ms(utc[i]) for i in range(3)]
    check("...and they are 5 s apart, as the clock says (not all in one second)",
          all(abs(x - 5000) <= 1 for x in d), (utc, d))
    p = protocol(tmp, "fastutc.sg", "gap 0s", dur="0.1s", repeat=4)
    o = run_et(["--latency", "15000000"], base + [p])
    utc = [ms(t["utc"]) for t in o.trials]
    check("trials a tenth of a second apart get different times", len(set(utc)) == 4 and
          all(abs((utc[i + 1] - utc[i]) - 115) <= 1 for i in range(3)), (utc, o.starts()))

    # a plain stimulus is one trial
    o = run_et([], ["examples/step.sg"])
    check("a plain stimulus is one trial at time 0", o.starts() == [0] and o.trials[0]["end"] == 2 * S and
          o.status == 0, o.raw)

    # one hour in a second of computing: no overflow, no sleeping
    o = run_et([], ["-t", "60min", "-r", "1Hz"])
    check("a one-hour record-only run takes exactly one hour of simulated time",
          o.status == 0 and o.kv["elapsed"] == 3600 * S and o.trials[0]["recorded"] == 3600, o.raw[:300])


# --------------------------------------------------------------------------
# 2. Start on trigger
# --------------------------------------------------------------------------
def test_trigger(tmp):
    base = ["-s", "1", "-r", "20kHz"]
    p = protocol(tmp, "trig.sg", "period 5s\nstart on trigger", repeat=3)
    with_pfi = write(tmp, "pfi.ini", open(os.path.join(ROOT, CFG)).read().replace("rate = 20kHz", "rate = 20kHz\ntrigger = PFI0"))
    o = run_et([], base + [p], cfg=with_pfi)
    check("trigger on a PFI line: simulated at once, before every trial", o.status == 0 and
          o.events == "ORTPRTPRTPRRC", o.events)
    check("trigger on a PFI line: the period still holds", o.starts() == [0, 5 * S, 10 * S], o.starts())

    o = run_et([], base + [p], stdin="\n\n\n")
    check("trigger without a PFI line: Enter starts each trial", o.status == 0 and o.events == "ORTPRTPRTPRRC",
          o.events + o.stderr)
    check("...and the prompt says what to do", "press Enter" in o.stderr, o.stderr)

    o = run_et([], base + [p], stdin="\n")
    check("trigger: Enter once, then the input ends: the run stops with the outputs at rest",
          o.status == 2 and o.kv["completed"] == 1 and o.events.endswith("RC") and o.events == "ORTPRTRC", o.raw)

    o = run_et([], base + [p], stdin="")
    check("trigger: the input ends before any Enter: nothing is played, outputs at rest",
          o.status == 2 and o.kv["started"] == 0 and o.events == "ORTRC", o.raw)


# --------------------------------------------------------------------------
# 3. Rest values, on every way out
# --------------------------------------------------------------------------
def test_rest(tmp):
    # P1 rests at the stimulus's own rest (50 pA = 0.125 V); the idle P2 at the rest of the
    # configuration (100 pA = 0.25 V); TTL at 0. Expected values computed here, not by the program.
    cfg = write(tmp, "rest.ini", open(os.path.join(ROOT, CFG)).read().replace("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 100"))
    p = protocol(tmp, "rest.sg", "period 5s", repeat=5, dur="2s", amp=200, rest=50)
    base = ["-s", "1", "-r", "20kHz", p]
    want = {"ao0": 50 / 400, "ao1": 100 / 400, "line0": 0}

    def at_rest(o):
        return all(abs(o.state[k] - v) < 1e-15 for k, v in want.items())

    o = run_et([], base, cfg=cfg)
    check("a normal run ends with every output at its rest value", o.status == 0 and at_rest(o), o.state)
    check("a normal run: open, rest, (play, rest) x trials, rest, close", o.events == "OR" + "PR" * 5 + "RC", o.events)

    o = run_et(["--abort-now"], base, cfg=cfg)
    check("abort before the first trial: nothing played, outputs at rest",
          o.status == 2 and o.kv["started"] == 0 and o.events == "ORRC" and at_rest(o), o.raw)

    o = run_et(["--abort-after", "1"], base, cfg=cfg)
    check("abort between trials: the next trial is not played, outputs at rest",
          o.status == 2 and o.kv["completed"] == 2 and len(o.trials) == 2 and o.events == "ORPRPRRC" and at_rest(o),
          o.raw)

    o = run_et(["--abort-block", "2:3"], base, cfg=cfg)
    t = o.trials[-1]
    check("abort during a trial: the trial is reported incomplete, with the samples it had",
          o.status == 2 and len(o.trials) == 3 and t["incomplete"] == 1 and t["recorded"] == 4 * 2000, o.raw)
    check("abort during a trial: outputs end at rest, not where the trial left them",
          o.events == "ORPRPRPpRC" and at_rest(o), o.events + str(o.state))

    o = run_et(["--tamper", "2"], base, cfg=cfg)
    check("a trial that is not the one that was checked is refused",
          o.status == 3 and "not the one that was checked" in o.error and len(o.trials) == 2, o.raw)
    check("...and the outputs end at rest", o.events == "ORPRPRRC" and at_rest(o), o.events)

    o = run_et(["--fail-block", "1"], base, cfg=cfg)
    check("a failure inside a trial: reported, outputs at rest",
          o.status == 3 and "told to fail" in o.error and o.events == "ORPRPRC", o.raw)
    check("...the failed trial was reported to the sink as incomplete", o.trials[-1]["incomplete"] == 1 and
          at_rest(o), o.raw)

    # a failure to put the outputs at rest is never silent (on a board it is the worst failure there is)
    p2 = protocol(tmp, "twotrials.sg", "period 5s", repeat=2)
    b2 = ["-s", "1", "-r", "20kHz", p2]
    o = run_et(["--fail-rest", "1"], b2, cfg=cfg)
    check("the first rest fails: the run does not start, and says the outputs may not be at rest",
          o.status == 3 and o.kv["started"] == 0 and o.kv["rest_failed"] == 1 and
          "outputs may NOT be at rest" in o.stderr and o.events == "OC", o.raw + o.stderr)
    o = run_et(["--fail-rest", "3"], b2, cfg=cfg)
    check("a rest between trials fails: the run stops, and the final attempt fails loudly too",
          o.status == 3 and o.kv["rest_failed"] == 1 and "outputs may NOT be at rest" in o.stderr and
          "told to fail" in o.error, o.raw + o.stderr)
    o = run_et(["--fail-rest", "4"], b2, cfg=cfg)
    check("only the very last rest fails: a run that otherwise succeeded is reported as failed",
          o.status == 3 and o.kv["completed"] == 2 and o.kv["rest_failed"] == 1 and
          "outputs may NOT be at rest" in o.stderr, o.raw + o.stderr)
    o = run_et([], b2, cfg=cfg)
    check("(and with no failure the report says so)", o.kv["rest_failed"] == 0 and "NOT" not in o.stderr)

    # before anything is touched a refusal is not a failure of the run
    o = run_et([], ["-s", "1", "-r", "20kHz", "-e", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) }"],
               cfg=cfg)
    check("(a plain stimulus works through the same path)", o.status == 0 and o.events == "ORPRRC", o.raw)


# --------------------------------------------------------------------------
# 4. The loopback: what the inputs measure
# --------------------------------------------------------------------------
def test_loopback(tmp):
    # no noise: exact numbers. P1: 200 pA = 0.5 V, wired ao0 -> ai0, in = 100 mV per volt = 50 mV.
    o = run_et(["--noise", "0"], ["-s", "1", "examples/step.sg"])
    a = o.inputs[(0, "P1")]
    check("loopback, no noise: the input sees exactly the output (in physical units)",
          a["n"] == 40000 and a["mn"] == 0.0 and a["mx"] == 50.0 and a["mean"] == 25.0 and a["over"] == 0, a)
    b = o.inputs[(0, "P2")]
    check("an input with no output of its own number hears only the idle output (here 0)",
          b["mn"] == 0.0 and b["mx"] == 0.0, b)

    # in voltage clamp the gains and units of that mode apply: 20 mV at 20 mV/V = 1 V; in = 1000 pA/V -> 1000 pA
    o = run_et(["--noise", "0"], ["-s", "1", "-m", "vc", "examples/step_vc.sg"], cfg="examples/single_pipette.ini")
    a = o.inputs[(0, "P1")]
    check("voltage clamp: 20 mV -> 1 V -> 1000 pA", a["mx"] == 1000.0 and a["mean"] == 500.0, a)

    # calibration on the input: v_true = scale * v + offset, then times the gain
    cfg = write(tmp, "cal.ini", open(os.path.join(ROOT, CFG)).read().replace("[in P1]\nline = ai0", "[in P1]\nline = ai0\ncal_scale = 2\ncal_offset = 0.25"))
    o = run_et(["--noise", "0"], ["-s", "1", "examples/step.sg"], cfg=cfg)
    a = o.inputs[(0, "P1")]
    check("input calibration: 100 mV/V * (2 * 0.5 V + 0.25 V) = 125 mV on the step, 25 mV at rest",
          a["mx"] == 125.0 and a["mn"] == 25.0, a)

    # noise: small, reproducible, different with another seed
    o1 = run_et([], ["-s", "1", "examples/step.sg"])
    o2 = run_et([], ["-s", "1", "examples/step.sg"])
    o3 = run_et(["--seed", "2"], ["-s", "1", "examples/step.sg"])
    k = (0, "P1")
    check("the noise is deterministic: the same command gives the same recording",
          o1.inputs[k]["fnv"] == o2.inputs[k]["fnv"], (o1.inputs[k], o2.inputs[k]))
    check("...another noise seed gives another recording", o1.inputs[k]["fnv"] != o3.inputs[k]["fnv"])
    check("the noise is about 2 mV x 100 mV/V = 0.2 mV and averages out",
          abs(o1.inputs[k]["mean"] - 25.0) < 0.02 and 40000 == o1.inputs[k]["n"], o1.inputs[k])
    check("...and the two inputs have different noise", o1.inputs[(0, "P1")]["fnv"] != o1.inputs[(0, "P2")]["fnv"])
    p = protocol(tmp, "noise.sg", "period 3s", repeat=3)
    o = run_et([], ["-s", "1", "-r", "20kHz", p])
    f = [o.inputs[(i, "P2")]["fnv"] for i in range(3)]
    check("each trial has its own noise", len(set(f)) == 3, f)

    # the input range: clamped and counted, never passed on
    cfg = write(tmp, "range.ini", open(os.path.join(ROOT, CFG)).read().replace("[in P1]\nline = ai0", "[in P1]\nline = ai0\nrange = 1"))
    p = write(tmp, "big.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 500ms dc(0)\n 1s dc(1000)\n 500ms dc(0) }\n")
    o = run_et(["--noise", "0"], ["-s", "1", p], cfg=cfg)        # 1000 pA = 2.5 V, range 1 V
    a = o.inputs[(0, "P1")]
    check("an input is clamped at its range (1 V x 100 mV/V = 100 mV)", a["mx"] == 100.0, a)
    check("...and the clamped samples are counted: 1.5 s... the 1 s plateau, 20000 samples", a["over"] == 20000, a)
    o = subprocess.run([LCG2, "-c", cfg, p], cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    check("the program warns about it", o.returncode == 0 and "20000 samples beyond the range of 1 V" in o.stderr,
          o.stderr)

    # idle inputs in a record-only run
    cfg = write(tmp, "idle.ini", open(os.path.join(ROOT, CFG)).read().replace("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 100"))
    o = run_et(["--noise", "0"], ["-t", "1s"], cfg=cfg)
    check("record-only: one trial, every output at rest, inputs hear the rest values",
          o.status == 0 and len(o.trials) == 1 and o.trials[0]["recorded"] == 20000 and
          o.inputs[(0, "P1")]["mean"] == 0.0 and o.inputs[(0, "P2")]["mean"] == 25.0 and
          o.events == "ORPRRC", o.raw)
    check("record-only: the name of the trial", o.trials[0]["name"] == "record")


# --------------------------------------------------------------------------
# 5. The real program: output streams, exit status, refusals
# --------------------------------------------------------------------------
def lcg2(args, **kw):
    kw.setdefault("timeout", 120)
    return subprocess.run([LCG2] + A(args), cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, **kw)


def test_program(tmp):
    r = lcg2(["-c", CFG, "examples/step.sg"])
    check("a run succeeds with exit status 0", r.returncode == 0, r.stderr)
    check("a run writes nothing on stdout", r.stdout == "", r.stdout)
    check("the summary says where the data and the run folder are",
          re.search(r"wrote \d{8}_\d{6}(-\d+)?\.h5 and \.\d{8}_\d{6}(-\d+)?/", r.stderr) is not None, r.stderr)
    check("the progress line says that the time is simulated",
          "start 0.000 s  delay 0.000 s  (simulated time)" in r.stderr, r.stderr)
    r = lcg2(["-q", "-c", CFG, "examples/step.sg"])
    check("-q silences the progress", r.returncode == 0 and r.stderr == "", r.stderr)
    r = lcg2(["-c", CFG, "-P", "single", "-s", "7", "examples/steps_protocol.sg"])
    lines = [l for l in r.stderr.splitlines() if " trial " in l and "/39" in l]
    check("a protocol: one progress line per trial with its variables and exact times",
          r.returncode == 0 and len(lines) == 39 and "amp=" in lines[0] and "start 5.000 s" in lines[1] and
          "start 190.000 s" in lines[38], r.stderr[-400:])
    r = lcg2(["-c", CFG, "-t", "1s"])
    check("-t records for that long", r.returncode == 0 and "1 trial(s) in 1.000 s" in r.stderr, r.stderr)
    r = lcg2(["-c", "examples/single_pipette.ini", "-t", "1s"])
    check("-t with a rate in the configuration works", r.returncode == 0, r.stderr)
    nor = write(tmp, "norate.ini", open(os.path.join(ROOT, CFG)).read().replace("rate = 20kHz\n", ""))
    r = lcg2(["-c", nor, "-t", "1s"])
    check("-t with no rate anywhere is an error (before anything is touched)", r.returncode == 1 and
          "no sampling rate" in r.stderr, r.stderr)
    r = lcg2(["-c", nor, "-r", "10kHz", "-t", "1s"])
    check("-t with -r works", r.returncode == 0, r.stderr)
    r = lcg2(["-c", CFG, "-t", "1us"])
    check("-t shorter than one sample is refused", r.returncode == 1 and "shorter than one sample" in r.stderr, r.stderr)

    neu = write(tmp, "neuron.ini", open(os.path.join(ROOT, CFG)).read().replace("device = sim", "device = neuron\nsim_script = m.py"))
    r = lcg2(["-c", neu, "examples/step.sg"])
    check("device = neuron is refused with a clear message (exit 1)", r.returncode == 1 and
          "milestone M3b" in r.stderr, r.stderr)
    dev = write(tmp, "dev.ini", open(os.path.join(ROOT, CFG)).read().replace("device = sim", "device = Dev1"))
    r = lcg2(["-c", dev, "examples/step.sg"])
    check("a board device is refused: there is no hardware backend yet (exit 1)", r.returncode == 1 and
          "no hardware backend" in r.stderr, r.stderr)
    r = lcg2(["-n", "-c", dev, "examples/step.sg"])
    check("...but a dry run of such a configuration still works", r.returncode == 0, r.stderr)


# --------------------------------------------------------------------------
# 6. Real signals to the real program
# --------------------------------------------------------------------------
def read_until(proc, needle, timeout=20):
    """Read stderr lines until one contains needle; return everything read so far."""
    buf = ""
    end = time.time() + timeout
    fd = proc.stderr.fileno()
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            chunk = os.read(fd, 4096).decode("utf-8", "replace")
            if not chunk:
                break
            buf += chunk
            if needle in buf:
                return buf
        if proc.poll() is not None and needle not in buf:
            break
    return buf


def test_signals(tmp):
    waiting = protocol(tmp, "wait.sg", "period 5s\nstart on trigger", repeat=3)

    for name, sig in (("SIGINT", signal.SIGINT), ("SIGTERM", signal.SIGTERM)):
        p = subprocess.Popen([LCG2] + A(["-c", CFG, "-r", "20kHz", waiting]), cwd=WORK, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        seen = read_until(p, "press Enter")
        check("%s test: the program is waiting for Enter" % name, "press Enter" in seen, seen)
        p.send_signal(sig)
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
        rest = p.stderr.read().decode("utf-8", "replace")
        out = p.stdout.read().decode()
        check("%s while waiting for the trigger: exit status 2, not killed by the signal" % name,
              p.returncode == 2, "rc=%s %s" % (p.returncode, seen + rest))
        check("%s: it says it aborted with the outputs at rest" % name,
              "aborted after 0 of 3 trial(s): outputs at rest" in seen + rest, seen + rest)
        check("%s: nothing on stdout" % name, out == "")
        p.stdin.close()

    # after a REAL signal, the outputs are really at rest: measured, not just announced.
    cfg = write(tmp, "restsig.ini", open(os.path.join(ROOT, CFG)).read().replace("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 100"))
    rp = protocol(tmp, "restsig.sg", "period 5s\nstart on trigger", repeat=3, rest=50)
    p = subprocess.Popen([LCG2, "-v", "-c", cfg, "-r", "20kHz", rp], cwd=WORK, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seen = read_until(p, "press Enter")
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    rest = p.stderr.read().decode("utf-8", "replace")
    check("after a real SIGINT the simulator's outputs are at the expected rest values (0.125 V, 0.25 V, 0)",
          p.returncode == 2 and "outputs left at ao0=0.125 V ao1=0.25 V port0/line0=0" in seen + rest,
          "rc=%s %s" % (p.returncode, seen + rest))
    p.stdin.close()

    # a second Ctrl-C must not skip the rest step
    p = subprocess.Popen([LCG2] + A(["-c", CFG, "-r", "20kHz", waiting]), cwd=WORK, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seen = read_until(p, "press Enter")
    p.send_signal(signal.SIGINT)
    p.send_signal(signal.SIGINT)
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    rest = p.stderr.read().decode("utf-8", "replace")
    check("repeated Ctrl-C: still exit status 2 and the outputs put at rest", p.returncode == 2 and
          "outputs at rest" in seen + rest, "rc=%s %s" % (p.returncode, seen + rest))
    p.stdin.close()

    # the end of the input while waiting for Enter (nobody can ever press it)
    p = subprocess.run([LCG2] + A(["-c", CFG, "-r", "20kHz", waiting]), cwd=WORK, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=30)
    check("the input ends while waiting for Enter: the trial is not started, exit status 2",
          p.returncode == 2 and "aborted after 0 of 3" in p.stderr, "rc=%s %s" % (p.returncode, p.stderr))

    # mid-trial, in real time (loose bounds)
    p = subprocess.Popen([LCG2, "-R"] + A(["-c", CFG, "examples/step.sg"]), cwd=WORK, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    seen = read_until(p, "in real time")
    time.sleep(0.6)
    t0 = time.time()
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    took = time.time() - t0
    rest = p.stderr.read().decode("utf-8", "replace")
    check("Ctrl-C in the middle of a real-time trial: exit status 2 within a fraction of a second",
          p.returncode == 2 and took < 1.5, "rc=%s took=%.2f %s" % (p.returncode, took, seen + rest))
    check("...the trial is reported INCOMPLETE and the outputs put at rest",
          "INCOMPLETE" in seen + rest and "outputs at rest" in seen + rest, seen + rest)

    # real time, a smoke test: three trials of 0.2 s, 0.5 s apart: about 1.2 s
    fast = protocol(tmp, "fast.sg", "period 0.5s", repeat=3, dur="0.2s")
    t0 = time.time()
    r = lcg2(["-R", "-c", CFG, "-r", "20kHz", fast])
    took = time.time() - t0
    check("real time: it takes about as long as the protocol says (1.2 s)", r.returncode == 0 and 1.0 < took < 5.0,
          "rc=%s took=%.2f %s" % (r.returncode, took, r.stderr))
    check("real time: the progress is not labelled simulated", "(simulated" not in r.stderr and
          "in real time" in r.stderr, r.stderr)
    check("real time: no data on stdout", r.stdout == "")


# --------------------------------------------------------------------------
def main():
    for tool in (ET, LCG2):
        if not os.path.exists(tool):
            print("missing %s: run `make` first" % os.path.relpath(tool, ROOT))
            return 1
    tmp = tempfile.mkdtemp()
    try:
        for fn in (test_timing, test_trigger, test_rest, test_loopback, test_program, test_signals):
            sub = os.path.join(tmp, fn.__name__)
            os.makedirs(sub)
            fn(sub)
    finally:
        shutil.rmtree(tmp)
        shutil.rmtree(WORK, ignore_errors=True)
    print("engine tests: %d checks, %d failed" % (count, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
