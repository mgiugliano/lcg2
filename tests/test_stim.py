#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M2 acceptance test: StimGen integration.
#
#     python3 tests/test_stim.py          (after `make`, which builds the tools)
#
# What it proves, with the tools built by `make tools`:
#   * the samples LCG2 decodes equal, bit for bit, what StimGen's own `sg render`
#     writes for the same text, rate and seed (plain stimulus, protocol, plain
#     waveform, -e text, other units);
#   * trial order and seeds equal `sg expand`;
#   * rendering twice, with no seed given, gives the same samples (the seeds are
#     fixed after the first render);
#   * the rate rules of SPEC 4.1 in every combination;
#   * StimGen's messages are translated (no "editor", no "--rate");
#   * channel mapping, unit checks, and the clipping check refuse before
#     anything is played, naming trial, channel and sample;
#   * the volts and the rest samples that the backend will receive;
#   * the JSON reader agrees with an independent parser, on real StimGen output
#     and on a table of hostile inputs, and survives mutation.
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# LCG2_BUILD and LCG2_BIN point the test at other builds (for example one made
# with -fsanitize=address,undefined); by default it uses `make`'s.
BUILD = os.environ.get("LCG2_BUILD", os.path.join(ROOT, "build"))
LCG2 = os.environ.get("LCG2_BIN", os.path.join(ROOT, "lcg2"))
SG = os.path.join(BUILD, "sg")
DUMP = os.path.join(BUILD, "stimdump")
JSONTOOL = os.path.join(BUILD, "jsontool")
CFG = "examples/lcg2.ini"          # two pipettes P1 (limit 5 V), P2, and a digital line TTL

failures = []
count = 0


def check(name, ok, detail=""):
    global count
    count += 1
    if not ok:
        failures.append(name)
        print("FAIL  %s" % name)
        if detail:
            for line in str(detail).splitlines()[:8]:
                print("      " + line)


def run(cmd, **kw):
    kw.setdefault("cwd", ROOT)
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=120, **kw)


def rows(path):
    """The data rows of a text dump (no header), as strings."""
    with open(path) as f:
        return [l for l in f if not l.startswith("#")]


def write(tmp, name, text):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write(text)
    return p


def lcg2(args, cfg=CFG):
    return run([LCG2, "-c", cfg, "-n"] + args)


# --------------------------------------------------------------------------
# 1. Samples equal sg render, bit for bit
# --------------------------------------------------------------------------
def test_examples_are_valid(tmp):
    """Every example stimulus is valid StimGen text (and the wrong ones are wrong on purpose)."""
    for name in sorted(os.listdir(os.path.join(ROOT, "examples"))):
        if name.endswith(".sg"):
            r = run([SG, "check", os.path.join("examples", name)])
            check("sg check accepts examples/" + name, r.returncode == 0, r.stderr)
    for name, stim, mode in (("step.sg", "cc", "cc"), ("step_vc.sg", "vc", "vc"), ("pair.sg", "cc", "cc"),
                             ("camera.sg", "cc", "cc"), ("steps_protocol.sg", "cc", "cc"),
                             ("frozen_noise.sg", "cc", "cc")):
        r = lcg2(["-m", mode, "examples/" + name], "examples/lcg2.ini" if name != "step_vc.sg" else
                 "examples/single_pipette.ini")
        check("lcg2 accepts examples/%s in %s mode" % (name, mode), r.returncode == 0, r.stderr)


def test_equal_to_sg(tmp):
    plain = [
        ("step.sg", ["examples/step.sg"], ["examples/step.sg"]),
        ("pair.sg", ["examples/pair.sg"], ["examples/pair.sg"]),
        ("camera.sg (ou noise)", ["examples/camera.sg"], ["examples/camera.sg"]),
        ("-r given, stated rate agrees", ["-r", "20kHz", "examples/step.sg"],
         ["-r", "20kHz", "examples/step.sg"]),
    ]
    for name, mine, theirs in plain:
        d1, d2 = os.path.join(tmp, "m"), os.path.join(tmp, "s")
        os.makedirs(d1, exist_ok=True); os.makedirs(d2, exist_ok=True)
        r = run([DUMP, "samples", d1, "-c", CFG, "-s", "7"] + mine)
        s = run([SG, "render", "--text", "-s", "7"] + theirs + ["-o", os.path.join(d2, "x.txt")])
        ok = r.returncode == 0 and s.returncode == 0 and \
            rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "x.txt"))
        check("equals sg render: " + name, ok, r.stderr + s.stderr)

    # a plain waveform: sg gets the unit that LCG2 gives it (the unit of the output)
    d1, d2 = os.path.join(tmp, "m"), os.path.join(tmp, "s")
    r = run([DUMP, "samples", d1, "-c", CFG, "-s", "7", "-T", "P2", "-r", "20kHz", "examples/ramp.sg"])
    s = run([SG, "render", "--text", "-s", "7", "-r", "20kHz", "-u", "pA", "examples/ramp.sg",
             "-o", os.path.join(d2, "w.txt")])
    check("equals sg render: plain waveform to P2 with -T",
          r.returncode == 0 and s.returncode == 0 and
          rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "w.txt")), r.stderr + s.stderr)

    # -e text (several lines)
    text = "sg 2 stimulus\nrate 10kHz\nchannel P1 unit=pA { 100ms dc(5)\n 200ms ou(mean=0, sd=20, tau=5ms) }"
    r = run([DUMP, "samples", d1, "-c", CFG, "-s", "9", "-e", text])
    s = run([SG, "render", "--text", "-s", "9", "-e", text, "-o", os.path.join(d2, "e.txt")])
    check("equals sg render: -e text with noise",
          r.returncode == 0 and s.returncode == 0 and
          rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "e.txt")), r.stderr + s.stderr)

    # a stimulus in nA stays in nA at this stage (the factor is applied later)
    p = write(tmp, "na.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=nA { 500ms dc(0.2) }\n")
    r = run([DUMP, "samples", d1, "-c", CFG, "-s", "1", p])
    s = run([SG, "render", "--text", "-s", "1", p, "-o", os.path.join(d2, "na.txt")])
    check("equals sg render: unit nA", r.returncode == 0 and s.returncode == 0 and
          rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "na.txt")), r.stderr + s.stderr)

    # protocols: every trial
    for proto in ("examples/steps_protocol.sg", "examples/frozen_noise.sg"):
        m = os.path.join(tmp, "pm"); t = os.path.join(tmp, "ps")
        os.makedirs(m, exist_ok=True); os.makedirs(t, exist_ok=True)
        r = run([DUMP, "samples", m, "-c", CFG, "-s", "7", "-r", "20kHz", proto])
        s = run([SG, "render", "--text", "-s", "7", "-r", "20kHz", proto, "-o", t])
        names_m = sorted(os.listdir(m)); names_s = sorted(os.listdir(t))
        ok = r.returncode == 0 and s.returncode == 0 and names_m == names_s and names_m
        bad = [n for n in names_m if ok and rows(os.path.join(m, n)) != rows(os.path.join(t, n))]
        check("equals sg render: every trial of %s (%d trials)" % (os.path.basename(proto), len(names_m)),
              ok and not bad, r.stderr + s.stderr + " differing: %s" % bad[:3])
        shutil.rmtree(m); shutil.rmtree(t)


# --------------------------------------------------------------------------
# 2. Same samples, every time: the seeds are fixed after the first render
# --------------------------------------------------------------------------
def test_determinism(tmp):
    for f, extra in (("examples/camera.sg", []), ("examples/pair.sg", []),
                     ("examples/frozen_noise.sg", ["-r", "20kHz"]),
                     ("examples/steps_protocol.sg", ["-r", "20kHz"])):
        r = run([DUMP, "twice", tmp, "-c", CFG] + extra + [f])
        words = [l.split()[1] for l in r.stdout.splitlines()]
        check("two renders without a seed are identical: " + os.path.basename(f),
              r.returncode == 0 and words and all(w == "same" for w in words), r.stdout + r.stderr)

    # the seed that was drawn is a real seed: -s with it reproduces the samples
    d1, d2 = os.path.join(tmp, "d1"), os.path.join(tmp, "d2")
    os.makedirs(d1, exist_ok=True); os.makedirs(d2, exist_ok=True)
    r = run([DUMP, "samples", d1, "-c", CFG, "examples/camera.sg"])
    seed = lcg2(["examples/camera.sg"]).stdout.split("seed           ")[1].split()[0]
    r2 = run([DUMP, "samples", d2, "-c", CFG, "-s", seed, "examples/camera.sg"])
    # (a different draw is fine: only the seed-given run must equal sg with that seed)
    s = run([SG, "render", "--text", "-s", seed, "examples/camera.sg", "-o", os.path.join(d1, "ref.txt")])
    check("a drawn seed can be given back with -s",
          r.returncode == 0 and r2.returncode == 0 and s.returncode == 0 and
          rows(os.path.join(d2, "stimulus.txt")) == rows(os.path.join(d1, "ref.txt")), r.stderr + r2.stderr)
    check("without -s two runs draw different seeds",
          lcg2(["examples/camera.sg"]).stdout.split("seed           ")[1].split()[0] != seed)


# --------------------------------------------------------------------------
# 2b. Where the seed came from (the plan must not mislead)
# --------------------------------------------------------------------------
def test_seed_source(tmp):
    def seed_line(r):
        for l in r.stdout.splitlines():
            if l.startswith("  seed"):
                return l
        return ""

    drawn = "drawn for this check; a run draws its own unless you give -s"
    stated = write(tmp, "seeded.sg", "sg 2 stimulus\nrate 20kHz\nseed 42\n"
                   "channel P1 unit=pA { 1s ou(mean=0, sd=1, tau=5ms) }\n")
    r = lcg2(["examples/camera.sg"])
    check("plan: a drawn seed is labelled as drawn for the check", r.returncode == 0 and drawn in seed_line(r),
          seed_line(r))
    r = lcg2(["-s", "3", "examples/camera.sg"])
    check("plan: a seed from -s says so", "seed           3 (from -s)" in seed_line(r), seed_line(r))
    r = lcg2([stated])
    check("plan: a seed stated by the stimulus is not called drawn",
          "seed           42 (stated by the stimulus)" in seed_line(r), seed_line(r))
    r = lcg2(["-s", "5", stated])
    check("plan: -s beats the seed stated by the stimulus", "seed           5 (from -s)" in seed_line(r),
          seed_line(r))
    # and it is really -s that is used: equal to sg with -s 5
    d1, d2 = os.path.join(tmp, "a"), os.path.join(tmp, "b")
    os.makedirs(d1); os.makedirs(d2)
    r1 = run([DUMP, "samples", d1, "-c", CFG, "-s", "5", stated])
    r2 = run([SG, "render", "--text", "-s", "5", stated, "-o", os.path.join(d2, "x.txt")])
    check("-s beats the seed stated by the stimulus, as in sg", r1.returncode == 0 and r2.returncode == 0 and
          rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "x.txt")), r1.stderr + r2.stderr)
    r1 = run([DUMP, "samples", d1, "-c", CFG, stated])
    r2 = run([SG, "render", "--text", stated, "-o", os.path.join(d2, "y.txt")])
    check("a stated seed gives the same samples as sg (no -s)", r1.returncode == 0 and r2.returncode == 0 and
          rows(os.path.join(d1, "stimulus.txt")) == rows(os.path.join(d2, "y.txt")), r1.stderr + r2.stderr)

    pst = write(tmp, "pseed.sg", "sg 2 protocol\nseed 11\nrepeat 2\nstimulus {\n channel P1 unit=pA { 1s dc(1) }\n}\n")
    r = lcg2(["-r", "20kHz", pst])
    check("plan: a protocol seed stated by the protocol", "protocol seed 11 (stated by the stimulus)" in seed_line(r),
          seed_line(r))
    r = lcg2(["-r", "20kHz", "-s", "9", pst])
    check("plan: -s beats a stated protocol seed", "protocol seed 9 (from -s)" in seed_line(r), seed_line(r))
    r = lcg2(["-r", "20kHz", "examples/steps_protocol.sg"])
    check("plan: a drawn protocol seed is labelled as drawn", drawn in seed_line(r) and "protocol seed" in seed_line(r),
          seed_line(r))


# --------------------------------------------------------------------------
# 3. Trial order and seeds equal sg expand
# --------------------------------------------------------------------------
def test_expand(tmp):
    for proto in ("examples/steps_protocol.sg", "examples/frozen_noise.sg"):
        d = os.path.join(tmp, "exp_" + os.path.basename(proto))
        s = run([SG, "expand", "-s", "7", proto, d])
        sgi = open(os.path.join(d, "protocol.sgi")).read().splitlines()
        want = [(l.split()[0][:-3], l.split("seed=")[1].split()[0]) for l in sgi if ".sg " in l and "seed=" in l]
        timing = [l for l in sgi if l.startswith(("period", "gap"))]
        start = [l for l in sgi if l.startswith("start")]
        r = run([DUMP, "list", tmp, "-c", CFG, "-s", "7", "-r", "20kHz", proto])
        got = [(l.split()[0], l.split()[1]) for l in r.stdout.splitlines()]
        check("trial order and seeds equal sg expand: " + os.path.basename(proto),
              s.returncode == 0 and r.returncode == 0 and got == want and len(want) > 1,
              "%s\n%s\n%s" % (want[:2], got[:2], r.stderr))
        if r.stdout:
            first = r.stdout.splitlines()[0]
            check("the timing line of the protocol is passed on: " + os.path.basename(proto),
                  timing and start and ("%s | %s" % (timing[0], start[0].replace("start ", ""))) in first,
                  first + " / " + str(timing) + str(start))


# --------------------------------------------------------------------------
# 4. The rate rules (SPEC 4.1)
# --------------------------------------------------------------------------
def test_rate(tmp):
    nocfg_rate = write(tmp, "norate.ini", open(os.path.join(ROOT, CFG)).read().replace("rate = 20kHz\n", ""))
    cfg10 = write(tmp, "rate10.ini", open(os.path.join(ROOT, CFG)).read().replace("rate = 20kHz", "rate = 10kHz"))
    nostated = write(tmp, "nostated.sg", "sg 2 stimulus\nchannel P1 unit=pA { 1s dc(1) }\n")
    stated = "examples/step.sg"            # states 20 kHz

    def rate_line(r):
        for l in r.stdout.splitlines():
            if l.startswith("  rate"):
                return l
        return ""

    r = lcg2([stated], cfg10)
    check("stated rate beats the rate of the configuration", r.returncode == 0 and
          "20 kHz (stated by the stimulus)" in rate_line(r), r.stderr + rate_line(r))
    r = lcg2(["-r", "20kHz", stated], cfg10)
    check("-r that agrees with the stated rate is fine", r.returncode == 0 and
          "20 kHz (from -r)" in rate_line(r), r.stderr + rate_line(r))
    r = lcg2(["-r", "10kHz", stated], cfg10)
    check("-r that differs from the stated rate is an error", r.returncode == 1 and
          "-r 10kHz differs from the rate stated in the stimulus" in r.stderr and "--rate" not in r.stderr,
          r.stderr)
    r = lcg2(["-r", "20000", stated])
    check("-r 20000 equals a stated 20 kHz", r.returncode == 0, r.stderr)
    r = lcg2(["-r", "0.02MHz", stated])
    check("-r 0.02MHz equals a stated 20 kHz", r.returncode == 0, r.stderr)
    r = lcg2(["-r", "5kHz", nostated], cfg10)
    check("without a stated rate, -r is used", r.returncode == 0 and "5 kHz (from -r)" in rate_line(r),
          r.stderr + rate_line(r))
    r = lcg2([nostated], cfg10)
    check("without a stated rate and without -r, the rate of the configuration is used",
          r.returncode == 0 and "10 kHz (from the configuration" in rate_line(r), r.stderr + rate_line(r))
    r = lcg2([nostated], nocfg_rate)
    check("no rate anywhere is an error that says what to do", r.returncode == 1 and
          "no sampling rate" in r.stderr and "-r RATE" in r.stderr and "[rig]" in r.stderr, r.stderr)
    r = lcg2(["-r", "400kHz", nostated])
    check("the rate of a stimulus must be possible on the board", r.returncode == 1 and "500 kS/s" in r.stderr,
          r.stderr)
    p = write(tmp, "fast.sg", "sg 2 stimulus\nrate 300kHz\nchannel P1 unit=pA { 10ms dc(1) }\n")
    r = lcg2([p])
    check("a stated rate above the board's limit is refused too", r.returncode == 1 and "500 kS/s" in r.stderr,
          r.stderr)
    r = lcg2(["-r", "2.5Hz", nostated])
    check("a fractional rate is exact", r.returncode == 0 and "2.5 Hz" in rate_line(r), r.stderr + rate_line(r))
    r = lcg2(["-r", "20kHz", "examples/steps_protocol.sg"])
    check("protocol: -r gives the rate of every trial", r.returncode == 0 and "20 kHz (from -r)" in rate_line(r),
          r.stderr)
    r = lcg2(["examples/steps_protocol.sg"])
    check("protocol: the configuration's rate is the default", r.returncode == 0 and
          "from the configuration" in rate_line(r), r.stderr + rate_line(r))


# --------------------------------------------------------------------------
# 5. Messages name the user's own file, in LCG2's words
# --------------------------------------------------------------------------
def test_messages(tmp):
    bad = write(tmp, "bad.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA {\n  1s bogus(1)\n}\n")
    r = lcg2([bad])
    check("a syntax error names the file and the line", r.returncode == 1 and
          r.stderr.startswith("lcg2: %s:4: " % bad) and "editor" not in r.stderr, r.stderr)
    r = lcg2(["-e", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s bogus(1) }"])
    check("a syntax error in -e names the -e text", r.returncode == 1 and
          r.stderr.startswith("lcg2: the -e text:") and "editor" not in r.stderr, r.stderr)
    # a marker is kept with its full name (channel.name) in 64 bytes: a longer one is refused, not emptied
    for n, ok in ((60, True), (61, False)):
        r = lcg2(["-e", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 5ms dc(0)\n@%s\n5ms dc(1) }" % ("m" * n)])
        check("a marker named with %d characters (P1.%s...) is %s" % (n + 3, "m" * 3, "kept" if ok else "refused"),
              (r.returncode == 0) if ok else (r.returncode == 1 and "named with more than 63 characters" in r.stderr),
              r.stderr[-300:])
    r = lcg2(["/nonexistent/x.sg"])
    check("a missing stimulus file is reported", r.returncode == 1 and "cannot open stimulus file" in r.stderr,
          r.stderr)
    d = os.path.join(tmp, "adir"); os.makedirs(d, exist_ok=True)
    nul = os.path.join(tmp, "nul.sg")
    with open(nul, "wb") as f:
        f.write(b"sg 2 stimulus\x00\nrate 20kHz\n")
    r = lcg2([nul])
    check("a file with a NUL byte is refused", r.returncode == 1 and "NUL" in r.stderr, r.stderr)
    # a protocol error names the trial, and says that its line number counts the
    # trial's own text (the protocol file has the mistake on another line)
    p = write(tmp, "protobad.sg",
              "sg 2 protocol\n# a comment\nsweep amp = [1pA, 2pA]\n\nstimulus {\n channel P1 unit=pA {\n"
              " 1s dc($amp)\n 1s bogus(1)\n }\n}\n")
    r = lcg2(["-r", "20kHz", p])
    check("an error in a protocol names the trial and whose line number it is",
          r.returncode == 1 and r.stderr.startswith("lcg2: %s (trial 0000, line " % p) and
          "of that trial's text): unknown generator" in r.stderr, r.stderr)
    # an error in the protocol itself keeps the plain file:line form
    p = write(tmp, "protobad2.sg", "sg 2 protocol\nsweep amp = from 1pA to\n")
    r = lcg2(["-r", "20kHz", p])
    check("an error in the protocol text itself is file:line", r.returncode == 1 and
          r.stderr.startswith("lcg2: %s:" % p) and "trial" not in r.stderr, r.stderr)


# --------------------------------------------------------------------------
# 6. Kinds, -T, channel mapping, units
# --------------------------------------------------------------------------
def test_mapping(tmp):
    r = lcg2(["-r", "20kHz", "examples/ramp.sg"])
    check("a plain waveform without -T is an error", r.returncode == 1 and "-T NAME" in r.stderr, r.stderr)
    r = lcg2(["-T", "P2", "examples/step.sg"])
    check("-T with a stimulus is an error, not ignored", r.returncode == 1 and "only for a plain waveform" in r.stderr,
          r.stderr)
    r = lcg2(["-T", "P2", "-r", "20kHz", "examples/steps_protocol.sg"])
    check("-T with a protocol of stimuli is an error", r.returncode == 1 and "only for a plain waveform" in r.stderr,
          r.stderr)
    r = lcg2(["-T", "P2", "-r", "20kHz", "examples/ramp.sg"])
    check("a plain waveform goes to the pipette named by -T", r.returncode == 0 and
          "waveform (pA)  ->  [out P2] on ao1" in r.stdout, r.stderr + r.stdout)
    r = lcg2(["-T", "P1", "-r", "20kHz", "-m", "vc", "-e", "500ms dc(0); 100ms dc(50)"])
    check("a plain waveform takes the unit of the output in this mode", r.returncode == 0 and
          "waveform (mV)  ->  [out P1] on ao0, peak 2.5 V" in r.stdout, r.stderr + r.stdout)
    r = lcg2(["-T", "P1", "-r", "20kHz", "-m", "vc", "examples/ramp.sg"])
    check("...and 300 mV at 20 mV per volt is refused (15 V, above the limit)", r.returncode == 1 and
          "channel out" in r.stderr and "300 mV needs 15 V" in r.stderr, r.stderr)

    def stim(name, text):
        return write(tmp, name, "sg 2 stimulus\nrate 20kHz\n" + text + "\n")

    r = lcg2([stim("p9.sg", "channel P9 unit=pA { 1s dc(1) }")])
    check("a channel with no [out] is an error that lists the outputs", r.returncode == 1 and
          "[out P9]" in r.stderr and "outputs in use: P1, P2, TTL" in r.stderr, r.stderr)
    r = lcg2(["-P", "single", stim("p2.sg", "channel P2 unit=pA { 1s dc(1) }")])
    check("a channel whose [out] is skipped is an error", r.returncode == 1 and "is skipped" in r.stderr, r.stderr)
    r = lcg2([stim("d.sg", "digital P1 { 1s dc(1) }")])
    check("a digital channel on an analog output is an error", r.returncode == 1 and
          "digital in the stimulus" in r.stderr and "analog" in r.stderr, r.stderr)
    r = lcg2([stim("a.sg", "channel TTL unit=pA { 1s dc(1) }")])
    check("an analog channel on a digital line is an error", r.returncode == 1 and
          "analog in the stimulus" in r.stderr and "digital line" in r.stderr, r.stderr)
    r = lcg2([stim("ns.sg", "channel P1 unit=nS { 1s dc(1) }")])
    check("a unit that is neither current nor voltage is an error", r.returncode == 1 and
          "not a current" in r.stderr, r.stderr)
    r = lcg2(["-m", "vc", "examples/step.sg"])
    check("pA against a voltage clamp output is an error that suggests the mode", r.returncode == 1 and
          "a current" in r.stderr and "carries a voltage in vc mode" in r.stderr and "-m cc" in r.stderr, r.stderr)
    r = lcg2([stim("mv.sg", "channel P1 unit=mV { 1s dc(100) }")])
    check("mV against a current clamp output is an error", r.returncode == 1 and "a voltage" in r.stderr
          and "carries a current in cc mode" in r.stderr and "-m vc" in r.stderr, r.stderr)
    r = lcg2(["-m", "vc", stim("mv2.sg", "channel P1 unit=mV { 1s dc(100) }")])
    check("mV against a voltage clamp output is fine", r.returncode == 0, r.stderr)
    r = lcg2([stim("na.sg", "channel P1 unit=nA { 500ms dc(0.2) }")])
    check("nA against pA is fine and its size is counted in volts (0.2 nA = 0.5 V)", r.returncode == 0 and
          "peak 0.5 V" in r.stdout, r.stderr + r.stdout)
    r = lcg2([stim("uv.sg", "channel P1 unit=uV { 500ms dc(100000) }")])
    check("uV against a voltage clamp is refused in cc (a current output)", r.returncode == 1, r.stderr)

    # two channels, one idle output, one digital
    r = lcg2(["examples/camera.sg"])
    check("channels, the idle output and the digital line are reported", r.returncode == 0 and
          "P1 (pA)" in r.stdout and "TTL (digital)" in r.stdout and "idle           P2" in r.stdout, r.stdout + r.stderr)

    # the first trial's channels are mapped; every trial is checked, in order
    p = write(tmp, "kinds.sg",
              "sg 2 protocol\nsweep amp = [10pA, 20pA]\nstimulus {\n channel P1 unit=pA { 1s dc($amp) }\n}\n")
    r = lcg2(["-r", "20kHz", p])
    check("a protocol of two trials is described", r.returncode == 0 and "a protocol of 2 trials" in r.stdout and
          "2 trial(s) rendered and checked" in r.stdout, r.stdout + r.stderr)


# --------------------------------------------------------------------------
# 7. The clipping check
# --------------------------------------------------------------------------
def cfg_with(tmp, name, old, new):
    text = open(os.path.join(ROOT, CFG)).read()
    assert old in text, old
    return write(tmp, name, text.replace(old, new, 1))


def test_clipping(tmp):
    # pair.sg pushes P1 to 2000 pA = exactly 5 V, the limit of P1 in the example
    r = lcg2(["examples/pair.sg"])
    check("a sample exactly at the limit is allowed", r.returncode == 0 and "peak 5 V of the limit 5 V" in r.stdout,
          r.stderr + r.stdout)
    c = cfg_with(tmp, "limit49.ini", "limit = 5", "limit = 4.9")
    r = lcg2(["examples/pair.sg"], c)
    check("a sample above the limit is refused, before the plan", r.returncode == 1 and r.stdout == "" and
          "the stimulus, channel P1: sample 20000 (1 s): 2000 pA needs 5 V at the amplifier command, "
          "above the limit of 4.9 V" in r.stderr, r.stderr)
    check("the refusal says how many samples clip", "clip in all" in r.stderr and "of 50000 samples" in r.stderr,
          r.stderr)
    # a run (without -n) refuses in the same way and never reaches "not implemented"
    r = run([LCG2, "-c", c, os.path.join(ROOT, "examples/pair.sg")], cwd=tmp)      # a real run: not in the repository
    check("a run without -n refuses a clipping stimulus too", r.returncode == 1 and "above the limit" in r.stderr
          and "not implemented" not in r.stderr, r.stderr)
    # the board range, with no limit: P2 has none
    p = write(tmp, "big.sg", "sg 2 stimulus\nrate 20kHz\nchannel P2 unit=pA { 100ms dc(4000)\n 100ms dc(4001) }\n")
    r = lcg2([p])
    check("above the board's 10 V is refused with no limit set", r.returncode == 1 and
          "channel P2: sample 2000" in r.stderr and "above its range of 10 V" in r.stderr, r.stderr)
    p = write(tmp, "edge.sg", "sg 2 stimulus\nrate 20kHz\nchannel P2 unit=pA { 100ms dc(4000)\n 100ms dc(-4000) }\n")
    r = lcg2([p])
    check("exactly +10 V and -10 V are allowed", r.returncode == 0, r.stderr)
    # the calibration is part of the check
    c = cfg_with(tmp, "cal.ini", "[out P2]\nline = ao1", "[out P2]\nline = ao1\ncal_scale = 0.99")
    r = lcg2([p], c)
    check("the calibration scale is part of the range check", r.returncode == 1 and "after calibration" in r.stderr,
          r.stderr)
    # a rest value of the stimulus is checked too
    p = write(tmp, "rest.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA rest=2100 { 100ms dc(0) }\n")
    r = lcg2([p])
    check("the stimulus's own rest value is checked", r.returncode == 1 and "the rest value 2100 pA" in r.stderr,
          r.stderr)
    # a protocol: the error names the trial
    p = write(tmp, "pc.sg", "sg 2 protocol\nsweep amp = [100pA, 300pA, 2500pA]\norder sequential\n"
              "stimulus {\n channel P1 unit=pA { 200ms dc($amp) }\n}\n")
    r = lcg2(["-r", "20kHz", p])
    check("a protocol: the error names the trial that clips", r.returncode == 1 and
          "trial 0002, channel P1: sample 0" in r.stderr and "above the limit of 5 V" in r.stderr, r.stderr)
    # a clipping trial refuses the whole run, even when the earlier trials are fine
    check("...and nothing was printed on stdout", r.stdout == "")
    # a warning is passed on, and -q silences it
    w = write(tmp, "warn.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) }\n"
              "channel P2 unit=pA { 500ms dc(1) }\n")
    r = lcg2([w])
    check("a StimGen warning is printed", r.returncode == 0 and "lcg2: warning:" in r.stderr and
          "different durations" in r.stderr, r.stderr)
    check("...on stderr only: stdout is the plan and nothing else",
          r.stdout.startswith("LCG2 0.4.0, dry run") and "lcg2: warning" not in r.stdout, r.stdout[:200])
    r = lcg2(["-q", w])
    check("-q silences warnings", r.returncode == 0 and "warning" not in r.stderr, r.stderr)
    # a valid stimulus: a real run does all the checks first, then plays on the simulator (M3)
    r = run([LCG2, "-c", os.path.join(ROOT, CFG), os.path.join(ROOT, "examples/step.sg")], cwd=tmp)
    check("a run without -n plays and writes the data file and the run folder (M4)",
          r.returncode == 0 and "wrote " in r.stderr and ".h5 and ." in r.stderr, r.stderr)
    # record only reads no stimulus at all
    r = lcg2(["-t", "30s"])
    check("-t reads no stimulus", r.returncode == 0 and "stimulus   " not in r.stdout, r.stderr)


# --------------------------------------------------------------------------
# 8. The signals for the backend: volts and rest samples
# --------------------------------------------------------------------------
def test_volts(tmp):
    base = open(os.path.join(ROOT, CFG)).read()
    cfg = write(tmp, "volts.ini", base.replace("[out P1]\nline = ao0", "[out P1]\nline = ao0\ncal_scale = 1.002\ncal_offset = 0.01")
                .replace("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 100"))
    d = os.path.join(tmp, "volts"); os.makedirs(d, exist_ok=True)
    r = run([DUMP, "volts", d, "-c", cfg, "examples/step.sg"])
    check("volts dump runs", r.returncode == 0, r.stderr)
    if r.returncode != 0:
        return
    lines = open(os.path.join(d, "stimulus.txt")).read().splitlines()
    head = lines[0]
    data = [l.split("\t") for l in lines[1:]]
    check("volts: header lists the outputs in use (analog then digital)",
          head == "# n=40000 pad=2\tP1(V)\tP2(V)\tTTL(0/1)", head)
    check("volts: n samples plus the rest samples", len(data) == 40002, len(data))
    # the same arithmetic as C, in the same order: wanted = value/gain, written = (wanted - off)/scale
    rest0 = (0.0 / 400.0 - 0.01) / 1.002
    step = (200.0 / 400.0 - 0.01) / 1.002
    p1 = [float(x[1]) for x in data]
    check("volts: before the step, P1 holds the inverse-calibrated rest", all(v == rest0 for v in p1[:10000]), p1[:2])
    check("volts: during the step, 200 pA = 0.5 V wanted = (0.5 - 0.01)/1.002 written",
          all(v == step for v in p1[10000:30000]), (p1[10000], step))
    check("volts: after the step, and in the padding, P1 is at rest again",
          all(v == rest0 for v in p1[30000:]) and p1[-1] == rest0 and p1[-2] == rest0, p1[-3:])
    p2 = set(float(x[2]) for x in data)
    check("volts: P2 is idle at the rest of the configuration (100 pA = 0.25 V)", p2 == {0.25}, p2)
    check("volts: the digital line is idle at 0", set(x[3] for x in data) == {"0"})

    # digital line: the stimulus, then its rest in the padding
    d2 = os.path.join(tmp, "volts2"); os.makedirs(d2, exist_ok=True)
    r = run([DUMP, "volts", d2, "-c", CFG, "-s", "7", "examples/camera.sg"])
    s = os.path.join(tmp, "samples_cam"); os.makedirs(s, exist_ok=True)
    r2 = run([DUMP, "samples", s, "-c", CFG, "-s", "7", "examples/camera.sg"])
    v = [l.split("\t") for l in open(os.path.join(d2, "stimulus.txt")).read().splitlines()[1:]]
    x = [l.split("\t") for l in rows(os.path.join(s, "stimulus.txt"))]
    check("volts: the digital line carries the stimulus's 0 and 1, then its rest",
          r.returncode == 0 and r2.returncode == 0 and len(v) == len(x) + 2 and
          all(v[i][3] == str(int(float(x[i][2]))) for i in range(len(x))) and v[-1][3] == "0" and v[-2][3] == "0",
          r.stderr + r2.stderr)
    check("volts: the idle analog output rests at 0 V", set(float(l[2]) for l in v) == {0.0})

    # voltage clamp: a stimulus in mV, gain 20 mV per volt
    p = write(tmp, "vc.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=mV { 100ms dc(50) }\n")
    d3 = os.path.join(tmp, "volts3"); os.makedirs(d3, exist_ok=True)
    r = run([DUMP, "volts", d3, "-c", CFG, "-m", "vc", p])
    v = [l.split("\t") for l in open(os.path.join(d3, "stimulus.txt")).read().splitlines()[1:]]
    check("volts: voltage clamp, 50 mV at 20 mV per volt is 2.5 V", r.returncode == 0 and
          all(float(l[1]) == 2.5 for l in v[:2000]) and float(v[-1][1]) == 0.0, r.stderr)

    # nA stimulus on a pA output: the factor is 1000 (0.2 nA = 200 pA = 0.5 V)
    p = write(tmp, "na.sg", "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=nA { 100ms dc(0.2) }\n")
    r = run([DUMP, "volts", d3, "-c", CFG, p])
    v = [l.split("\t") for l in open(os.path.join(d3, "stimulus.txt")).read().splitlines()[1:]]
    check("volts: a stimulus in nA is rescaled to the output's pA (0.5 V)", r.returncode == 0 and
          abs(float(v[0][1]) - 0.5) < 1e-12, r.stderr)

    # plain waveform: the target gets it, the other output is idle
    d4 = os.path.join(tmp, "volts4"); os.makedirs(d4, exist_ok=True)
    r = run([DUMP, "volts", d4, "-c", CFG, "-T", "P2", "-r", "20kHz", "examples/ramp.sg"])
    v = [l.split("\t") for l in open(os.path.join(d4, "stimulus.txt")).read().splitlines()[1:]]
    check("volts: a plain waveform drives only the target (P2 gets 300 pA = 0.75 V, P1 idle)", r.returncode == 0 and
          float(v[15000][2]) == 0.75 and float(v[15000][1]) == 0.0, r.stderr)


# --------------------------------------------------------------------------
# 9. The JSON reader
# --------------------------------------------------------------------------
def jsontool(text):
    return subprocess.run([JSONTOOL], input=text.encode("utf-8", "surrogatepass"),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)


def test_json(tmp):
    good = [
        "null", "true", "false", "0", "-0", "1", "-12", "0.5", "1e5", "1E-3", "2.5e+2", "9007199254740992",
        '""', '"abc"', '"a\\"b\\\\c\\/d\\b\\f\\n\\r\\t"', '"\\u0041\\u00e9\\u20ac"', '"\\ud83d\\ude00"',
        '"é€\U0001F600"', "[]", "{}", "[1,2,3]", ' [ 1 , [ 2 , [ ] ] ] ', '{"a":1,"b":[true,null],"c":{"d":"e"}}',
        '{"a":1,"a":2}', '{"":""}', "\n\t [ ] \r\n",
        "[" * 64 + "]" * 64,
    ]
    for t in good:
        r = jsontool(t)
        ok = r.returncode == 0
        if ok:
            try:
                ok = json.loads(r.stdout.decode("utf-8")) == json.loads(t)
            except Exception as e:      # noqa
                ok = False
        check("json accepts and round-trips: %r" % t[:50], ok, r.stderr.decode() + r.stdout.decode()[:80])
    bad = [
        "", "   ", "nul", "tru", "True", "NaN", "Infinity", "-Infinity", "+1", "01", "00", "1.", ".5", "1e", "1e+",
        "-", "--1", "0x10", "1 2", "[1,]", "[,1]", "[1 2]", "{,}", '{"a"}', '{"a":}', '{"a":1,}', "{a:1}",
        "{'a':1}", '"abc', '"a\nb"', '"\\x41"', '"\\u00"', '"\\u00zz"', '"\\ud800"', '"\\udc00"', '"\\ud800\\u0041"',
        "[1]x", "[1] [2]", "/* c */ 1", "// c\n1", "1e999", "-1e999", "[" * 65 + "]" * 65, "[" * 5000,
        '{"a":[1,2', "\x00", "[\x00]",
    ]
    for t in bad:
        r = jsontool(t)
        check("json refuses: %r" % t[:50], r.returncode == 1 and b"JSON" in r.stderr,
              "rc=%d %s" % (r.returncode, r.stderr.decode()))

    # real StimGen output: the expanded trial list (with nested info) and the header of a .sgb file
    r = run([DUMP, "expand", tmp, "-c", CFG, "-s", "7", "-r", "20kHz", "examples/steps_protocol.sg"])
    check("real StimGen output: the expanded trial list round-trips", r.returncode == 0 and
          jsontool(r.stdout).returncode == 0 and
          json.loads(jsontool(r.stdout).stdout.decode()) == json.loads(r.stdout), r.stderr)
    sgb = os.path.join(tmp, "h.sgb")
    run([SG, "render", "-s", "7", "examples/camera.sg", "-o", sgb])
    b = open(sgb, "rb").read()
    L = int.from_bytes(b[8:16], "little")
    header = b[16:16 + L].decode("utf-8")
    r = jsontool(header)
    check("real StimGen output: an .sgb header round-trips", r.returncode == 0 and
          json.loads(r.stdout.decode()) == json.loads(header), r.stderr.decode())

    # mutation: the reader must never crash, and when it accepts, an independent parser agrees
    rnd = random.Random(5)
    expand = run([DUMP, "expand", tmp, "-c", CFG, "-s", "7", "-r", "20kHz", "examples/frozen_noise.sg"]).stdout
    seeds = [header, expand]
    bad_runs = 0
    for n in range(500):
        t = list(rnd.choice(seeds))
        for _ in range(rnd.randint(1, 6)):
            op = rnd.random()
            i = rnd.randrange(len(t) + 1)
            if op < .4 and t:
                del t[min(i, len(t) - 1)]
            elif op < .8:
                t.insert(i, rnd.choice('{}[]",:\\ \n01e-.tfnué\x00'))
            elif t:
                t[min(i, len(t) - 1)] = rnd.choice(t)
        s = "".join(t)
        res = jsontool(s)
        if res.returncode not in (0, 1):
            bad_runs += 1
            print("      mutation %d crashed with %d" % (n, res.returncode))
            continue
        if res.returncode == 0:
            try:
                same = json.loads(res.stdout.decode("utf-8")) == json.loads(s)
            except Exception:           # noqa
                same = False
            if not same:
                bad_runs += 1
                print("      mutation %d: accepted, but an independent parser disagrees: %r" % (n, s[:80]))
    check("json survives 500 mutations of real StimGen output", bad_runs == 0)


# --------------------------------------------------------------------------
def main():
    for tool in (SG, DUMP, JSONTOOL, LCG2):
        if not os.path.exists(tool):
            print("missing %s: run `make` first" % os.path.relpath(tool, ROOT))
            return 1
    tmp = tempfile.mkdtemp()
    try:
        for fn in (test_examples_are_valid, test_equal_to_sg, test_determinism, test_seed_source, test_expand, test_rate, test_messages, test_mapping,
                   test_clipping, test_volts, test_json):
            sub = os.path.join(tmp, fn.__name__)
            os.makedirs(sub)
            fn(sub)
    finally:
        shutil.rmtree(tmp)
    print("stimulus tests: %d checks, %d failed" % (count, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
