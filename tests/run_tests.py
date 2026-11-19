#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M0/M1 acceptance test: a table of good and bad command lines and
# configuration files, each with the exit status and the message expected.
#
#   python3 tests/run_tests.py ./lcg2
#
# Every case writes its configuration to a temporary folder, runs lcg2 with
# -n (dry run: nothing is played) and checks two things: the exit status, and
# a fragment of the message (stderr for errors, stdout for a good plan).
# A crash (a signal) is always a failure.  Add a case for every bug fixed.

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import neuronpy  # noqa: E402

LCG2 = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./lcg2")
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# A good configuration: a two-pipette rig with one digital line, and two
# profiles.  Bad cases are this text with one thing changed.
BASE = """\
[rig]
device = sim
mode = cc
rate = 20kHz

[out P1]
line = ao0
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV
limit = 5

[out P2]
line = ao1
gain_cc = 400
unit_cc = pA
gain_vc = 20
unit_vc = mV

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
gain_cc = 100
unit_cc = mV
gain_vc = 1000
unit_vc = pA

[profile single]
out.P2.skip = yes
in.P2.skip = yes

[profile swap]
out.P2.line = ao2
rig.rate = 10kHz
"""

# A dry run now reads the stimulus (M2), so the source of work must be real. The
# placeholders are replaced in run_case(): a stimulus on P1 in the unit of the
# amplifier mode in force (pA for cc, mV for vc), and a plain waveform for -T.
# The stimulus states no rate, so -r and the rate of the file decide.
STIM = ["-e", "@STIM@"]
WAVE = "@WAVE@"

cases = []                  # (name, config, args, rc, fragment, where)


def good(name, args, fragment="", cfg=None, where="out"):
    cases.append((name, BASE if cfg is None else cfg, args + ["-n"], 0, fragment, where))


def bad(name, args, fragment, cfg=None, rc=1):
    cases.append((name, BASE if cfg is None else cfg, args + ["-n"], rc, fragment, "err"))


def edit(old, new, text=BASE):
    """BASE with one piece of text replaced (the old text must be there)."""
    assert old in text, "test bug: %r not in the base configuration" % old
    return text.replace(old, new, 1)


def line_of(text, needle):
    for i, l in enumerate(text.splitlines(), 1):
        if needle in l:
            return i
    raise AssertionError("test bug: %r not in the text" % needle)


# --------------------------------------------------------------------------
# Good: the plan is printed, exit 0
# --------------------------------------------------------------------------
good("base", STIM, "device         sim")
good("mode vc", STIM + ["-m", "vc"], "mode           vc")
good("mode from file only", STIM, "mode           cc")
good("-m overrides the file", STIM + ["-m", "vc"], "20 mV per volt")
good("profile single", STIM + ["-P", "single"], "P2       skipped")
good("profile swap changes a line and the rate", STIM + ["-P", "swap"],
     "P2       -> ao2")
good("profile rate is shown", STIM + ["-P", "swap"], "10 kHz")
good("-r wins over the file", STIM + ["-r", "10kHz"], "10 kHz (from -r)")
good("rate: bare number is Hz", STIM + ["-r", "10000"], "10 kHz")
good("rate: 1e4", STIM + ["-r", "1e4"], "10 kHz")
good("rate: fractional", STIM + ["-r", "2.5Hz"], "2.5 Hz")
good("rate: 0.5kHz", STIM + ["-r", "0.5kHz"], "500 Hz")
good("rate: MHz is refused later by the board limit? no: 0.001MHz = 1kHz",
     STIM + ["-r", "0.001MHz"], "1 kHz")
good("-t record only", ["-t", "30s"], "record only, 30 s")
good("-t minutes", ["-t", "2min"], "record only, 120 s")
good("-t milliseconds", ["-t", "500ms"], "0.5 s")
good("-t fractional minutes", ["-t", "1.5min"], "record only, 90 s")
good("stimulus file", ["stim.sg"], "play the stimulus in stim.sg")
good("-e text", STIM, "stimulus text given with -e")
good("-T analog output", ["-e", WAVE, "-T", "P2"], "plain waveform to P2")
good("-s seed", STIM + ["-s", "42"], "seed           42")
good("-s seed zero", STIM + ["-s", "0"], "seed           0")
good("-p prefix", STIM + ["-p", "cell_3-a.1"], "file prefix    cell_3-a.1")
good("-v dumps the resolved configuration", STIM + ["-P", "swap", "-v"],
     "line = ao2")
good("-v shows that -m overrode the file", STIM + ["-m", "vc", "-v"],
     "mode = vc   ; set on the command line")
good("-v adds a mode the file lacks", STIM + ["-m", "vc", "-v"], "mode = vc",
     cfg=edit("mode = cc\n", ""))
good("-v names the profile", STIM + ["-P", "swap", "-v"], "; profile: swap")
good("-q is accepted", STIM + ["-q"], "")
good("digital line listed", STIM, "TTL      -> port0/line0 (digital, clocked)")
good("limit shown", STIM, "limit 5 V")
good("comments and blank lines", STIM, "device         sim",
     cfg=edit("[rig]\n", "; a comment\n# another\n\n[rig]\n"))
good("inline comment", STIM, "device         sim",
     cfg=edit("device = sim", "device = sim ; the simulator"))
good("default input range is 10 V", STIM, "range 10 V")
good("range 0.2", STIM, "range 0.2 V", cfg=edit("line = ai0", "line = ai0\nrange = 0.2"))
good("calibration keys", STIM, "P1",
     cfg=edit("limit = 5", "limit = 5\ncal_offset = 0.01\ncal_scale = 1.002"))
good("DIFF input", STIM, "DIFF",
     cfg=edit("line = ai0", "line = ai0\nterminal = DIFF"))
good("cell and synapse sections are accepted (and ignored) on device = sim, so one file serves both",
     STIM, "P1",
     cfg=edit("rate = 20kHz", "rate = 20kHz")
     + "\n[cell P1]\nmodel = hh_soma\n[cell P2]\nmodel = hh_soma\n[synapse P1_P2]\npre = P1\npost = P2\n")
good("daqmx device name", STIM, "Dev1", cfg=edit("device = sim", "device = Dev1"))
cases.append(("-h", BASE, ["-h"], 0, "usage: lcg2", "out"))   # no -n added

# --------------------------------------------------------------------------
# Bad command lines
# --------------------------------------------------------------------------
bad("no arguments", [], "nothing to do")
bad("unknown option", STIM + ["-x"], "unknown option '-x'")
bad("long option", STIM + ["--rate", "1"], "unknown option '--rate'")
bad("combined options", STIM + ["-nv"], "unknown option '-nv'")
cases.append(("option without value", BASE, ["-r"], 1, "option -r needs a value", "err"))
bad("option value that looks like an option", ["-r", "-n"], "option -r needs a value, but the next word is the option -n")
bad("option twice", STIM + ["-r", "1kHz", "-r", "2kHz"], "option -r is given twice")
bad("stdin file", ["-"], "standard input")
bad("two stimulus files", ["a.sg", "b.sg"], "two stimulus files")
bad("file and -e", ["a.sg", "-e", "x"], "give only one of")
bad("file and -t", ["a.sg", "-t", "5s"], "give only one of")
bad("-e and -t", STIM + ["-t", "5s"], "give only one of")
bad("-T with -t", ["-t", "5s", "-T", "P1"], "cannot be used with -t")
bad("-q and -v", STIM + ["-q", "-v"], "contradict")
bad("bad -m", STIM + ["-m", "ic"], "-m: expected cc or vc")
bad("bad -r unit", STIM + ["-r", "20kHZ"], "-r: '20kHZ' is not a rate")
bad("bad -r word", STIM + ["-r", "fast"], "-r: 'fast' is not a rate")
bad("zero -r", STIM + ["-r", "0"], "-r: '0' is not a rate")
bad("negative -r", STIM + ["-r", "-5"], "-r: '-5' is not a rate")
bad("-r too many decimals", STIM + ["-r", "0.0000001Hz"], "not a rate")
bad("bad -t unit", ["-t", "30x"], "-t: '30x' is not a duration")
bad("-t without a unit", ["-t", "30"], "-t: '30' is not a duration")
bad("-t with a plus sign", ["-t", "+5s"], "-t: '+5s' is not a duration")
bad("-r with a plus sign", STIM + ["-r", "+5Hz"], "-r: '+5Hz' is not a rate")
bad("-r with an incomplete exponent", STIM + ["-r", "1eHz"], "-r: '1eHz' is not a rate")
bad("-r with a doubled exponent letter", STIM + ["-r", "1ee3Hz"], "-r: '1ee3Hz' is not a rate")
bad("-r with a dangling exponent sign", STIM + ["-r", "1e-"], "-r: '1e-' is not a rate")
bad("zero -t", ["-t", "0s"], "-t: '0s' is not a duration")
# an option whose value is missing must not take the next option as its value (-p -n made a real run)
bad("-p followed by -n", STIM + ["-p", "-n"], "option -p needs a value, but the next word is the option -n")
bad("-s followed by -R", STIM + ["-s", "-R"], "option -s needs a value, but the next word is the option -R")
# texts that would be cut where they are kept are refused, not cut
bad("-r written with 48 characters", STIM + ["-r", "0" * 43 + "20kHz"], "-r: the rate is written with more than 47 characters")
good("-r written with 47 characters", STIM + ["-r", "0" * 42 + "20kHz"], "rate")
bad("rate in [rig] written with 48 characters", STIM, "the rate is written with more than 47 characters",
    cfg=edit("rate = 20kHz", "rate = " + "0" * 43 + "20kHz"))
bad("-p longer than 100 characters", STIM + ["-p", "a" * 101], "has at most 100 characters")
# -n checks -t as the run does
bad("-t shorter than one sample, in a dry run too", ["-t", "1us"], "shorter than one sample")
bad("negative -t", ["-t", "-3s"], "-t: '-3s' is not a duration")
bad("-t finer than a picosecond", ["-t", "0.0001ns"], "not a duration")
bad("bad seed", STIM + ["-s", "abc"], "-s: the seed must be")
bad("negative seed", STIM + ["-s", "-1"], "-s: the seed must be")
bad("huge seed", STIM + ["-s", "99999999999999999999999"], "-s: the seed must be")
bad("prefix with slash", STIM + ["-p", "a/b"], "-p: the prefix")
bad("prefix starting with dot", STIM + ["-p", ".x"], "-p: the prefix")
bad("empty value", ["-c", ""], "option -c has an empty value")
bad("missing stimulus file", ["nosuch.sg"], "cannot open stimulus file 'nosuch.sg'")
bad("missing config file", STIM + ["-c", "/nonexistent/lcg2.ini"],
    "cannot open configuration file")

# --------------------------------------------------------------------------
# Idle (rest) values: never silently clipped, so refused at load
# --------------------------------------------------------------------------
good("rest exactly at the limit", STIM, "rest 2000 pA",
     cfg=edit("limit = 5", "limit = 5\nrest = 2000"))
bad("rest above the limit", STIM, "2100 pA needs 5.25 V at the amplifier command, above the limit of 5 V",
    cfg=edit("limit = 5", "limit = 5\nrest = 2100"))
bad("rest above the limit, negative", STIM, "above the limit of 5 V",
    cfg=edit("limit = 5", "limit = 5\nrest = -2100"))
bad("rest above the board range", STIM, "4100 pA needs 10.25 V at the board after calibration, above its range of 10 V",
    cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 4100"))
good("rest just inside the board range", STIM, "rest 3990 pA",
     cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 3990"))
bad("calibration pushes a rest over 10 V", STIM, "after calibration",
    cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 3990\ncal_scale = 0.99"))
bad("calibration offset makes the default rest impossible", STIM,
    "the default rest of 0 is not possible",
    cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\ncal_offset = 10.5"))
bad("rest in vc mode is in mV", STIM + ["-m", "vc"], "250 mV needs 12.5 V",
    cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 250"))
good("a skipped output's rest is not checked", STIM, "P2       skipped",
     cfg=edit("[out P2]\nline = ao1", "[out P2]\nline = ao1\nskip = yes\nrest = 99999"))
bad("a profile can push a rest over the limit", STIM + ["-P", "p"], "above the limit of 5 V",
    cfg=BASE + "\n[profile p]\nout.P1.rest = 2100\n")

# --------------------------------------------------------------------------
# Files from other systems
# --------------------------------------------------------------------------
good("CRLF line endings", STIM, "device         sim", cfg=BASE.replace("\n", "\r\n"))
good("UTF-8 byte order mark", STIM, "device         sim", cfg="\ufeff" + BASE)
good("CRLF and BOM together", STIM, "device         sim", cfg="\ufeff" + BASE.replace("\n", "\r\n"))
good("no newline at the end of the file", STIM, "device         sim", cfg=BASE.rstrip("\n"))
good("tabs around the equals sign", STIM, "mode           vc",
     cfg=edit("mode = cc", "mode\t=\tvc"))

# --------------------------------------------------------------------------
# Profiles
# --------------------------------------------------------------------------
bad("unknown profile", STIM + ["-P", "nope"], "profile 'nope' not found")
bad("unknown profile lists the others", STIM + ["-P", "nope"], "available: single, swap")
bad("profile in a file without profiles", STIM + ["-P", "x"], "defines no [profile NAME]",
    cfg=BASE[:BASE.index("[profile single]")])
bad("profile targets a missing section", STIM + ["-P", "p"],
    "there is no section [out P9] to override",
    cfg=BASE + "\n[profile p]\nout.P9.line = ao3\n")
bad("profile with a bad key form", STIM,
    "bad key 'line'", cfg=BASE + "\n[profile p]\nline = ao3\n")
bad("profile with a bad kind", STIM,
    "bad key 'foo.P1.line'", cfg=BASE + "\n[profile p]\nfoo.P1.line = ao3\n")
bad("profile override makes a duplicate line", STIM + ["-P", "p"],
    "line ao0 is already used by output P1",
    cfg=BASE + "\n[profile p]\nout.P2.line = ao0\n")
bad("profile override with a bad value", STIM + ["-P", "p"], "unknown key",
    cfg=BASE + "\n[profile p]\nout.P1.colour = red\n")

# --------------------------------------------------------------------------
# Bad configuration files
# --------------------------------------------------------------------------
bad("empty file", STIM, "there is no [rig] section", cfg="")
bad("key outside a section", STIM, "outside any section", cfg="device = sim\n" + BASE)
bad("bad section kind", STIM, "bad section [wire P1]", cfg=BASE + "\n[wire P1]\n")
bad("section without name", STIM, "bad section [out]", cfg=BASE + "\n[out]\n")
bad("rig with a name", STIM, "bad section [rig x]", cfg=BASE + "\n[rig x]\n")
bad("bad section name", STIM, "bad section [out P-1]", cfg=BASE + "\n[out P-1]\n")
bad("two spaces in a section", STIM, "bad section [out  P3]", cfg=BASE + "\n[out  P3]\n")
bad("duplicate section", STIM, "section [out P1] appears twice",
    cfg=BASE + "\n[out P1]\nline = ao3\n")
bad("duplicate key", STIM, "key 'line' is given twice",
    cfg=edit("line = ao0", "line = ao0\nline = ao1"))
bad("unterminated section", STIM, "syntax error", cfg=BASE + "\n[out P7\n")
bad("line without equals", STIM, "syntax error", cfg=BASE + "\njust some words\n")
bad("indented continuation line", STIM, "syntax error",
    cfg=edit("unit_cc = pA", "unit_cc = pA\n   more"))
bad("upper case key", STIM, "bad key name 'Gain_cc'",
    cfg=edit("gain_cc = 400", "Gain_cc = 400"))
bad("section name too long", STIM, "section name is too long",
    cfg=BASE + "\n[out " + "N" * 60 + "]\nline = ao3\n")
bad("value too long", STIM, "value is too long",
    cfg=edit("device = sim", "device = " + "x" * 450))
bad("line too long", STIM, "line is too long",
    cfg=edit("device = sim", "device = sim ; " + "x" * 1100))
bad("error line number is reported", STIM,
    "cfg.ini:%d: [out P1] gian_cc: unknown key" % line_of(BASE, "gain_cc = 400"),
    cfg=edit("gain_cc = 400", "gian_cc = 400"))

# [rig]
bad("no device", STIM, "device is required", cfg=edit("device = sim\n", ""))
bad("bad device name", STIM, "is not sim, neuron or a board name",
    cfg=edit("device = sim", "device = 3Dev"))
bad("unknown rig key", STIM, "unknown key", cfg=edit("rate = 20kHz", "speed = 3"))
bad("no mode anywhere", STIM, "the amplifier mode is not set",
    cfg=edit("mode = cc\n", ""))
bad("bad mode in the file", STIM, "expected cc or vc, got 'ic'",
    cfg=edit("mode = cc", "mode = ic"))
bad("bad rate in the file", STIM, "is not a rate", cfg=edit("rate = 20kHz", "rate = 20 kHz"))
bad("bad trigger", STIM, "expected PFI0 ... PFI15",
    cfg=edit("rate = 20kHz", "rate = 20kHz\ntrigger = PFI16"))
bad("trigger lower case", STIM, "expected PFI0 ... PFI15",
    cfg=edit("rate = 20kHz", "rate = 20kHz\ntrigger = pfi1"))
bad("sim_script without neuron", STIM, "only used with device = neuron",
    cfg=edit("rate = 20kHz", "rate = 20kHz\nsim_script = m.py"))
bad("neuron: needs at least one cell", STIM, "needs at least one [cell NAME]",
    cfg=edit("device = sim", "device = neuron"))
bad("neuron: a pipette needs a cell of its name", STIM, "has no [cell P1]",
    cfg=edit("device = sim", "device = neuron") + "\n[cell Q]\nmodel = hh_soma\n")
bad("neuron: a synapse needs pre", STIM, "needs pre = NAME",
    cfg=edit("device = sim", "device = sim") + "\n[cell P1]\n[synapse S]\npost = P1\n")
bad("neuron: a synapse names an unknown cell", STIM, "'Z' is not a [cell] of this file",
    cfg=edit("device = sim", "device = sim") + "\n[cell P1]\n[synapse S]\npre = P1\npost = Z\n")
bad("neuron: unknown synapse key", STIM, "unknown key (allowed: pre, post, type",
    cfg=edit("device = sim", "device = sim") + "\n[cell P1]\n[cell P2]\n[synapse S]\npre = P1\npost = P2\nspeed = 3\n")
bad("sim_dt without neuron", STIM, "only used with device = neuron",
    cfg=edit("rate = 20kHz", "rate = 20kHz\nsim_dt = 25us"))

# [out]
bad("out without line", STIM, "line is required", cfg=edit("line = ao0\n", ""))
bad("analog line out of range", STIM, "an analog output is ao0 ... ao3",
    cfg=edit("line = ao0", "line = ao4"))
bad("analog line with a leading zero", STIM, "an analog output is ao0 ... ao3",
    cfg=edit("line = ao0", "line = ao01"))
bad("digital line on the analog name", STIM, "an analog output is ao0 ... ao3",
    cfg=edit("line = ao0", "line = port0/line1"))
bad("digital line not on port 0", STIM, "must be on port 0",
    cfg=edit("line = port0/line0", "line = port1/line0"))
bad("digital line number too high", STIM, "must be on port 0",
    cfg=edit("line = port0/line0", "line = port0/line8"))
bad("digital with gain", STIM, "not allowed on a digital line",
    cfg=edit("digital = yes", "digital = yes\ngain_cc = 3"))
bad("digital with rest", STIM, "not allowed on a digital line",
    cfg=edit("digital = yes", "digital = yes\nrest = 1"))
bad("digital typo", STIM, "expected yes or no, got 'true'",
    cfg=edit("digital = yes", "digital = true"))
bad("gain without unit", STIM, "gain_cc and unit_cc must be given together",
    cfg=edit("unit_cc = pA\n", "", ))
bad("gain not a number", STIM, "expected a number, got 'x'",
    cfg=edit("gain_cc = 400", "gain_cc = x"))
bad("gain zero", STIM, "the gain must be positive",
    cfg=edit("gain_cc = 400", "gain_cc = 0"))
bad("gain negative", STIM, "the gain must be positive",
    cfg=edit("gain_cc = 400", "gain_cc = -4"))
bad("gain nan", STIM, "expected a number", cfg=edit("gain_cc = 400", "gain_cc = nan"))
bad("unknown unit", STIM, "unknown unit 'pa'", cfg=edit("unit_cc = pA", "unit_cc = pa"))
bad("output cc unit is a voltage", STIM, "wrong dimension",
    cfg=edit("unit_cc = pA", "unit_cc = mV"))
bad("output vc unit is a current", STIM, "wrong dimension",
    cfg=edit("unit_vc = mV", "unit_vc = pA"))
bad("input cc unit is a current", STIM, "wrong dimension",
    cfg=edit("unit_cc = mV", "unit_cc = pA"))
bad("no gain for the mode in use", STIM + ["-m", "vc"], "which the vc mode of this run needs",
    cfg=edit("gain_vc = 20\nunit_vc = mV\nlimit", "limit"))
bad("limit above the board range", STIM, "at most 10",
    cfg=edit("limit = 5", "limit = 11"))
bad("limit zero", STIM, "above 0", cfg=edit("limit = 5", "limit = 0"))
bad("cal_scale zero", STIM, "cal_scale must not be zero",
    cfg=edit("limit = 5", "limit = 5\ncal_scale = 0"))
bad("cal_offset not a number", STIM, "expected a number",
    cfg=edit("limit = 5", "limit = 5\ncal_offset = a"))
bad("skip typo", STIM, "expected yes or no, got 'on'",
    cfg=edit("limit = 5", "limit = 5\nskip = on"))
bad("two outputs on one analog line", STIM, "line ao0 is already used by output P1",
    cfg=edit("line = ao1", "line = ao0"))
bad("two outputs on one digital line", STIM, "already used by output TTL",
    cfg=BASE + "\n[out TTL2]\nline = port0/line0\ndigital = yes\n")
bad("unknown out key", STIM, "unknown key", cfg=edit("limit = 5", "limit = 5\ncolour = red"))
good("skipped output may share a line",
     STIM, "P2       skipped",
     cfg=edit("line = ao1", "line = ao0\nskip = yes"))
good("skipped output needs no gain for the mode",
     STIM + ["-m", "vc"], "P2       skipped",
     cfg=edit("[out P2]\nline = ao1\ngain_cc = 400\nunit_cc = pA\ngain_vc = 20\nunit_vc = mV",
              "[out P2]\nline = ao1\nskip = yes"))

# [in]
bad("in without line", STIM, "line is required",
    cfg=edit("[in P1]\nline = ai0\n", "[in P1]\n"))
bad("input line out of range", STIM, "an analog input is ai0 ... ai15",
    cfg=edit("line = ai0", "line = ai16"))
bad("input range not allowed", STIM, "the range is 0.2, 1, 5 or 10",
    cfg=edit("line = ai0", "line = ai0\nrange = 2"))
bad("input terminal unknown", STIM, "expected RSE, NRSE or DIFF",
    cfg=edit("line = ai0", "line = ai0\nterminal = rse"))
bad("two inputs on one line", STIM, "line ai0 is already used by input P1",
    cfg=edit("line = ai1", "line = ai0"))
bad("DIFF above ai7", STIM, "a differential input is ai0 ... ai7",
    cfg=edit("line = ai0", "line = ai9\nterminal = DIFF"))
bad("DIFF pair already used", STIM, "its second pin",
    cfg=edit("line = ai1", "line = ai8")
        .replace("line = ai0", "line = ai0\nterminal = DIFF", 1))
bad("no input at all", STIM + ["-P", "p"], "no input to record",
    cfg=BASE + "\n[profile p]\nin.P1.skip = yes\nin.P2.skip = yes\n")
bad("input without gain for the mode", STIM, "no gain_cc and unit_cc",
    cfg=edit("gain_cc = 100\nunit_cc = mV\n", ""))

# --------------------------------------------------------------------------
# Checks that need the command line and the file together
# --------------------------------------------------------------------------
bad("-T unknown pipette", STIM + ["-T", "P9"], "there is no output named 'P9'")
bad("-T lists the analog outputs", STIM + ["-T", "P9"], "in use: P1, P2")
bad("-T digital", STIM + ["-T", "TTL"], "is a digital line")
bad("-T skipped", STIM + ["-T", "P2", "-P", "single"], "is skipped in this configuration")
bad("no output to play on", STIM, "no output to play on",
    cfg="\n".join(l for l in BASE.splitlines())
        .replace("[out P1]", "[out P1]\nskip = yes").replace("[out P2]", "[out P2]\nskip = yes")
        .replace("[out TTL]", "[out TTL]\nskip = yes"))
good("record only needs no output",
     ["-t", "5s"], "record only",
     cfg=BASE.replace("[out P1]", "[out P1]\nskip = yes").replace("[out P2]", "[out P2]\nskip = yes")
     .replace("[out TTL]", "[out TTL]\nskip = yes"))
bad("rate above the board limit (2 inputs)", STIM + ["-r", "300kHz"],
    "above the board's 500 kS/s total")
good("rate at the board limit", STIM + ["-r", "250kHz"], "250 kHz")
bad("file rate above the limit", STIM, "500 kS/s total",
    cfg=edit("rate = 20kHz", "rate = 400kHz"))
cases.append(("a run without -n plays on the simulator and writes the data file and the run folder", BASE, STIM, 0,
              ".h5 and .", "err"))                              # no -n added; progress is on stderr

# --------------------------------------------------------------------------
# The runner
# --------------------------------------------------------------------------

def run_case(tmp, case):
    name, cfg, args, rc, fragment, where = case
    path = os.path.join(tmp, "cfg.ini")
    with open(path, "w", newline="") as f:      # newline="": keep \r\n as written
        f.write(cfg)
    with open(os.path.join(tmp, "stim.sg"), "w") as f:
        f.write("sg 2 stimulus\nchannel P1 unit=pA { 1s dc(0) }\n")
    vc = ("-m" in args and args[args.index("-m") + 1] == "vc") or \
         ("-m" not in args and re.search(r"^mode\s*=\s*vc", cfg, re.M) is not None)
    unit = "mV" if vc else "pA"
    args = [("sg 2 stimulus\nchannel P1 unit=%s { 1s dc(0) }" % unit) if a == "@STIM@" else
            ("500ms dc(0); 100ms dc(1)" if a == "@WAVE@" else a) for a in args]
    cmd = [LCG2] + (["-c", path] if "-c" not in args and "-h" not in args else []) + args
    # The message names the file as given, so run in the folder with "cfg.ini".
    cmd = [c if c != path else "cfg.ini" for c in cmd]
    p = subprocess.run(cmd, cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True, timeout=20)
    text = p.stdout if where == "out" else p.stderr
    problems = []
    if p.returncode < 0:
        problems.append("crashed with signal %d" % -p.returncode)
    elif p.returncode != rc:
        problems.append("exit status %d, expected %d" % (p.returncode, rc))
    if fragment not in text:
        problems.append("%s lacks %r" % ("stdout" if where == "out" else "stderr", fragment))
    if where == "err" and not p.stderr.startswith("lcg2: "):
        problems.append("message does not start with 'lcg2: ': %r" % p.stderr[:60])
    return problems, p


def test_examples():
    """Every example configuration loads (the deliberately wrong one is refused)."""
    problems = []
    for path in sorted(os.listdir(os.path.join(ROOT, "examples"))):
        if not path.endswith(".ini"):
            continue
        text = open(os.path.join(ROOT, "examples", path)).read()
        if re.search(r"^device\s*=\s*neuron", text, re.M) and not neuronpy.PY:
            # a dry run of a neuron rig builds its cells for real: it needs a Python with NEURON
            print("      (skipped examples/%s: no Python with NEURON was found; tests/test_neuron.py covers it)" % path)
            continue
        p = subprocess.run([LCG2, "-c", os.path.join("examples", path), "-n", "examples/step.sg"],
                           cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, env=neuronpy.env())
        want = 1 if path in ("bad_rest.ini", "low_limit.ini") else 0   # refused on purpose
        if p.returncode != want:
            problems.append("examples/%s: exit status %d, expected %d (%s)"
                            % (path, p.returncode, want, p.stderr.strip()[:120]))
    return problems


def test_pin_check():
    """make check-pin must fail when a StimGen file differs from the pin."""
    src = os.path.join(os.path.dirname(ROOT), "StimGen", "src")
    if not os.path.isdir(src):
        return ["(skipped: no StimGen folder next to LCG2)"], True
    # The test needs a StimGen that equals the pin, to copy and then damage. If
    # the working copy has been changed on purpose, say so and skip: the pin is
    # doing its job, and STIMGEN_PIN is to be updated by the owner.
    here = subprocess.run(["make", "-s", "check-pin"], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)
    if here.returncode != 0:
        return ["(skipped: ../StimGen differs from STIMGEN_PIN; update the pin after "
                "re-running the tests)"], True
    tmp = tempfile.mkdtemp()
    try:
        for f in os.listdir(src):
            if f.endswith((".c", ".h")):
                shutil.copy(os.path.join(src, f), tmp)
        ok = subprocess.run(["make", "-s", "check-pin", "STIMGEN_SRC=" + tmp], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
        problems = []
        if ok.returncode != 0:
            problems.append("a faithful copy of StimGen was refused: " + ok.stderr + ok.stdout)
        with open(os.path.join(tmp, "eval.c"), "a") as f:
            f.write("\n/* changed */\n")
        bad = subprocess.run(["make", "-s", "check-pin", "STIMGEN_SRC=" + tmp], cwd=ROOT,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True)
        if bad.returncode == 0:
            problems.append("a changed StimGen was accepted")
        if "differ from STIMGEN_PIN" not in bad.stdout + bad.stderr:
            problems.append("the pin message is missing")
        os.remove(os.path.join(tmp, "util.c"))
        gone = subprocess.run(["make", "-s", "check-pin", "STIMGEN_SRC=" + tmp], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        if gone.returncode == 0 or "cannot read" not in gone.stdout + gone.stderr:
            problems.append("a missing StimGen file was not reported")
        return problems, False
    finally:
        shutil.rmtree(tmp)


def plan_of(text):
    """The plan without the line that names the configuration file."""
    head = text.split("resolved configuration:")[0]
    return [l for l in head.splitlines() if not l.startswith("  configuration")]


def test_round_trip():
    """The text printed by -n -v is a configuration that gives the same plan.

    The run archive (M4) stores this text as 'the config used', so loading it
    again, without the profile and the -m flag, must reproduce the run."""
    tmp = tempfile.mkdtemp()
    problems = []
    try:
        for flags in ([], ["-P", "swap"], ["-m", "vc"], ["-P", "single", "-m", "vc"]):
            with open(os.path.join(tmp, "a.ini"), "w") as f:
                f.write(BASE)
            text = "sg 2 stimulus\nchannel P1 unit=%s { 1s dc(0) }" % ("mV" if "vc" in flags else "pA")
            first = subprocess.run([LCG2, "-c", "a.ini", "-e", text, "-s", "1", "-n", "-v"] + flags,
                                   cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   universal_newlines=True)
            if first.returncode != 0:
                problems.append("%s: first run failed: %s" % (flags, first.stderr))
                continue
            dumped = first.stdout.split("resolved configuration:\n\n")[1]
            with open(os.path.join(tmp, "b.ini"), "w") as f:
                f.write(dumped)
            second = subprocess.run([LCG2, "-c", "b.ini", "-e", text, "-s", "1", "-n", "-v"],
                                    cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    universal_newlines=True)
            if second.returncode != 0:
                problems.append("%s: reload failed: %s" % (flags, second.stderr))
                continue
            a = plan_of(first.stdout)
            b = plan_of(second.stdout)
            if a != b:
                problems.append("%s: plans differ:\n%s\n--\n%s" % (flags, "\n".join(a), "\n".join(b)))
            # and the dump of the reloaded file must be a fixed point
            d2 = second.stdout.split("resolved configuration:\n\n")[1]
            strip = lambda t: [l.split("   ; set on")[0] for l in t.splitlines()
                               if not l.startswith(";")]
            if strip(d2) != strip(dumped):
                problems.append("%s: the dump is not a fixed point" % (flags,))
    finally:
        shutil.rmtree(tmp)
    return problems


def main():
    failed = 0
    pin_skipped = False
    tmp = tempfile.mkdtemp()
    try:
        for case in cases:
            problems, p = run_case(tmp, case)
            if problems:
                failed += 1
                print("FAIL  %s" % case[0])
                for pr in problems:
                    print("      " + pr)
                print("      args: %s" % " ".join(case[2]))
                print("      stderr: %s" % p.stderr.strip()[:300])
        problems = test_examples()
        if problems:
            failed += 1
            print("FAIL  examples")
            for pr in problems:
                print("      " + pr)
        problems = test_round_trip()
        if problems:
            failed += 1
            print("FAIL  round trip of the resolved configuration")
            for pr in problems:
                print("      " + pr)
        problems, skipped = test_pin_check()
        if skipped:
            pin_skipped = True
            print(problems[0])
        elif problems:
            failed += 1
            print("FAIL  pin check")
            for pr in problems:
                print("      " + pr)
    finally:
        shutil.rmtree(tmp)
    n = len(cases) + 2 + (0 if pin_skipped else 1)
    print("%d of %d tests passed" % (n - failed, n))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
