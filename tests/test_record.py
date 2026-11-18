#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# M4 acceptance test: the HDF5 data file and the run folder.
#
#     python3 tests/test_record.py        (after `make`)
#
# The files are read back with h5py, an independent reader. It needs a Python that has
# h5py: this script finds one (the one running it, $LCG2_H5PY_PYTHON, or a Homebrew
# Python 3.11/3.12/3.13) and runs itself with it. If there is none the test FAILS, loudly:
#
#     pip install h5py        (in a Python whose HDF5 you can use)
#
# What it proves:
#   * the layout, the attributes and the exact values (noise switched off through the
#     test driver build/runtest, which calls the same run_execute() as lcg2, so lcg2
#     has no hidden switch for it);
#   * the run folder holds every item of SPEC 3.2 and the digests match hashlib;
#   * an aborted trial is kept and marked incomplete; a killed run (SIGKILL) keeps its
#     completed trials; a full disk ends the run with status 2 and keeps what was written;
#   * an unwritable folder ends with status 1 before anything is touched, and a dry run
#     or a refused stimulus writes nothing;
#   * HDF5 never prints on its own.
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
RECORDTEST = os.path.join(BUILD, "recordtest")
SG = os.path.join(BUILD, "sg")
CFG = os.path.join(ROOT, "examples/lcg2.ini")
EX = lambda name: os.path.join(ROOT, "examples", name)           # noqa: E731

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


def go(cmd, cwd, stdin=None, **kw):
    kw.setdefault("timeout", 120)
    return subprocess.run(cmd, cwd=cwd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, errors="replace", **kw)


def rt(args, cwd, driver=(), **kw):
    """Run the test driver: the real run_execute(), with the simulator settings of the driver."""
    return go([RUNTEST] + list(driver) + ["--"] + args, cwd, **kw)


def h5_of(cwd):
    files = glob.glob(os.path.join(cwd, "*.h5"))
    return files[0] if len(files) == 1 else None


def folder_of(h5):
    return os.path.join(os.path.dirname(h5), "." + os.path.basename(h5)[:-3])


def text(x):
    return x.decode("utf-8") if isinstance(x, bytes) else x


def readfile(path):
    with open(path, "rb") as f:
        return f.read()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def newdir(tmp, name):
    d = os.path.join(tmp, name)
    os.makedirs(d)
    return d


def pin(key):
    for l in open(os.path.join(ROOT, "STIMGEN_PIN")):
        m = re.match(r"^%s\s*=\s*(\S+)" % key, l)
        if m:
            return m.group(1)


# --------------------------------------------------------------------------
# 1. The layout, the attributes, the exact values
# --------------------------------------------------------------------------
def test_layout(tmp):
    d = newdir(tmp, "layout")
    r = rt(["-c", CFG, "-s", "7", EX("step.sg")], d, driver=["--noise", "0"])
    check("a recorded run succeeds", r.returncode == 0, r.stderr)
    h5 = h5_of(d)
    check("exactly one data file is written", h5 is not None, os.listdir(d))
    if not h5:
        return
    f = h5py.File(h5, "r")
    stem = os.path.basename(h5)[:-3]
    check("the file name is YYYYMMDD_HHMMSS (local time)", re.match(r"^\d{8}_\d{6}$", stem) is not None, stem)
    A = {k: text(v) for k, v in f.attrs.items()}
    check("root attributes: format, versions, pin, run name, mode, device",
          A.get("format") == "lcg2-hdf5/1" and A.get("lcg2_version") == "0.2.0" and
          A.get("stimgen_commit") == pin("commit") and A.get("stimgen_sources_sha256") == pin("sources_sha256") and
          A.get("run_name") == stem and A.get("mode") == "cc" and A.get("device") == "sim" and
          re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$", A.get("started_utc", "")) and
          A.get("stimgen_library_version"), A)
    given = open(EX("step.sg")).read()
    sb = f.id.get_create_plist().get_version()
    check("the file format is capped at HDF5 1.10 (superblock version 3 at most), so older readers can open it",
          sb[0] <= 3, sb)
    check("the stimulus text is stored as given", text(f["stimulus_text"][()]) == given)
    check("the configuration is stored as used", text(f["config"][()]) == open(os.path.join(folder_of(h5), "config.ini")).read())
    check("the command line is stored", "-s 7" in text(f["command_line"][()]))
    prov = json.loads(text(f["provenance"][()]))
    check("the provenance is JSON, and the same as in the run folder",
          prov == json.load(open(os.path.join(folder_of(h5), "provenance.json"))), list(prov))

    g = f["trial_0000"]
    a = {k: text(v) if isinstance(v, bytes) else v for k, v in g.attrs.items()}
    check("trial attributes: index, name, kind, seed, rate, samples, times, flags",
          a["index"] == 0 and a["name"] == "stimulus" and a["kind"] == "stimulus" and a["seed"] == "7" and
          a["rate_hz"] == 20000 and a["rate_p"] == 20000 and a["rate_q"] == 1 and a["n_samples"] == 40000 and
          a["scheduled_ns"] == 0 and a["start_ns"] == 0 and a["end_ns"] == 2000000000 and a["delay_ns"] == 0 and
          a["latency_ns"] == 0 and a["recorded"] == 40000 and a["incomplete"] == 0 and a["virtual_time"] == 1 and
          a["variables"] == "" and re.match(r"^\d{4}-.*Z$", a["start_utc"]), a)
    p1, p2 = g["ai/P1"], g["ai/P2"]
    x = p1[:]
    check("ai/P1 holds exactly the loopback of the command (noise off): 0, then 50 mV, then 0",
          p1.dtype == "float64" and p1.shape == (40000,) and (x[:10000] == 0).all() and
          (x[10000:30000] == 50.0).all() and (x[30000:] == 0).all(), (x[:3], x[10000:10003]))
    check("ai/P2 hears only the idle output (0)", (p2[:] == 0).all())
    check("inputs are chunked with a fixed chunk and an unlimited maximum",
          p1.chunks == (8192,) and p1.maxshape == (None,), (p1.chunks, p1.maxshape))
    pa = {k: text(v) if isinstance(v, bytes) else v for k, v in p1.attrs.items()}
    check("ai attributes: unit, line, gain, calibration, range, terminal, mode, over-range count",
          pa == {"unit": "mV", "line": "ai0", "gain": 100.0, "cal_scale": 1.0, "cal_offset": 0.0, "range_v": 10.0,
                 "terminal": "RSE", "mode": "cc", "overrange_samples": 0}, pa)
    ao = g["ao/P1"][:]
    check("ao/P1 is the command that was played, in the unit of the output (0, 200 pA, 0)",
          (ao[:10000] == 0).all() and (ao[10000:30000] == 200.0).all() and (ao[30000:] == 0).all() and
          dict((k, text(v) if isinstance(v, bytes) else v) for k, v in g["ao/P1"].attrs.items()) ==
          {"unit": "pA", "line": "ao0", "digital": 0, "rest": 0.0, "gain": 400.0, "limit_v": 5.0})
    check("idle outputs have no dataset", list(g["ao"].keys()) == ["P1"], list(g["ao"].keys()))
    mk = g["markers"][:]
    check("the markers of the stimulus are stored (name, sample)",
          len(mk) == 1 and text(mk[0]["name"]) == "P1.step" and mk[0]["sample"] == 10000, mk)
    check("a plain stimulus has no 'info' record", "info" not in g)
    check("the ai group has the recorded inputs and nothing else", sorted(g["ai"].keys()) == ["P1", "P2"])
    f.close()


# --------------------------------------------------------------------------
# 2. Protocols, digital lines, calibration, voltage clamp, latency
# --------------------------------------------------------------------------
def test_variants(tmp):
    d = newdir(tmp, "proto")
    r = rt(["-c", CFG, "-P", "single", "-s", "7", EX("steps_protocol.sg")], d, driver=["--noise", "0"])
    h5 = h5_of(d)
    check("a protocol of 39 trials is recorded", r.returncode == 0 and h5, r.stderr)
    f = h5py.File(h5, "r")
    groups = sorted(k for k in f if k.startswith("trial_"))
    check("one group per trial, in order", groups == ["trial_%04d" % i for i in range(39)], groups[:3])
    ok = True
    for i, name in enumerate(groups):
        g = f[name]
        v = text(g.attrs["variables"])
        m = re.match(r"^amp=(-?\d+)pA$", v)
        if not m or g["ao/P1"][15000] != float(m.group(1)) or g.attrs["start_ns"] != i * 5000000000 or \
                g.attrs["incomplete"] != 0 or g["ai/P1"].shape != (40000,):
            ok = False
            print("      trial", i, v, g["ao/P1"][15000], g.attrs["start_ns"])
    check("each trial's command equals the amplitude of its variables, and the starts are 5 s apart", ok)
    info = json.loads(text(f["trial_0003/info"][()]))
    check("each trial of a protocol keeps StimGen's own record of it", info["index"] == 3 and
          "protocol_seed" in info and text(f["trial_0003"].attrs["kind"]) == "protocol trial", info)
    seeds = [text(f[n].attrs["seed"]) for n in groups]
    check("every trial has its own seed", len(set(seeds)) == 39)
    f.close()

    d = newdir(tmp, "digital")
    r = rt(["-c", CFG, "-s", "7", EX("camera.sg")], d, driver=["--noise", "0"])
    f = h5py.File(h5_of(d), "r")
    ttl = f["trial_0000/ao/TTL"]
    t = ttl[:]
    check("a digital line is stored as 0/1 bytes with the digital flag", ttl.dtype == "uint8" and
          set(t.tolist()) == {0, 1} and int(ttl.attrs["digital"]) == 1 and int(t.sum()) == 4000, (ttl.dtype, int(t.sum())))
    check("an analog and a digital command side by side", sorted(f["trial_0000/ao"].keys()) == ["P1", "TTL"])
    f.close()

    d = newdir(tmp, "cal")
    cfg = os.path.join(tmp, "cal.ini")
    open(cfg, "w").write(open(CFG).read().replace("[in P1]\nline = ai0", "[in P1]\nline = ai0\ncal_scale = 2\ncal_offset = 0.25"))
    rt(["-c", cfg, "-s", "7", EX("step.sg")], d, driver=["--noise", "0"])
    f = h5py.File(h5_of(d), "r")
    x = f["trial_0000/ai/P1"]
    check("input calibration and gain are applied: 100 * (2 * 0.5 + 0.25) = 125 mV, and 25 mV at rest",
          x[10000] == 125.0 and x[0] == 25.0 and float(x.attrs["cal_scale"]) == 2.0 and float(x.attrs["cal_offset"]) == 0.25)
    f.close()

    d = newdir(tmp, "vc")
    rt(["-c", EX("single_pipette.ini"), "-m", "vc", "-s", "7", EX("step_vc.sg")], d, driver=["--noise", "0"])
    f = h5py.File(h5_of(d), "r")
    ai, ao = f["trial_0000/ai/P1"], f["trial_0000/ao/P1"]
    check("voltage clamp: the command is in mV and the recording in pA, with the attributes of that mode",
          ao[15000] == 20.0 and text(ao.attrs["unit"]) == "mV" and ai[15000] == 1000.0 and
          text(ai.attrs["unit"]) == "pA" and text(ai.attrs["mode"]) == "vc" and text(f.attrs["mode"]) == "vc")
    f.close()

    d = newdir(tmp, "latency")
    fast = os.path.join(tmp, "fast.sg")
    open(fast, "w").write("sg 2 protocol\nrepeat 4\ngap 0s\nstimulus {\n channel P1 unit=pA { 0.1s dc(10) }\n}\n")
    rt(["-c", CFG, "-s", "7", "-r", "20kHz", fast], d, driver=["--noise", "0", "--latency", "15000000"])
    f = h5py.File(h5_of(d), "r")
    g = [f["trial_%04d" % i] for i in range(4)]
    check("the start latency and the delay are stored", all(x.attrs["latency_ns"] == 15000000 and
          x.attrs["delay_ns"] == 15000000 for x in g), [(x.attrs["latency_ns"], x.attrs["delay_ns"]) for x in g])

    def ms(u):
        h, m, s = text(u)[11:-1].split(":")
        return (int(h) * 3600 + int(m) * 60) * 1000 + round(float(s) * 1000)
    u = [ms(x.attrs["start_utc"]) for x in g]
    check("trials a tenth of a second apart have different UTC start times (to the millisecond)",
          len(set(u)) == 4 and all(abs((u[i + 1] - u[i]) - 115) <= 1 for i in range(3)), u)
    f.close()


# --------------------------------------------------------------------------
# 3. Record only, and the streaming layout
# --------------------------------------------------------------------------
def test_record_only(tmp):
    d = newdir(tmp, "rec")
    r = rt(["-c", CFG, "-r", "20kHz", "-t", "2s"], d, driver=["--noise", "0"])
    f = h5py.File(h5_of(d), "r")
    g = f["trial_0000"]
    check("record only: ai datasets, no command, no stimulus text, kind 'record'",
          r.returncode == 0 and sorted(g["ai"].keys()) == ["P1", "P2"] and "ao" not in g and "stimulus_text" not in f and
          text(g.attrs["kind"]) == "record" and text(g.attrs["name"]) == "record" and g.attrs["n_samples"] == 40000 and
          g["ai/P1"].shape == (40000,) and "markers" not in g and "info" not in g, list(g))
    check("record only: no seed is stored", text(g.attrs["seed"]) == "")
    f.close()

    d = newdir(tmp, "slow")
    r = rt(["-c", CFG, "-r", "1Hz", "-t", "60min"], d, driver=["--noise", "0"])
    f = h5py.File(h5_of(d), "r")
    x = f["trial_0000/ai/P1"]
    check("at 1 Hz the chunk stays 8192 samples (not one sample per block): one hour is 3600 samples",
          r.returncode == 0 and x.shape == (3600,) and x.chunks == (8192,), (x.shape, x.chunks, r.stderr))
    f.close()


# --------------------------------------------------------------------------
# 4. The run folder
# --------------------------------------------------------------------------
def verify_sums(cwd, folder):
    """SHA256SUMS: every line must match the file, checked with hashlib."""
    lines = open(os.path.join(folder, "SHA256SUMS")).read().splitlines()
    bad = []
    for l in lines:
        digest, path = l.split("  ", 1)
        full = os.path.join(cwd, path)
        if not os.path.exists(full) or sha(readfile(full)) != digest:
            bad.append(path)
    return lines, bad


def test_folder(tmp):
    d = newdir(tmp, "folder")
    # a stimulus that uses a waveform file and reads a data file
    open(os.path.join(d, "inner.sg"), "w").write("0.5s dc(2)\n")
    open(os.path.join(d, "noise.sg"), "w").write("1s ou(mean=0, sd=10, tau=5ms)\n")
    open(os.path.join(d, "data.txt"), "w").write("\n".join(str(i * 0.5) for i in range(100)) + "\n")
    stim = ('sg 2 stimulus\n# use "ignored.sg" is only a comment\nrate 20kHz\n'
            'channel P1 unit=pA use "noise.sg"\nchannel P2 unit=pA { 5ms file("data.txt", rate=20kHz) }\n')
    open(os.path.join(d, "both.sg"), "w").write(stim)
    args = ["-c", CFG, "-s", "9", "-p", "cell3", "both.sg"]
    r = rt(args, d, driver=["--noise", "0"])
    check("a run with use and file() succeeds", r.returncode == 0, r.stderr)
    h5 = h5_of(d)
    base = os.path.basename(h5)
    check("-p gives the prefix of the names", re.match(r"^cell3_\d{8}_\d{6}\.h5$", base) is not None, base)
    fo = folder_of(h5)
    check("the run folder is hidden and has the same stem", os.path.isdir(fo) and os.path.basename(fo) == "." + base[:-3], fo)
    names = sorted(os.listdir(fo))
    want = ["SHA256SUMS", "canonical.sg", "command.txt", "config.ini", "config_original.ini", "files", "log.txt",
            "provenance.json", "stimulus.sg", "trials.json"]
    check("the run folder holds every item of SPEC 3.2", names == want, names)

    check("config_original.ini is the file as it was", readfile(os.path.join(fo, "config_original.ini")) == readfile(CFG))
    check("config.ini is the resolved configuration (a comment names the file and the profile)",
          open(os.path.join(fo, "config.ini")).read().startswith("; LCG2 resolved configuration") and
          "[out P1]" in open(os.path.join(fo, "config.ini")).read())
    check("stimulus.sg is the text as given", open(os.path.join(fo, "stimulus.sg")).read() == stim)
    canon = open(os.path.join(fo, "canonical.sg")).read()
    check("canonical.sg is self-contained: the use is expanded away", "use" not in canon and "ou(" in canon, canon[:200])
    cmd_part, _, folder_part = open(os.path.join(fo, "command.txt")).read().rpartition("\nfolder: ")
    check("command.txt holds the command line, quoted so it can be run again, and the folder",
          shlex.split(cmd_part) == ["lcg2"] + args and os.path.realpath(folder_part.strip()) == os.path.realpath(d),
          (cmd_part, folder_part))
    tj = json.load(open(os.path.join(fo, "trials.json")))
    check("trials.json for a plain stimulus: its kind and seed", tj == {"kind": "stimulus", "seed": "9", "trials": 1}, tj)

    files = sorted(os.listdir(os.path.join(fo, "files")))
    check("files/ has the file read by file() and the files pulled in by use, under safe flat names",
          files == ["01_data.txt", "02_noise.sg"], files)
    check("...copies identical to the originals", readfile(os.path.join(fo, "files/01_data.txt")) == readfile(os.path.join(d, "data.txt"))
          and readfile(os.path.join(fo, "files/02_noise.sg")) == readfile(os.path.join(d, "noise.sg")))
    check("...a use inside a comment is not followed", "ignored" not in "".join(files) and "ignored.sg" not in
          open(os.path.join(fo, "log.txt")).read().replace('"ignored.sg" is only a comment', ""))

    prov = json.load(open(os.path.join(fo, "provenance.json")))
    check("provenance: versions, pin, mode, device, profile, virtual time",
          prov["lcg2_version"] == "0.2.0" and prov["stimgen"]["pinned_commit"] == pin("commit") and
          prov["stimgen"]["pinned_sources_sha256"] == pin("sources_sha256") and prov["mode"] == "cc" and
          prov["device"] == "sim" and prov["virtual_time"] is True, prov["stimgen"])
    check("provenance: the digests are those of the files in the folder",
          prov["config_sha256"] == sha(readfile(os.path.join(fo, "config.ini"))) and
          prov["config_file"]["sha256"] == sha(readfile(CFG)) and prov["config_file"]["path"] == CFG and
          prov["stimulus_sha256"] == sha(stim.encode()) and prov["canonical_sha256"] == sha(canon.encode()))
    check("provenance: the rate, the seed and its source, the clipping count",
          prov["rate"]["hz"] == 20000 and prov["rate"]["source"] == "stated by the stimulus" and
          prov["seed"] == {"kind": "stimulus", "value": "9", "source": "-s"} and prov["clipping"]["samples_clipped"] == 0 and
          prov["convert_rate"] is None and prov["trials"] == 1, prov["seed"])
    check("provenance: the files StimGen says file() read, with its digest, and the copies kept",
          prov["files_read_by_file_function"] == [{"path": "data.txt", "sha256": sha(readfile(os.path.join(d, "data.txt"))),
                                                   "how": "listed by StimGen"}] and
          len(prov["files_archived"]) == 2, prov["files_archived"])

    lines, bad = verify_sums(d, fo)
    check("SHA256SUMS: every digest is right (checked with hashlib)", not bad and len(lines) == 10, (bad, len(lines)))
    check("SHA256SUMS: the data file comes first", lines[0].endswith("  " + base) and lines[0].split()[0] == sha(readfile(h5)))
    log = open(os.path.join(fo, "log.txt")).read()
    check("log.txt: versions, command, trial line, end, the data digest and the digest record",
          "LCG2 0.2.0, run cell3_" in log and "StimGen pinned at " + pin("commit") in log and "trial 1/1" in log and
          "end  completed: 1 of 1" in log and "data  " + base in log and "sha256 " + sha(readfile(h5)) in log and
          "digests of 10 items written" in log, log)
    check("log.txt: the digests of the copied files", "sha256 " + sha(readfile(os.path.join(d, "noise.sg"))) in log)
    if shutil.which("shasum"):
        v = go(["shasum", "-a", "256", "-c", os.path.join(fo, "SHA256SUMS")], d)
        check("`shasum -c` confirms the digests from the folder that holds the data file", v.returncode == 0, v.stdout + v.stderr)

    # StimGen's warnings reach the log and the provenance, and every digest is in the log
    d5 = newdir(tmp, "warn")
    warn_sg = os.path.join(tmp, "warn.sg")
    open(warn_sg, "w").write("sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) }\nchannel P2 unit=pA { 500ms dc(1) }\n")
    r = rt(["-c", CFG, "-s", "1", warn_sg], d5)
    fo5 = folder_of(h5_of(d5))
    log5 = open(os.path.join(fo5, "log.txt")).read()
    prov5 = json.load(open(os.path.join(fo5, "provenance.json")))
    check("a StimGen warning goes to the log and is printed",
          "warning  channels have different durations; shorter ones hold their rest value" in log5 and
          "different durations" in r.stderr, log5)
    check("...and to the provenance record",
          len(prov5["warnings"]) == 1 and "different durations" in prov5["warnings"][0]["text"] and
          prov5["warnings"][0]["trials"] == 1, prov5.get("warnings"))
    lines5, bad5 = verify_sums(d5, fo5)
    check("every item's digest is in the log, with its path (SPEC 3.2), and the data file's",
          all(("sha256 %s  %s" % tuple(l.split("  ", 1))) in log5 for l in lines5[1:]) and
          ("data  " + os.path.basename(h5_of(d5)) + "  ") in log5 and not bad5, (bad5, log5[-600:]))
    d6 = newdir(tmp, "warnproto")
    pw = os.path.join(tmp, "pw.sg")
    open(pw, "w").write("sg 2 protocol\nrepeat 3\nstimulus {\n channel P1 unit=pA { 1s dc(1) }\n channel P2 unit=pA { 500ms dc(1) }\n}\n")
    rt(["-c", CFG, "-s", "1", "-r", "20kHz", pw], d6)
    log6 = open(os.path.join(folder_of(h5_of(d6)), "log.txt")).read()
    check("the same warning in three trials is logged once, with its count and the first trial",
          log6.count("warning  ") == 1 and "(3 trials, first in trial 0000)" in log6, log6)

    # a command line with spaces, quotes and a newline survives the quoting
    d2 = newdir(tmp, "quote")
    text2 = "sg 2 stimulus\nrate 20kHz\nchannel P1 unit=pA { 1s dc(1) } # it's \"quoted\""
    args2 = ["-c", CFG, "-e", text2, "-p", "a-b"]
    rt(args2, d2)
    cmd2 = open(os.path.join(folder_of(h5_of(d2)), "command.txt")).read().rpartition("\nfolder: ")[0]
    check("a command line with a newline, quotes and spaces is stored so that it can be run again",
          shlex.split(cmd2) == ["lcg2"] + args2, cmd2)
    check("-e text is stored as the stimulus", open(os.path.join(folder_of(h5_of(d2)), "stimulus.sg")).read() == text2)

    # a protocol: its trials in trials.json (what sg expand gives), no canonical form
    d3 = newdir(tmp, "protofolder")
    rt(["-c", CFG, "-P", "single", "-s", "7", "-r", "20kHz", EX("timing_period.sg")], d3)
    fo3 = folder_of(h5_of(d3))
    tj = json.load(open(os.path.join(fo3, "trials.json")))
    check("a protocol: trials.json holds the trial list with seeds and timing",
          len(tj["trials"]) == 4 and tj["timing"] == "period 5s" and tj["protocol_seed"] == "7", list(tj))
    check("a protocol has no canonical.sg, and the log says why", not os.path.exists(os.path.join(fo3, "canonical.sg")) and
          "no canonical form" in open(os.path.join(fo3, "log.txt")).read())
    prov3 = json.load(open(os.path.join(fo3, "provenance.json")))
    check("provenance of a protocol: the protocol seed and its source",
          prov3["seed"] == {"kind": "protocol", "value": "7", "source": "-s"} and prov3["trials"] == 4, prov3["seed"])

    # a stimulus in a protocol that uses a file: the scan also reads the protocol text
    d4 = newdir(tmp, "protouse")
    open(os.path.join(d4, "noise.sg"), "w").write("0.5s ou(mean=0, sd=5, tau=5ms)\n")
    open(os.path.join(d4, "p.sg"), "w").write('sg 2 protocol\nrepeat 2\nstimulus {\n channel P1 unit=pA use "noise.sg"\n}\n')
    r = rt(["-c", CFG, "-s", "3", "-r", "20kHz", "p.sg"], d4)
    check("a protocol whose stimulus uses a file: the file is kept", r.returncode == 0 and
          os.listdir(os.path.join(folder_of(h5_of(d4)), "files")) == ["01_noise.sg"], r.stderr)


# --------------------------------------------------------------------------
# 5. The name is claimed atomically
# --------------------------------------------------------------------------
def test_names(tmp):
    d = newdir(tmp, "names")
    for i in range(8):                                   # eight runs, within a second or two
        r = rt(["-c", CFG, "-s", str(i), EX("step.sg")], d)
        if r.returncode != 0:
            check("repeated runs succeed", False, r.stderr)
            return
    h5s = sorted(glob.glob(os.path.join(d, "*.h5")))
    folders = sorted(g for g in os.listdir(d) if g.startswith("."))
    check("eight runs in a row give eight distinct data files and eight run folders (a clash gets a -2, -3...)",
          len(h5s) == 8 and len(folders) == 8 and len(set(h5s)) == 8, (h5s, folders))
    check("every data file has its own folder with the same stem", all(os.path.isdir(folder_of(h)) for h in h5s))
    check("each run kept its own seed (no run wrote into another's files)",
          sorted(text(h5py.File(h, "r")["trial_0000"].attrs["seed"]) for h in h5s) == sorted(str(i) for i in range(8)))


# --------------------------------------------------------------------------
# 6. Refusals before anything is touched, and runs that write nothing
# --------------------------------------------------------------------------
def test_setup(tmp):
    d = newdir(tmp, "dry")
    r = go([LCG2, "-n", "-c", CFG, EX("step.sg")], d)
    check("a dry run writes nothing", r.returncode == 0 and os.listdir(d) == [], os.listdir(d))
    r = go([LCG2, "-n", "-v", "-c", CFG, EX("step.sg")], d)
    check("...nor does -v", os.listdir(d) == [])
    r = go([LCG2, "-c", EX("low_limit.ini"), EX("step.sg")], d)
    check("a stimulus that clips is refused before anything is written: status 1, an empty folder",
          r.returncode == 1 and os.listdir(d) == [], (r.stderr, os.listdir(d)))
    r = go([LCG2, "-c", CFG, "-P", "nope", EX("step.sg")], d)
    check("a bad profile writes nothing", r.returncode == 1 and os.listdir(d) == [])

    if hasattr(os, "geteuid") and os.geteuid() == 0:
        check("an unwritable folder is refused (cannot be tested as root)", False, "run the tests as an ordinary user")
        return
    ro = newdir(tmp, "readonly")
    os.chmod(ro, 0o555)
    try:
        r = go([LCG2, "-c", CFG, EX("step.sg")], ro)
        check("an unwritable folder: status 1 before anything is touched, with a clear message",
              r.returncode == 1 and "cannot create the run folder" in r.stderr and "running" not in r.stderr and
              os.listdir(ro) == [], (r.returncode, r.stderr))
    finally:
        os.chmod(ro, 0o755)

    # a prefix that is a path would escape the folder: refused by the command line
    r = go([LCG2, "-c", CFG, "-p", "../x", EX("step.sg")], d)
    check("a prefix that is a path is refused (it would escape the folder)", r.returncode == 1 and os.listdir(d) == [])


def test_reader(tmp):
    """examples/read_run.py: the idiom the manual shows for reading a file."""
    d = newdir(tmp, "reader")
    rt(["-c", CFG, "-P", "single", "-s", "7", "-r", "20kHz", EX("timing_period.sg")], d, driver=["--noise", "0"])
    r = go([sys.executable, EX("read_run.py").replace("examples/", "examples/"), h5_of(d)], d)
    out = r.stdout
    check("examples/read_run.py reads a recorded file and summarises it",
          r.returncode == 0 and "trials     4" in out and "trial_0003  0003" in out and "start 15.000 s" in out and
          "command P1     pA" in out and "input   P1     mV  40000 samples" in out and "rate       20000 Hz" in out and
          "seed       7 (-s)" in out, r.stdout + r.stderr)
    r = go([sys.executable, EX("read_run.py")], d)
    check("...and shows its help without an argument", r.returncode == 2 and "How h5py hands back" in r.stdout)


def test_space(tmp):
    d = newdir(tmp, "estimate")
    r = rt(["-c", CFG, "-P", "single", "-s", "7", "-r", "20kHz", EX("steps_protocol.sg")], d, driver=["--noise", "0"])
    h5 = h5_of(d)
    log = open(os.path.join(folder_of(h5), "log.txt")).read()
    m = re.search(r"disk  the data is expected to take about (\d+) bytes; (-?\d+) bytes free", log)
    actual = os.path.getsize(h5)
    check("the log records the expected size of the data, and it covers the real file without being wild",
          m is not None and actual <= int(m.group(1)) <= 1.2 * actual and int(m.group(2)) > 0,
          (m.groups() if m else log[:300], actual))
    d = newdir(tmp, "estimate_rec")
    rt(["-c", CFG, "-r", "20kHz", "-t", "30s"], d, driver=["--noise", "0"])
    h5 = h5_of(d)
    m = re.search(r"about (\d+) bytes", open(os.path.join(folder_of(h5), "log.txt")).read())
    check("...also for a record-only run", m is not None and os.path.getsize(h5) <= int(m.group(1)) <= 1.2 * os.path.getsize(h5),
          (m.group(1) if m else None, os.path.getsize(h5)))

    # a file system that is really small: a disk image (macOS); not testable without privileges elsewhere
    if shutil.which("hdiutil"):
        img = os.path.join(tmp, "small.dmg")
        mnt = os.path.join(tmp, "mnt")
        subprocess.run(["hdiutil", "create", "-size", "6m", "-fs", "HFS+", "-volname", "lcg2small", "-ov", img],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        a = subprocess.run(["hdiutil", "attach", img, "-mountpoint", mnt], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if a.returncode == 0:
            try:
                r = go([LCG2, "-c", CFG, "-r", "20kHz", "-t", "60s"], mnt)         # 19 MB of data in a 6 MB volume
                ours = [x for x in os.listdir(mnt) if x.endswith(".h5") or re.match(r"^\.\d{8}_\d{6}", x)]
                check("not enough free space: the run is refused before anything is written (status 1)",
                      r.returncode == 1 and "not enough free disk space" in r.stderr and
                      "nothing was written" in r.stderr and ours == [], (r.returncode, r.stderr, ours))
                r = go([LCG2, "-c", CFG, "-r", "20kHz", "-t", "1s"], mnt)           # 0.3 MB: fits
                check("...but a run that fits in the same volume works", r.returncode == 0 and h5_of(mnt) is not None, r.stderr)
            finally:
                subprocess.run(["hdiutil", "detach", mnt, "-force"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if os.path.exists(img):
                    os.remove(img)
        else:
            print("NOTE  could not attach a disk image: the free-space refusal was not tested on a small volume")
    else:
        print("NOTE  no hdiutil: the free-space refusal was not tested on a small volume (needs a small file system)")


# --------------------------------------------------------------------------
# 7. Aborts: Ctrl-C mid-trial, and the end of input at the Enter prompt
# --------------------------------------------------------------------------
def read_until(proc, needle, timeout=20):
    import select
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


def test_abort(tmp):
    d = newdir(tmp, "abort")
    proto = os.path.join(tmp, "three.sg")
    open(proto, "w").write("sg 2 protocol\nrepeat 3\ngap 0s\nstimulus {\n channel P1 unit=pA { 1s dc(100) }\n}\n")
    p = subprocess.Popen([LCG2, "-R", "-c", CFG, "-s", "1", "-r", "20kHz", proto], cwd=d, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seen = read_until(p, "trial 1/3")                    # the first trial has just ended: the second begins
    time.sleep(0.35)
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    err = seen + p.stderr.read().decode("utf-8", "replace")
    check("Ctrl-C in the middle of the second trial: exit status 2", p.returncode == 2, "rc=%s %s" % (p.returncode, err))
    h5 = h5_of(d)
    f = h5py.File(h5, "r")
    t0, t1 = f["trial_0000"], f["trial_0001"]
    n1 = t1["ai/P1"].shape[0]
    check("the file is closed properly and the first trial is complete",
          t0.attrs["incomplete"] == 0 and t0["ai/P1"].shape == (20000,) and t0.attrs["recorded"] == 20000)
    check("the interrupted trial is KEPT, marked incomplete, with exactly the samples it had",
          t1.attrs["incomplete"] == 1 and 0 < n1 < 20000 and n1 == t1.attrs["recorded"] and n1 % 2000 == 0 and
          t1["ao/P1"].shape == (20000,), (t1.attrs["incomplete"], n1, t1.attrs["recorded"]))
    check("no third trial was started", "trial_0002" not in f, list(f))
    f.close()
    fo = folder_of(h5)
    log = open(os.path.join(fo, "log.txt")).read()
    check("the log says ABORTED and marks the trial INCOMPLETE", "end  ABORTED: 1 of 3" in log and "INCOMPLETE" in log, log)
    lines, bad = verify_sums(d, fo)
    check("the integrity record is written after an abort, and every digest is right", not bad and lines, bad)
    check("the program says where the data is", "the data so far is in" in err and "aborted after 1 of 3" in err, err)

    # the end of input while waiting for Enter: nothing was played, but the record exists
    d2 = newdir(tmp, "eof")
    waiting = os.path.join(tmp, "wait.sg")
    open(waiting, "w").write("sg 2 protocol\nrepeat 2\nstart on trigger\nstimulus {\n channel P1 unit=pA { 1s dc(1) }\n}\n")
    r = go([LCG2, "-c", CFG, "-r", "20kHz", waiting], d2, stdin="")
    h5 = h5_of(d2)
    f = h5py.File(h5, "r")
    check("end of input at the Enter prompt: status 2, a valid file with no trial, a record with the digests",
          r.returncode == 2 and not [k for k in f if k.startswith("trial_")] and
          not verify_sums(d2, folder_of(h5))[1] and "ABORTED" in open(os.path.join(folder_of(h5), "log.txt")).read(),
          r.stderr)
    f.close()


# --------------------------------------------------------------------------
# 8. SIGKILL keeps the completed trials, and a full disk is survived
# --------------------------------------------------------------------------
def test_kill(tmp):
    import random
    rnd = random.Random(11)
    proto = os.path.join(tmp, "kill.sg")
    open(proto, "w").write("sg 2 protocol\nrepeat 10\nperiod 0.4s\nstimulus {\n channel P1 unit=pA { 0.3s dc(100) }\n}\n")
    unreadable, bad, groups = 0, 0, []
    runs = 10
    for i in range(runs):
        d = newdir(tmp, "kill%d" % i)
        p = subprocess.Popen([LCG2, "-R", "-c", CFG, "-r", "20kHz", proto], cwd=d, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        time.sleep(rnd.uniform(0.2, 2.6))
        p.send_signal(signal.SIGKILL)
        p.wait()
        h5 = h5_of(d)
        if not h5:
            continue
        try:
            f = h5py.File(h5, "r")
            names = sorted(k for k in f if k.startswith("trial_"))
            groups.append(len(names))
            for n in names:
                if f[n].attrs["incomplete"] == 0 and f[n]["ai/P1"].shape != (6000,):
                    bad += 1
            f.close()
        except Exception as e:             # noqa
            unreadable += 1
            print("      unreadable:", str(e)[:100])
    # HDF5 promises nothing about a process killed in the middle of a write.  Measured on this
    # machine: 1 unreadable file in 120 kills of the chosen format (0 in 164 of the library
    # default): a kill that lands in the few milliseconds of the per-trial flush can damage the
    # file.  So the test demands "at most one in ten", which a real regression (an unflushed
    # file, a corrupt layout) would break, and which luck will not.
    check("SIGKILL at %d random moments: at most one file is unreadable (measured: about 1 in 120)" % runs,
          unreadable <= 1, "unreadable: %d" % unreadable)
    check("...and every trial that is there as complete has all its samples", bad == 0, "bad trials: %d" % bad)
    check("...the runs had got some way (the test is not vacuous)", max(groups or [0]) >= 2, groups)


def test_disk_full(tmp):
    import resource
    d = newdir(tmp, "full")
    rest_cfg = os.path.join(tmp, "restcfg.ini")
    open(rest_cfg, "w").write(open(CFG).read().replace("[out P2]\nline = ao1", "[out P2]\nline = ao1\nrest = 100"))
    proto = os.path.join(tmp, "five.sg")
    open(proto, "w").write("sg 2 protocol\nrepeat 5\nperiod 5s\nstimulus {\n channel P1 unit=pA rest=50 { 2s dc(100) }\n}\n")

    def limit():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)                 # a write past the limit fails with EFBIG
        resource.setrlimit(resource.RLIMIT_FSIZE, (1_700_000, 1_700_000))

    r = go([LCG2, "-v", "-c", rest_cfg, "-s", "1", "-r", "20kHz", proto], d, preexec_fn=limit)
    check("a full disk (file size limit) ends the run with status 2 and a message that names the data file",
          r.returncode == 2 and "data file" in r.stderr and "cannot" in r.stderr, "rc=%s %s" % (r.returncode, r.stderr[-500:]))
    check("...the outputs were put at rest (0.125 V on the named channel, 0.25 V on the idle one)",
          "outputs left at ao0=0.125 V ao1=0.25 V" in r.stderr, r.stderr[-300:])
    h5 = h5_of(d)
    try:
        f = h5py.File(h5, "r")
    except Exception as e:                                     # noqa
        check("...the data file can still be opened", False, e)
        return
    done, broken = [], []
    for k in sorted(x for x in f if x.startswith("trial_")):
        try:
            if f[k].attrs["incomplete"] == 0 and f[k]["ai/P1"].shape == (40000,):
                done.append(k)
            else:
                broken.append(k + " (incomplete)")
        except Exception:                                      # noqa
            broken.append(k + " (unreadable)")
    f.close()
    check("...the trials completed before the disk filled are all readable, whole",
          len(done) >= 1 and done == ["trial_%04d" % i for i in range(len(done))], (done, broken))
    check("...at most the one trial whose write failed is damaged (HDF5 cannot do better), and it is the last",
          len(broken) <= 1 and (not broken or broken[0].startswith("trial_%04d" % len(done))), (done, broken))
    check("...the program says where the data so far is", "the data so far is in" in r.stderr, r.stderr[-300:])


# --------------------------------------------------------------------------
# 9. HDF5 never prints on its own
# --------------------------------------------------------------------------
def test_hdf5_silent(tmp):
    d = newdir(tmp, "silent")
    r = go([RECORDTEST, d], d)
    lines = r.stdout.splitlines()
    check("HDF5 prints nothing on stderr when a call fails", r.stderr == "", r.stderr[:300])
    check("creating a file that exists is reported with the root cause and the file name",
          lines and lines[0].startswith("1 cannot create the file in the data file '") and "existing.h5" in lines[0] and
          "HDF5:" in lines[0], lines[:1])
    check("beginning a trial whose group exists is reported (the name already exists)",
          any(l.startswith("2 cannot create the group trial_0000") and "name already exists" in l for l in lines), lines)
    check("closing leaves no HDF5 object open", "3 closed cleanly" in lines, lines)


# --------------------------------------------------------------------------
def main():
    for tool in (RUNTEST, RECORDTEST, LCG2, SG):
        if not os.path.exists(tool):
            print("missing %s: run `make` first" % os.path.relpath(tool, ROOT))
            return 1
    tmp = tempfile.mkdtemp(prefix="lcg2_record_")
    print("(h5py %s, HDF5 %s)" % (h5py.__version__, h5py.version.hdf5_version))
    try:
        for fn in (test_layout, test_variants, test_record_only, test_folder, test_names, test_setup, test_space, test_reader, test_abort,
                   test_kill, test_disk_full, test_hdf5_silent):
            sub = newdir(tmp, fn.__name__)
            fn(sub)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("recording tests: %d checks, %d failed" % (count, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
