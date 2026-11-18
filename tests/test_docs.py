#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# Runs the commands shown in the documents, so that the tutorials stay true:
#
#     python3 tests/test_docs.py ./lcg2
#
# A command is a line, inside a fenced block of README.md or docs/*.md, that
# starts with "$ ./lcg2 ". Optionally "   # exit N" at its end says which exit
# status is expected (default 0). The command is run from the top folder.
#
# Every command is run (a run plays on the simulator, in virtual time, so it takes
# no time), provided that every file it names with -c, and its stimulus file,
# exist. A command that names a file that does not exist (such as rig.ini in an
# illustration) is an illustration and is skipped, not failed. Commands that wait
# for the user (start on trigger) do not appear in the documents. The number
# skipped is printed.
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import neuronpy  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LCG2 = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "lcg2"))

pages = [os.path.join(ROOT, "README.md")] + sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")))
run = skipped = failed = 0

# A real run leaves a data file and a run folder in its working folder, so the commands run
# in a scratch folder (and paths of the repository are made absolute).
WORK = tempfile.mkdtemp(prefix="lcg2_docs_work_")


def absolute(argv):
    return [os.path.join(ROOT, a) if (a and not a.startswith("-") and os.path.exists(os.path.join(ROOT, a)))
            else a for a in argv]


def needs_missing_neuron(files):
    """A configuration with device = neuron needs a Python that has NEURON (the examples say python3; see tests/neuronpy.py);
    if there is none the command is an illustration on this machine (tests/test_neuron.py is the test of that backend)."""
    for f in files:
        if f.endswith(".ini") and re.search(r"^device\s*=\s*neuron", open(os.path.join(ROOT, f)).read(), re.M) and not neuronpy.PY:
            print("      (skipped a command with %s: no Python with NEURON was found)" % f)
            return True
    return False


for page in pages:
    if not os.path.exists(page):
        continue
    in_block = False
    for n, line in enumerate(open(page, encoding="utf-8"), 1):
        if line.lstrip().startswith("```"):
            in_block = not in_block
            continue
        m = re.match(r"\s*\$ \./lcg2 (.*?)(?:\s+# exit (\d+))?\s*$", line)
        if not in_block or not m:
            continue
        expected = int(m.group(2)) if m.group(2) else 0
        argv = shlex.split(m.group(1))
        if "-R" in argv:
            skipped += 1            # real time: the test must not sleep
            continue
        # every file named must exist: otherwise it is an illustration
        files = [argv[i + 1] for i, a in enumerate(argv[:-1]) if a == "-c"]
        positional = [a for i, a in enumerate(argv)
                      if not a.startswith("-") and (i == 0 or argv[i - 1] not in
                      ("-c", "-P", "-m", "-r", "-s", "-T", "-t", "-e", "-p"))]
        files += positional
        if any(not os.path.exists(os.path.join(ROOT, f)) for f in files):
            skipped += 1
            continue
        if needs_missing_neuron(files):
            skipped += 1
            continue
        p = subprocess.run([LCG2] + absolute(argv), cwd=WORK, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, universal_newlines=True, timeout=120, env=neuronpy.env())
        run += 1
        if p.returncode != expected:
            failed += 1
            print("FAIL  %s:%d  ./lcg2 %s" % (os.path.relpath(page, ROOT), n, m.group(1)))
            print("      exit status %d, expected %d" % (p.returncode, expected))
            print("      stderr: %s" % p.stderr.strip()[:200])

shutil.rmtree(WORK, ignore_errors=True)
print("documents: %d commands run, %d illustrations skipped, %d failed"
      % (run, skipped, failed))
sys.exit(1 if failed else 0)
